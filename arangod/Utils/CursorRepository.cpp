////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2024 ArangoDB GmbH, Cologne, Germany
/// Copyright 2004-2014 triAGENS GmbH, Cologne, Germany
///
/// Licensed under the Business Source License 1.1 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     https://github.com/arangodb/arangodb/blob/devel/LICENSE
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is ArangoDB GmbH, Cologne, Germany
///
/// @author Jan Steemann
////////////////////////////////////////////////////////////////////////////////

#include "CursorRepository.h"

#include "ApplicationFeatures/ApplicationServer.h"
#include "Aql/Query.h"
#include "Aql/QueryCursor.h"
#include "Aql/QueryResult.h"
#include "Basics/ScopeGuard.h"
#include "Basics/system-functions.h"
#include "Cluster/ServerState.h"
#include "Containers/SmallVector.h"
#include "Logger/LogMacros.h"
#include "Logger/Logger.h"
#include "Logger/LoggerStream.h"
#include "Metrics/Gauge.h"
#include "RestServer/SoftShutdownFeature.h"
#include "Utils/ExecContext.h"
#include "VocBase/ticks.h"
#include "VocBase/vocbase.h"

#include <velocypack/Builder.h>

namespace {
bool authorized(std::pair<arangodb::Cursor*, std::string> const& cursor) {
  auto const& exec = arangodb::ExecContext::current();
  if (exec.isSuperuser()) {
    return true;
  }
  return (cursor.second == exec.user());
}
}  // namespace

using namespace arangodb;

////////////////////////////////////////////////////////////////////////////////
/// @brief create a cursor repository
////////////////////////////////////////////////////////////////////////////////

CursorRepository::CursorRepository(
    TRI_vocbase_t& vocbase, metrics::Gauge<uint64_t>* numberOfCursorsMetric,
    metrics::Gauge<uint64_t>* memoryUsageMetric)
    : _vocbase(vocbase),
      _softShutdownOngoing(nullptr),
      _numberOfCursorsMetric(numberOfCursorsMetric),
      _memoryUsageMetric(memoryUsageMetric) {
  if (ServerState::instance()->isCoordinator()) {
    try {
      auto const& softShutdownFeature{
          _vocbase.server().getFeature<SoftShutdownFeature>()};
      auto& softShutdownTracker{softShutdownFeature.softShutdownTracker()};
      _softShutdownOngoing = softShutdownTracker.getSoftShutdownFlag();
    } catch (...) {
      // Ignore problem, happens only in unit tests and at worst, we do
      // not have access to the flag.
    }
  }
}

////////////////////////////////////////////////////////////////////////////////
/// @brief destroy a cursor repository
////////////////////////////////////////////////////////////////////////////////

