////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2022 ArangoDB GmbH, Cologne, Germany
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
/// @author Andrei Lobov
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include "Basics/Common.h"
#include "Indexes/Index.h"
#include "Indexes/IndexIterator.h"
#include "Indexes/IndexFactory.h"
#include "IResearchCommon.h"
#include "IResearchDataStore.h"
#include "IResearchInvertedIndexMeta.h"
#include "search/boolean_filter.hpp"
#include "search/bitset_doc_iterator.hpp"
#include "search/score.hpp"
#include "search/conjunction.hpp"
#include "search/cost.hpp"
#include <search/proxy_filter.hpp>

namespace arangodb {
namespace iresearch {

class IResearchInvertedIndex : public IResearchDataStore {
 public:
  explicit IResearchInvertedIndex(IndexId iid, LogicalCollection& collection);

  void toVelocyPack(ArangodServer& server, TRI_vocbase_t const* defaultVocbase,
                    velocypack::Builder& builder,
                    bool writeAnalyzerDefinition) const;

  std::string const& getDbName() const noexcept {
    return _collection.vocbase().name();
  }

  bool isSorted() const { return !_meta._sort.empty(); }

  Result init(VPackSlice definition, bool& pathExists,
              InitCallback const& initCallback = {});

  static std::vector<std::vector<basics::AttributeName>> fields(
      IResearchInvertedIndexMeta const& meta);
  static std::vector<std::vector<basics::AttributeName>> sortedFields(
      IResearchInvertedIndexMeta const& meta);

  bool matchesDefinition(VPackSlice other, TRI_vocbase_t const& vocbase) const;

  AnalyzerPool::ptr findAnalyzer(AnalyzerPool const& analyzer) const override;

  bool inProgress() const { return false; }

  bool covers(aql::Projections& projections) const;

  std::unique_ptr<IndexIterator> iteratorForCondition(
      ResourceMonitor& monitor, LogicalCollection* collection,
      transaction::Methods* trx, aql::AstNode const* node,
      aql::Variable const* reference, IndexIteratorOptions const& opts,
      int mutableConditionIdx);

  Index::SortCosts supportsSortCondition(
      aql::SortCondition const* sortCondition, aql::Variable const* reference,
      size_t itemsInIndex) const;

  Index::FilterCosts supportsFilterCondition(
      transaction::Methods& trx, IndexId id,
      std::vector<std::vector<basics::AttributeName>> const& fields,
      std::vector<std::shared_ptr<Index>> const& allIndexes,
      aql::AstNode const* node, aql::Variable const* reference,
      size_t itemsInIndex) const;

  aql::AstNode* specializeCondition(transaction::Methods& trx,
                                    aql::AstNode* node,
                                    aql::Variable const* reference) const;

  IResearchInvertedIndexMeta const& meta() const noexcept { return _meta; }

  Result updateProperties(VPackSlice slice);

 protected:
  void invalidateQueryCache(TRI_vocbase_t* vocbase) override;

  irs::comparer const* getComparator() const noexcept override {
    return &_comparer;
  }

 private:
  IResearchInvertedIndexMeta _meta;
  VPackComparer<IResearchInvertedIndexSort> _comparer;
};

class IResearchInvertedClusterIndex final : public IResearchInvertedIndex,
                                            public Index {
 public:
  Index::IndexType type() const final {
    return Index::TRI_IDX_TYPE_INVERTED_INDEX;
  }

  ~IResearchInvertedClusterIndex() final {
    // should be in final dtor, otherwise its vtable already destroyed
    unload();
  }

  void toVelocyPack(
      VPackBuilder& builder,
      std::underlying_type<Index::Serialize>::type flags) const final;

  void toVelocyPackFigures(velocypack::Builder& builder) const final {
    IResearchDataStore::toVelocyPackStats(builder);
  }

  std::string getCollectionName() const;

  Stats stats() const final;

  size_t memory() const final { return stats().indexSize; }

  bool isHidden() const final { return false; }

  char const* typeName() const final { return oldtypeName(); }

  bool canBeDropped() const final { return true; }

  bool isSorted() const final { return IResearchInvertedIndex::isSorted(); }

  bool hasSelectivityEstimate() const final { return false; }

  bool inProgress() const final { return IResearchInvertedIndex::inProgress(); }

  bool covers(aql::Projections& projections) const final {
    return IResearchInvertedIndex::covers(projections);
  }

  Result drop() final {
    unload();
    return {};
  }
  void load() final {}
  void unload() final { _asyncSelf->reset(); }

  bool matchesDefinition(velocypack::Slice const& other) const final;

  std::unique_ptr<IndexIterator> iteratorForCondition(
      ResourceMonitor& monitor, transaction::Methods* trx,
      aql::AstNode const* node, aql::Variable const* reference,
      IndexIteratorOptions const& opts, ReadOwnWrites readOwnWrites,
      int mutableConditionIdx) final {
    TRI_ASSERT(readOwnWrites ==
               ReadOwnWrites::no);  // FIXME: check - should we ever care?
    return IResearchInvertedIndex::iteratorForCondition(
        monitor, &IResearchDataStore::collection(), trx, node, reference, opts,
        mutableConditionIdx);
  }

  Index::SortCosts supportsSortCondition(
      aql::SortCondition const* sortCondition, aql::Variable const* reference,
      size_t itemsInIndex) const final {
    return IResearchInvertedIndex::supportsSortCondition(
        sortCondition, reference, itemsInIndex);
  }

  Index::FilterCosts supportsFilterCondition(
      transaction::Methods& trx,
      std::vector<std::shared_ptr<Index>> const& allIndexes,
      aql::AstNode const* node, aql::Variable const* reference,
      size_t itemsInIndex) const final {
    return IResearchInvertedIndex::supportsFilterCondition(
        trx, IResearchDataStore::id(), Index::fields(), allIndexes, node,
        reference, itemsInIndex);
  }

  aql::AstNode* specializeCondition(
      transaction::Methods& trx, aql::AstNode* node,
      aql::Variable const* reference) const final {
    return IResearchInvertedIndex::specializeCondition(trx, node, reference);
  }

  IResearchInvertedClusterIndex(IndexId iid, uint64_t /*objectId*/,
                                LogicalCollection& collection,
                                std::string const& name)
      : IResearchInvertedIndex(iid, collection),
        Index(iid, collection, name, {}, false, true) {
    initClusterMetrics();
  }

  void initFields() {
    TRI_ASSERT(_fields.empty());
    *const_cast<std::vector<std::vector<basics::AttributeName>>*>(&_fields) =
        IResearchInvertedIndex::fields(meta());
  }
};

}  // namespace iresearch
}  // namespace arangodb
