////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2021 ArangoDB GmbH, Cologne, Germany
/// Copyright 2004-2014 triAGENS GmbH, Cologne, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is ArangoDB GmbH, Cologne, Germany
///
/// @author Daniel Larkin-York
////////////////////////////////////////////////////////////////////////////////

#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <rocksdb/db.h>

#include <velocypack/Builder.h>
#include <velocypack/Slice.h>

#include "IResearch/IResearchRocksDBRecoveryHelper.h"

#include "Basics/Exceptions.h"
#include "Basics/Result.h"
#include "Basics/debugging.h"
#include "Basics/error.h"
#include "IResearch/IResearchCommon.h"
#include "IResearch/IResearchLinkHelper.h"
#include "IResearch/IResearchRocksDBLink.h"
#include "Logger/LogMacros.h"
#include "Logger/Logger.h"
#include "Logger/LoggerStream.h"
#include "RestServer/DatabaseFeature.h"
#include "RocksDBEngine/RocksDBColumnFamilyManager.h"
#include "RocksDBEngine/RocksDBEngine.h"
#include "RocksDBEngine/RocksDBKey.h"
#include "RocksDBEngine/RocksDBLogValue.h"
#include "RocksDBEngine/RocksDBTypes.h"
#include "RocksDBEngine/RocksDBValue.h"
#include "StorageEngine/EngineSelectorFeature.h"
#include "Transaction/StandaloneContext.h"
#include "Utils/SingleCollectionTransaction.h"
#include "VocBase/AccessMode.h"
#include "VocBase/LogicalCollection.h"
#include "VocBase/vocbase.h"

namespace arangodb::transaction {
class Context;
}

namespace {

std::shared_ptr<arangodb::LogicalCollection> lookupCollection(
    arangodb::DatabaseFeature& db, arangodb::RocksDBEngine& engine,
    uint64_t objectId) {
  auto pair = engine.mapObjectToCollection(objectId);
  auto vocbase = db.useDatabase(pair.first);

  return vocbase ? vocbase->lookupCollection(pair.second) : nullptr;
}

}  // namespace

