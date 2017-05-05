////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2016 ArangoDB GmbH, Cologne, Germany
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
/// @author Jan Steemann
////////////////////////////////////////////////////////////////////////////////

#ifndef ARANGOD_ROCKSDB_ENGINE_ROCKSDB_INDEX_H
#define ARANGOD_ROCKSDB_ENGINE_ROCKSDB_INDEX_H 1

#include "Basics/AttributeNameParser.h"
#include "Basics/Common.h"
#include "Indexes/Index.h"
#include "RocksDBEngine/RocksDBKeyBounds.h"

namespace rocksdb {
class WriteBatch;
class WriteBatchWithIndex;
}

namespace arangodb {
namespace cache {
class Cache;
}
class LogicalCollection;
class RocksDBComparator;

class RocksDBIndex : public Index {
 protected:

  RocksDBIndex(TRI_idx_iid_t, LogicalCollection*,
               std::vector<std::vector<arangodb::basics::AttributeName>> const&
                   attributes,
               bool unique, bool sparse, uint64_t objectId = 0);

  RocksDBIndex(TRI_idx_iid_t, LogicalCollection*,
               arangodb::velocypack::Slice const&);

 public:
  ~RocksDBIndex();

  uint64_t objectId() const { return _objectId; }

  bool isPersistent() const override final { return true; }

  /// @brief return a VelocyPack representation of the index
  void toVelocyPack(velocypack::Builder& builder, bool withFigures,
                    bool forPersistence) const override;

  int drop() override;

  int unload() override;

  /// @brief provides a size hint for the index
  int sizeHint(transaction::Methods* /*trx*/, size_t /*size*/) override final {
    // nothing to do here
    return TRI_ERROR_NO_ERROR;
  }

  void load();

  /// insert index elements into the specified write batch. Should be used
  /// as an optimization for the non transactional fillIndex method
  virtual int insertRaw(rocksdb::WriteBatchWithIndex*, TRI_voc_rid_t,
                        arangodb::velocypack::Slice const&) = 0;
  
  /// remove index elements and put it in the specified write batch. Should be used
  /// as an optimization for the non transactional fillIndex method
  virtual int removeRaw(rocksdb::WriteBatch*, TRI_voc_rid_t,
                        arangodb::velocypack::Slice const&) = 0;
  
 protected:
  void createCache();
  void disableCache();
  inline bool useCache() const { return (_useCache && _cachePresent); }

 protected:
  uint64_t _objectId;
  RocksDBComparator* _cmp;

  mutable std::shared_ptr<cache::Cache> _cache;
  // we use this boolean for testing whether _cache is set.
  // it's quicker than accessing the shared_ptr each time
  bool _cachePresent;
  bool _useCache;
};
}  // namespace arangodb

#endif
