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

#include "QueryPlanCache.h"

#include "Aql/QueryOptions.h"
#include "Basics/Exceptions.h"
#include "Basics/VelocyPackHelper.h"
#include "Basics/conversions.h"
#include "Basics/debugging.h"
#include "Basics/system-functions.h"
#include "Logger/Logger.h"
#include "Metrics/Counter.h"
#include "Random/RandomGenerator.h"
#include "VocBase/vocbase.h"

#include <boost/container_hash/hash.hpp>
#include <velocypack/Builder.h>
#include <velocypack/Iterator.h>
#include <velocypack/Options.h>
#include <velocypack/Slice.h>

namespace arangodb::aql {

size_t QueryPlanCache::Key::hash() const {
  return QueryPlanCache::KeyHasher{}(*this);
}

size_t QueryPlanCache::Key::memoryUsage() const noexcept {
  // note: the 256 bytes overhead is a magic number estimated here.
  // we will definitely have some overhead for each entry because
  // strings may have reserved more capacity than actual bytes
  // are used. the velocypack buffer for the bind parameters also
  // may have some overhead because of over-allocation.
  return 256 + queryString.size() + bindParameters->byteSize();
}

size_t QueryPlanCache::KeyHasher::operator()(
    QueryPlanCache::Key const& key) const noexcept {
  size_t hash = 0;
  boost::hash_combine(hash, key.queryString.hash());
  boost::hash_combine(hash,
                      VPackSlice(key.bindParameters->data()).normalizedHash());
  // arbitrary integer values used here from https://hexwords.netlify.app/
  // for fullcount=true / fullcount=false
  boost::hash_combine(hash, key.fullCount ? 0xB16F007 : 0xB0BCA7);
  return hash;
}

bool QueryPlanCache::KeyEqual::operator()(
    QueryPlanCache::Key const& lhs,
    QueryPlanCache::Key const& rhs) const noexcept {
  if (lhs.fullCount != rhs.fullCount) {
    return false;
  }

  if (!lhs.queryString.equal(rhs.queryString)) {
    return false;
  }

  auto lhsSlice = VPackSlice(lhs.bindParameters->data());
  auto rhsSlice = VPackSlice(rhs.bindParameters->data());
  if (lhsSlice.normalizedHash() != rhsSlice.normalizedHash()) {
    return false;
  }
  if (basics::VelocyPackHelper::compare(
          lhsSlice, rhsSlice,
          /*useUtf8*/ true, &VPackOptions::Defaults, nullptr, nullptr) != 0) {
    return false;
  }
  return true;
}

size_t QueryPlanCache::Value::memoryUsage() const noexcept {
  // note: the magic numbers here are estimated.
  // we will definitely have some overhead for each entry because
  // strings may have reserved more capacity than actual bytes
  // are used. the velocypack buffer for the serialized plan also
  // may have some overhead because of over-allocation.
  size_t total = 32;
  for (auto const& it : dataSources) {
    total += 32 + it.first.size() + it.second.name.size();
  }
  total += serializedPlan->size();
  return total;
}

QueryPlanCache::QueryPlanCache(size_t maxEntries, size_t maxMemoryUsage,
                               size_t maxIndividualEntrySize,
                               metrics::Counter* numberOfHitsMetric,
                               metrics::Counter* numberOfMissesMetric)
    : _entries(5, KeyHasher{}, KeyEqual{}),
      _memoryUsage(0),
      _maxEntries(maxEntries),
      _maxMemoryUsage(maxMemoryUsage),
      _maxIndividualEntrySize(maxIndividualEntrySize),
      _numberOfHitsMetric(numberOfHitsMetric),
      _numberOfMissesMetric(numberOfMissesMetric) {}

QueryPlanCache::~QueryPlanCache() {
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
  size_t total = 0;
  for (auto const& it : _entries) {
    total += it.first.memoryUsage() + it.second->memoryUsage();
  }
  TRI_ASSERT(_memoryUsage == total);
#endif
}

std::shared_ptr<QueryPlanCache::Value const> QueryPlanCache::lookup(
    QueryPlanCache::Key const& key) {
  std::shared_lock guard(_mutex);
  if (auto it = _entries.find(key); it != _entries.end()) {
    // increase numUsed counter of current entry.
    // when used more than kMaxNumUsages times, we intentionally wipe the entry
    // from the cache and return a nullptr. this is done so that somehow
    // outdated entries get replaced with fresh entries eventually
    if ((*it).second->numUsed.fetch_add(1, std::memory_order_relaxed) <=
        kMaxNumUsages) {
      if (_numberOfHitsMetric != nullptr) {
        _numberOfHitsMetric->fetch_add(1);
      }
      return (*it).second;
    }
    guard.unlock();

    // write-lock and wipe the entry from the cache
    std::unique_lock wguard(_mutex);
    // need to lock up the most current version of the key, because
    // it may have changed between the original lookup and now.
    if (auto it = _entries.find(key); it != _entries.end()) {
      auto const& value = (*it).second;
      TRI_ASSERT(_memoryUsage >= value->memoryUsage());
      _memoryUsage -= value->memoryUsage();
      _entries.erase(it);
    }
  }

  if (_numberOfMissesMetric != nullptr) {
    _numberOfMissesMetric->fetch_add(1);
  }
  return nullptr;
}

bool QueryPlanCache::store(
    QueryPlanCache::Key&& key,
    std::unordered_map<std::string, DataSourceEntry>&& dataSources,
    std::shared_ptr<velocypack::UInt8Buffer> serializedPlan) {
  auto value = std::make_shared<Value>(
      std::move(dataSources), std::move(serializedPlan), TRI_microtime(), 0);
  size_t memoryUsage = key.memoryUsage() + value->memoryUsage();

  if (memoryUsage > _maxIndividualEntrySize) {
    return false;
  }

  std::unique_lock guard(_mutex);

  // for the sake of memory accouting, we need to perform a lookup for
  // the same cache key first, and subtract its memory usage before
  // we can actually delete it.
  if (auto it = _entries.find(key); it != _entries.end()) {
    _memoryUsage -= (*it).first.memoryUsage() + (*it).second->memoryUsage();
    _entries.erase(it);
  }

  bool inserted = _entries.emplace(std::move(key), std::move(value)).second;
  TRI_ASSERT(inserted);
  _memoryUsage += memoryUsage;

  applySizeConstraints();

  return true;
}

QueryPlanCache::Key QueryPlanCache::createCacheKey(
    QueryString const& queryString,
    std::shared_ptr<velocypack::Builder> const& bindVars,
    QueryOptions const& queryOptions) const {
  return {queryString, filterBindParameters(bindVars), queryOptions.fullCount};
}

void QueryPlanCache::invalidate(std::string const& dataSourceGuid) {
  size_t total = 0;

  std::unique_lock guard(_mutex);

  for (auto it = _entries.begin(); it != _entries.end(); /* no hoisting */) {
    auto const& value = (*it).second;
    if (value->dataSources.contains(dataSourceGuid)) {
      total += value->memoryUsage();
      it = _entries.erase(it);
    } else {
      ++it;
    }
  }

  TRI_ASSERT(_memoryUsage >= total);
  _memoryUsage -= total;
}

void QueryPlanCache::invalidateAll() {
  std::unique_lock guard(_mutex);
  // make sure all memory is acutally freed
  _entries = {};
  _memoryUsage = 0;
}

void QueryPlanCache::toVelocyPack(
    velocypack::Builder& builder,
    std::function<bool(QueryPlanCache::Key const&,
                       QueryPlanCache::Value const&)> const& filter) const {
  std::unique_lock guard(_mutex);

  builder.openArray(true);
  for (auto const& it : _entries) {
    auto const& key = it.first;
    auto const& value = *(it.second);

    if (!filter(key, value)) {
      continue;
    }

    builder.openObject();
    builder.add("hash", VPackValue(std::to_string(key.hash())));
    builder.add("query", VPackValue(key.queryString.string()));
    builder.add("queryHash", VPackValue(key.queryString.hash()));
    if (key.bindParameters == nullptr) {
      builder.add("bindVars", VPackSlice::emptyObjectSlice());
    } else {
      builder.add("bindVars", VPackSlice(key.bindParameters->data()));
    }
    builder.add("fullCount", VPackValue(key.fullCount));

    builder.add("dataSources", VPackValue(VPackValueType::Array));
    for (auto const& ds : value.dataSources) {
      builder.add(VPackValue(ds.second.name));
    }
    builder.close();  // dataSources

    builder.add("created", VPackValue(TRI_StringTimeStamp(
                               value.dateCreated, Logger::getUseLocalTime())));
    builder.add("numUsed",
                VPackValue(value.numUsed.load(std::memory_order_relaxed)));
    builder.add("memoryUsage",
                VPackValue(it.first.memoryUsage() + it.second->memoryUsage()));

    builder.close();
  }
  builder.close();
}

std::shared_ptr<velocypack::UInt8Buffer> QueryPlanCache::filterBindParameters(
    std::shared_ptr<velocypack::Builder> const& source) {
  // extract relevant bind vars from passed bind variables.
  // we intentionally ignore all value bind parameters here
  velocypack::Builder result;
  result.openObject();
  if (source != nullptr) {
    for (auto it : VPackObjectIterator(source->slice())) {
      if (it.key.stringView().starts_with('@')) {
        // collection name bind parameter
        result.add(it.key.stringView(), it.value);
      }
    }
  }
  result.close();

  return result.steal();
}

void QueryPlanCache::applySizeConstraints() {
  while (_entries.size() > _maxEntries || _memoryUsage > _maxMemoryUsage) {
    TRI_ASSERT(!_entries.empty());
    if (_entries.empty()) {
      // this should never be true. if we get here, it indicates a programming
      // error.
      break;
    }

    // pick a "random" entry to evict. for simplicity, we either pick a
    // pseudorandom number between 0 and 63 and skip that many steps in the map.
    // when done with skipping, we have found our target item and evict it. this
    // is not very random, but it is bounded and allows us to get away without
    // maintaining a full LRU list.
    size_t entriesToSkip =
        RandomGenerator::interval(uint32_t(64)) % _entries.size();
    auto it = _entries.begin();
    while (entriesToSkip-- > 0) {
      ++it;
    }
    TRI_ASSERT(it != _entries.end());
    _memoryUsage -= (*it).first.memoryUsage() + (*it).second->memoryUsage();
    _entries.erase(it);
  }
}

}  // namespace arangodb::aql