CursorRepository::~CursorRepository() {
  try {
    garbageCollect(true);
  } catch (...) {
  }

  // wait until all used cursors have vanished
  int tries = 0;

  while (true) {
    if (!containsUsedCursor()) {
      break;
    }

    if (tries == 0) {
      LOG_TOPIC("4596e", INFO, arangodb::Logger::FIXME)
          << "waiting for used cursors to become unused";
    } else if (tries == 120) {
      LOG_TOPIC("033f6", WARN, arangodb::Logger::FIXME)
          << "giving up waiting for unused cursors";
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    ++tries;
  }

  size_t n = 0;
  uint64_t memoryUsage = 0;
  {
    std::lock_guard mutexLocker{_lock};

    for (auto it : _cursors) {
      memoryUsage += it.second.first->memoryUsage();
      delete it.second.first;
    }

    n = _cursors.size();

    _cursors.clear();
  }

  decreaseNumberOfCursorsMetric(n);
  decreaseMemoryUsageMetric(memoryUsage);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief stores a cursor in the registry
/// the repository will take ownership of the cursor
////////////////////////////////////////////////////////////////////////////////

Cursor* CursorRepository::addCursor(std::unique_ptr<Cursor> cursor) {
  TRI_ASSERT(cursor != nullptr);
  TRI_ASSERT(cursor->isUsed());

  CursorId id = cursor->id();
  std::string user = ExecContext::current().user();

  {
    std::lock_guard mutexLocker{_lock};
    _cursors.emplace(id, std::make_pair(cursor.get(), std::move(user)));
  }

  increaseNumberOfCursorsMetric(1);
  increaseMemoryUsageMetric(cursor->memoryUsage());

  TRI_IF_FAILURE(
      "CursorRepository::directKillStreamQueryAfterCursorIsBeingCreated") {
    cursor->debugKillQuery();
  }

  return cursor.release();
}

////////////////////////////////////////////////////////////////////////////////
/// @brief creates a cursor and stores it in the registry
/// the cursor will be returned with the usage flag set to true. it must be
/// returned later using release()
/// the cursor will take ownership and retain the entire QueryResult object
////////////////////////////////////////////////////////////////////////////////

Cursor* CursorRepository::createFromQueryResult(aql::QueryResult&& result,
                                                size_t batchSize, double ttl,
                                                bool hasCount,
                                                bool isRetriable) {
  TRI_ASSERT(result.data != nullptr);

  if (_softShutdownOngoing != nullptr &&
      _softShutdownOngoing->load(std::memory_order_relaxed)) {
    // Refuse to create the cursor:
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_SHUTTING_DOWN,
                                   "Coordinator soft shutdown ongoing.");
  }

  auto cursor = std::make_unique<aql::QueryResultCursor>(
      _vocbase, std::move(result), batchSize, ttl, hasCount, isRetriable);
  cursor->use();

  return addCursor(std::move(cursor));
}

//////////////////////////////////////////////////////////////////////////////
/// @brief creates a cursor and stores it in the registry
/// the cursor will be returned with the usage flag set to true. it must be
/// returned later using release()
/// the cursor will create a query internally and retain it until deleted
//////////////////////////////////////////////////////////////////////////////

Cursor* CursorRepository::createQueryStream(
    std::shared_ptr<arangodb::aql::Query> q, size_t batchSize, double ttl,
    bool isRetriable, transaction::OperationOrigin operationOrigin,
    std::shared_ptr<aql::QueryAborter> aborter) {
  TRI_ASSERT(q->queryOptions().stream);

  if (_softShutdownOngoing != nullptr &&
      _softShutdownOngoing->load(std::memory_order_relaxed)) {
    // Refuse to create the cursor:
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_SHUTTING_DOWN,
                                   "Coordinator soft shutdown ongoing.");
  }

  auto cursor = std::make_unique<aql::QueryStreamCursor>(
      std::move(q), batchSize, ttl, isRetriable, operationOrigin, aborter);
  cursor->use();

  return addCursor(std::move(cursor));
}

////////////////////////////////////////////////////////////////////////////////
/// @brief remove a cursor by id
////////////////////////////////////////////////////////////////////////////////

