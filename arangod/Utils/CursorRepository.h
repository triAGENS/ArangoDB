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

#pragma once

#include <mutex>
#include <unordered_map>

#include "Metrics/Fwd.h"
#include "Utils/Cursor.h"
#include "VocBase/voc-types.h"

struct TRI_vocbase_t;

namespace arangodb {

namespace futures {
template<typename T>
class Future;
struct Unit;
}  // namespace futures

namespace velocypack {
class Builder;
}

namespace aql {
class Query;
struct QueryResult;
}  // namespace aql

namespace transaction {
struct OperationOrigin;
}

class CursorRepository {
  CursorRepository(CursorRepository const&) = delete;
  CursorRepository& operator=(CursorRepository const&) = delete;

 public:
  explicit CursorRepository(TRI_vocbase_t& vocbase,
                            metrics::Gauge<uint64_t>* numberOfCursorsMetric,
                            metrics::Gauge<uint64_t>* memoryUsageMetric);

  ~CursorRepository();

  /// @brief creates a cursor and stores it in the registry
  /// the cursor will be returned with the usage flag set to true. it must be
  /// returned later using release()
  /// the cursor will take ownership and retain the entire QueryResult object
  Cursor* createFromQueryResult(aql::QueryResult&& result, size_t batchSize,
                                double ttl, bool hasCount, bool isRetriable);

  /// @brief creates a cursor and stores it in the registry
  /// the cursor will be returned with the usage flag set to true. it must be
  /// returned later using release()
  /// the cursor will create a query internally and retain it until deleted
  futures::Future<Cursor*> createQueryStream(std::shared_ptr<aql::Query> q,
                                             size_t batchSize, double ttl,
                                             bool isRetriable);

  /// @brief remove a cursor by id
  bool remove(CursorId id);

  /// @brief find an existing cursor by id
  /// if found, the cursor will be returned with the usage flag set to true.
  /// it must be returned later using release()
  Cursor* find(CursorId id, bool& found);

  /// @brief return a cursor
  void release(Cursor*);

  /// @brief whether or not the repository contains a used cursor
  bool containsUsedCursor() const;

  /// @brief return the number of cursors
  size_t count() const;

  /// @brief run a garbage collection on the cursors
  bool garbageCollect(bool force);

 private:
  /// @brief stores a cursor in the registry
  /// the repository will take ownership of the cursor
  Cursor* addCursor(std::unique_ptr<Cursor> cursor);

  void increaseNumberOfCursorsMetric(size_t value) noexcept;
  void decreaseNumberOfCursorsMetric(size_t value) noexcept;

  void increaseMemoryUsageMetric(size_t value) noexcept;
  void decreaseMemoryUsageMetric(size_t value) noexcept;

  /// @brief maximum number of cursors to garbage-collect in one go
  static constexpr size_t maxCollectCount = 1024;

  TRI_vocbase_t& _vocbase;

  /// @brief mutex for the cursors repository
  std::mutex mutable _lock;

  /// @brief list of current cursors
  std::unordered_map<CursorId, std::pair<Cursor*, std::string>> _cursors;

  /// @brief flag, if a soft shutdown is ongoing, this is used for the soft
  /// shutdown feature in coordinators, in all other instance types
  /// this pointer is a nullptr and not used.
  std::atomic<bool> const* _softShutdownOngoing;

  /// @brief metric to increase/decrease for the number of cursors.
  /// can be a nullptr in tests
  metrics::Gauge<uint64_t>* _numberOfCursorsMetric;

  /// @brief metric to increase/decrease for the memory usage of cursors.
  /// is only used for non-streaming cursors. streaming cursors already
  /// count their memory against other AQL metrics
  /// can be a nullptr in tests
  metrics::Gauge<uint64_t>* _memoryUsageMetric;
};

}  // namespace arangodb