namespace arangodb {
namespace iresearch {

IResearchRocksDBRecoveryHelper::IResearchRocksDBRecoveryHelper(
    application_features::ApplicationServer& server,
    std::vector<std::string> const& skipRecoveryItems)
    : _server(server), _skipAllItems(false) {
  for (auto const& item : skipRecoveryItems) {
    if (item == "all") {
      _skipAllItems = true;
      _skipRecoveryItems.clear();
      break;
    }

    auto parts = basics::StringUtils::split(item, '/');
    TRI_ASSERT(parts.size() == 2);
    // look for collection part
    auto it = _skipRecoveryItems.find(parts[0]);
    if (it == _skipRecoveryItems.end()) {
      // collection not found, insert new set into map with the index id/name
      _skipRecoveryItems.emplace(
          parts[0], containers::FlatHashSet<std::string>{parts[1]});
    } else {
      // collection found. append index/name to existing set
      it->second.emplace(parts[1]);
    }
  }
}

void IResearchRocksDBRecoveryHelper::prepare() {
  _dbFeature = &_server.getFeature<DatabaseFeature>();
  _engine =
      &_server.getFeature<EngineSelectorFeature>().engine<RocksDBEngine>();
  _documentCF = RocksDBColumnFamilyManager::get(
                    RocksDBColumnFamilyManager::Family::Documents)
                    ->GetID();
}

void IResearchRocksDBRecoveryHelper::PutCF(uint32_t column_family_id,
                                           const rocksdb::Slice& key,
                                           const rocksdb::Slice& value,
                                           rocksdb::SequenceNumber tick) {
  if (column_family_id != _documentCF) {
    return;
  }

  auto coll =
      lookupCollection(*_dbFeature, *_engine, RocksDBKey::objectId(key));

  if (coll == nullptr) {
    return;
  }

  LinkContainer::allocator_type::arena_type arena;
  LinkContainer links{arena};
  auto mustReplay = lookupLinks(links, *coll);

  if (links.empty()) {
    // no links found. nothing to do
    TRI_ASSERT(!mustReplay);
    return;
  }

  if (!mustReplay) {
    // links found, but the recovery for all of them will be skipped.
    // so we need to mark all the links as out-of-sync
    for (auto const& link : links) {
      TRI_ASSERT(link.second);
      _skippedIndexes.emplace(link.first->id());
    }
    return;
  }

  auto docId = RocksDBKey::documentId(key);
  auto doc = RocksDBValue::data(value);

  transaction::StandaloneContext ctx(coll->vocbase());

  // FIXME: check ticks range and possibly omit this step
  containers::FlatHashSet<IndexId> skipInsert;
  mustReplay = false;
  {
    for (auto const& link : links) {
      if (link.second) {
        // link excluded from recovery
        _skippedIndexes.emplace(link.first->id());
      } else {
        // link participates in recovery
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
        IResearchLink& impl =
            dynamic_cast<IResearchRocksDBLink&>(*(link.first));
#else
        IResearchLink& impl = static_cast<IResearchRocksDBLink&>(*(link.first));
#endif
        auto snapshotCookie = _cookies.find(link.first.get());
        if (snapshotCookie == _cookies.end()) {
          snapshotCookie =
              _cookies.emplace(link.first.get(), impl.snapshot()).first;
        }
        if (impl.exists(snapshotCookie->second, docId, &tick)) {
          skipInsert.emplace(link.first->id());
        } else {
          mustReplay = true;
        }
      }
    }
  }

  if (!mustReplay) {
    return;
  }

  SingleCollectionTransaction trx(std::shared_ptr<transaction::Context>(
                                      std::shared_ptr<transaction::Context>(),
                                      &ctx),  // aliasing ctor
                                  *coll, arangodb::AccessMode::Type::WRITE);

  Result res = trx.begin();

  if (res.fail()) {
    THROW_ARANGO_EXCEPTION(res);
  }

  for (auto const& link : links) {
    if (link.second) {
      // link excluded from recovery
      _skippedIndexes.emplace(link.first->id());
    } else {
      if (skipInsert.find(link.first->id()) != skipInsert.end()) {
        continue;
      }
      // link participates in recovery
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
      IResearchLink& impl = dynamic_cast<IResearchRocksDBLink&>(*(link.first));
#else
      IResearchLink& impl = static_cast<IResearchRocksDBLink&>(*(link.first));
#endif

      impl.insert(trx, docId, doc, &tick);
    }
  }

  res = trx.commit();

  if (res.fail()) {
    THROW_ARANGO_EXCEPTION(res);
  }
}

// common implementation for DeleteCF / SingleDeleteCF
void IResearchRocksDBRecoveryHelper::handleDeleteCF(
    uint32_t column_family_id, const rocksdb::Slice& key,
    rocksdb::SequenceNumber tick) {
  if (column_family_id != _documentCF) {
    return;
  }

  auto coll =
      lookupCollection(*_dbFeature, *_engine, RocksDBKey::objectId(key));

  if (coll == nullptr) {
    return;
  }

  LinkContainer::allocator_type::arena_type arena;
  LinkContainer links{arena};
  auto mustReplay = lookupLinks(links, *coll);

  if (links.empty()) {
    // no links found. nothing to do
    TRI_ASSERT(!mustReplay);
    return;
  }

  if (!mustReplay) {
    // links found, but the recovery for all of them will be skipped.
    // so we need to mark all the links as out of sync
    for (auto const& link : links) {
      TRI_ASSERT(link.second);
      _skippedIndexes.emplace(link.first->id());
    }
    return;
  }

  auto docId = RocksDBKey::documentId(key);

  transaction::StandaloneContext ctx(coll->vocbase());

  SingleCollectionTransaction trx(std::shared_ptr<transaction::Context>(
                                      std::shared_ptr<transaction::Context>(),
                                      &ctx),  // aliasing ctor
                                  *coll, arangodb::AccessMode::Type::WRITE);

  Result res = trx.begin();

  if (res.fail()) {
    THROW_ARANGO_EXCEPTION(res);
  }

  for (auto const& link : links) {
    if (link.second) {
      // link excluded from recovery
      _skippedIndexes.emplace(link.first->id());
    } else {
      // link participates in recovery
      TRI_ASSERT(link.first->type() ==
                 arangodb::Index::IndexType::TRI_IDX_TYPE_IRESEARCH_LINK);
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
      IResearchLink& impl = dynamic_cast<IResearchRocksDBLink&>(*(link.first));
#else
      IResearchLink& impl = static_cast<IResearchRocksDBLink&>(*(link.first));
#endif
      impl.remove(trx, docId, arangodb::velocypack::Slice::emptyObjectSlice(),
                  &tick);
    }
  }

  res = trx.commit();

  if (res.fail()) {
    THROW_ARANGO_EXCEPTION(res);
  }
}

void IResearchRocksDBRecoveryHelper::LogData(const rocksdb::Slice& blob,
                                             rocksdb::SequenceNumber tick) {
  RocksDBLogType const type = RocksDBLogValue::type(blob);

  switch (type) {
    case RocksDBLogType::IndexCreate: {
      // Intentional NOOP. Index is committed upon creation.
      // So if this marker was written  - index was persisted already.
      break;
    }
    case RocksDBLogType::CollectionTruncate: {
      TRI_ASSERT(_dbFeature);
      TRI_ASSERT(_engine);
      uint64_t objectId = RocksDBLogValue::objectId(blob);
      auto coll = lookupCollection(*_dbFeature, *_engine, objectId);

      if (coll == nullptr) {
        return;
      }

      LinkContainer::allocator_type::arena_type arena;
      LinkContainer links{arena};
      auto mustReplay = lookupLinks(links, *coll);

      if (links.empty()) {
        // no links found. nothing to do
        TRI_ASSERT(!mustReplay);
        return;
      }

      for (auto const& link : links) {
        if (link.second) {
          _skippedIndexes.emplace(link.first->id());
        } else {
          link.first->afterTruncate(tick, nullptr);
        }
      }
      break;
    }
    default:
      break;  // shut up the compiler
  }
}

bool IResearchRocksDBRecoveryHelper::lookupLinks(
    LinkContainer& result, arangodb::LogicalCollection& coll) const {
  TRI_ASSERT(result.empty());

  bool mustReplay = false;

  auto indexes = coll.getIndexes();

  // filter out non iresearch links
  const auto it = std::remove_if(
      indexes.begin(), indexes.end(),
      [](std::shared_ptr<arangodb::Index> const& idx) {
        return idx->type() !=
               arangodb::Index::IndexType::TRI_IDX_TYPE_IRESEARCH_LINK;
      });
  indexes.erase(it, indexes.end());

  result.reserve(indexes.size());
  for (auto& index : indexes) {
    bool mustFail = _skipAllItems;
    if (!mustFail && !_skipRecoveryItems.empty()) {
      if (auto it = _skipRecoveryItems.find(coll.name());
          it != _skipRecoveryItems.end()) {
        mustFail = it->second.contains(index->name()) ||
                   it->second.contains(std::to_string(index->id().id()));
      }
    }
    result.emplace_back(std::make_pair(std::move(index), mustFail));
    if (!mustFail) {
      mustReplay = true;
    }
  }

  return mustReplay;
}

}  // namespace iresearch
}  // namespace arangodb

// -----------------------------------------------------------------------------
// --SECTION--                                                       END-OF-FILE
// -----------------------------------------------------------------------------