bool CursorRepository::remove(CursorId id) {
  arangodb::Cursor* cursor = nullptr;
  uint64_t memoryUsage = 0;

  {
    std::lock_guard mutexLocker{_lock};

    auto it = _cursors.find(id);
    if (it == _cursors.end() || !::authorized(it->second)) {
      // not found
      return false;
    }

    cursor = (*it).second.first;

    if (cursor->isUsed()) {
      // cursor is in use by someone else. now mark as deleted
      cursor->setDeleted();
      return true;
    }

    // cursor not in use by someone else
    memoryUsage = cursor->memoryUsage();
    _cursors.erase(it);
  }

  decreaseNumberOfCursorsMetric(1);
  decreaseMemoryUsageMetric(memoryUsage);

  TRI_ASSERT(cursor != nullptr);

  delete cursor;
  return true;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief find an existing cursor by id
/// if found, the cursor will be returned with the usage flag set to true.
/// it must be returned later using release()
////////////////////////////////////////////////////////////////////////////////

Cursor* CursorRepository::find(CursorId id, bool& busy) {
  arangodb::Cursor* cursor = nullptr;
  busy = false;

  {
    std::lock_guard mutexLocker{_lock};

    auto it = _cursors.find(id);
    if (it == _cursors.end() || !::authorized(it->second)) {
      // not found
      return nullptr;
    }

    cursor = (*it).second.first;

    if (cursor->isDeleted()) {
      // already deleted
      return nullptr;
    }

    if (cursor->isUsed()) {
      busy = true;
      return nullptr;
    }

    if (cursor->expires() < TRI_microtime()) {
      // cursor has expired already
      return nullptr;
    }

    cursor->use();
  }

  return cursor;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief return a cursor
////////////////////////////////////////////////////////////////////////////////

void CursorRepository::release(Cursor* cursor) {
  uint64_t memoryUsage = 0;
  {
    std::lock_guard mutexLocker{_lock};

    TRI_ASSERT(cursor->isUsed());
    cursor->release();

    if (!cursor->isDeleted()) {
      return;
    }

    memoryUsage = cursor->memoryUsage();
    // remove from the list
    _cursors.erase(cursor->id());
  }

  decreaseNumberOfCursorsMetric(1);
  decreaseMemoryUsageMetric(memoryUsage);

  // and free the cursor
  delete cursor;
}

size_t CursorRepository::count() const {
  std::lock_guard mutexLocker{_lock};
  return _cursors.size();
}

////////////////////////////////////////////////////////////////////////////////
/// @brief whether or not the repository contains a used cursor
////////////////////////////////////////////////////////////////////////////////

bool CursorRepository::containsUsedCursor() const {
  std::lock_guard mutexLocker{_lock};

  for (auto const& it : _cursors) {
    if (it.second.first->isUsed()) {
      return true;
    }
  }

  return false;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief run a garbage collection on the cursors
////////////////////////////////////////////////////////////////////////////////

bool CursorRepository::garbageCollect(bool force) {
  auto const now = TRI_microtime();
  containers::SmallVector<arangodb::Cursor*, 8> found;
  uint64_t memoryUsage = 0;

  try {
    std::lock_guard mutexLocker{_lock};

    for (auto it = _cursors.begin(); it != _cursors.end(); /* no hoisting */) {
      auto cursor = (*it).second.first;

      if (cursor->isUsed()) {
        // must not destroy used cursors
        ++it;
        continue;
      }

      if (force || cursor->expires() < now) {
        cursor->kill();
        cursor->setDeleted();
      }

      if (cursor->isDeleted()) {
        try {
          found.emplace_back(cursor);
          it = _cursors.erase(it);
        } catch (...) {
          // stop iteration
          break;
        }

        if (!force && found.size() >= maxCollectCount) {
          break;
        }
      } else {
        ++it;
      }
    }
  } catch (...) {
    // go on and remove whatever we found so far
  }

  // remove cursors outside the lock
  for (auto it : found) {
    TRI_ASSERT(it != nullptr);
    memoryUsage += it->memoryUsage();
    delete it;
  }

  decreaseNumberOfCursorsMetric(found.size());
  decreaseMemoryUsageMetric(memoryUsage);

  return !found.empty();
}

void CursorRepository::increaseNumberOfCursorsMetric(size_t value) noexcept {
  TRI_ASSERT(value == 1);
  if (_numberOfCursorsMetric != nullptr) {
    _numberOfCursorsMetric->fetch_add(value);
  }
}

void CursorRepository::decreaseNumberOfCursorsMetric(size_t value) noexcept {
  if (_numberOfCursorsMetric != nullptr && value > 0) {
    _numberOfCursorsMetric->fetch_sub(value);
  }
}

void CursorRepository::increaseMemoryUsageMetric(size_t value) noexcept {
  if (_memoryUsageMetric != nullptr && value > 0) {
    _memoryUsageMetric->fetch_add(value);
  }
}

void CursorRepository::decreaseMemoryUsageMetric(size_t value) noexcept {
  if (_memoryUsageMetric != nullptr && value > 0) {
    _memoryUsageMetric->fetch_sub(value);
  }
}
