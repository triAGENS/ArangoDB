////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2020 ArangoDB GmbH, Cologne, Germany
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
/// @author Andrey Abramov
/// @author Vasiliy Nabatchikov
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include "IResearch/IResearchLink.h"

#include "Indexes/Index.h"
#include "Indexes/IndexFactory.h"
#include "VocBase/Identifiers/IndexId.h"

namespace arangodb {

struct IndexTypeFactory;  // forward declaration
}

namespace arangodb {
namespace iresearch {

class IResearchLinkMock final : public arangodb::Index, public IResearchLink {
 public:
  IResearchLinkMock(IndexId iid, arangodb::LogicalCollection& collection);

  [[nodiscard]] static auto setCallbackForScope(
      std::function<irs::directory_attributes()> const& callback) {
    InitCallback = callback;
    return irs::Finally([]() noexcept { InitCallback = nullptr; });
  }

  bool canBeDropped() const override { return IResearchLink::canBeDropped(); }

  arangodb::Result drop() override { return IResearchLink::drop(); }

  bool hasSelectivityEstimate() const override {
    return IResearchLink::hasSelectivityEstimate();
  }

  arangodb::Result insert(arangodb::transaction::Methods& trx,
                          arangodb::LocalDocumentId const& documentId,
                          arangodb::velocypack::Slice const doc) {
    return IResearchDataStore::insert<FieldIterator<FieldMeta>,
                                      IResearchLinkMeta>(trx, documentId, doc,
                                                         meta(), nullptr);
  }

  arangodb::Result insertInRecovery(arangodb::transaction::Methods& trx,
                                    arangodb::LocalDocumentId const& documentId,
                                    arangodb::velocypack::Slice doc,
                                    uint64_t tick) {
    return IResearchDataStore::insert<FieldIterator<FieldMeta>,
                                      IResearchLinkMeta>(trx, documentId, doc,
                                                         meta(), &tick);
  }

  bool isSorted() const override { return IResearchLink::isSorted(); }

  bool isHidden() const override { return IResearchLink::isHidden(); }

  bool needsReversal() const override { return true; }

  void load() override { IResearchLink::load(); }

  bool matchesDefinition(
      arangodb::velocypack::Slice const& slice) const override {
    return IResearchLink::matchesDefinition(slice);
  }

  size_t memory() const override {
    // FIXME return in memory size
    return stats().indexSize;
  }

  ////////////////////////////////////////////////////////////////////////////////
  /// @brief fill and return a JSON description of a IResearchLink object
  /// @param withFigures output 'figures' section with e.g. memory size
  ////////////////////////////////////////////////////////////////////////////////
  using Index::toVelocyPack;  // for std::shared_ptr<Builder>
                              // Index::toVelocyPack(bool, Index::Serialize)
  void toVelocyPack(
      arangodb::velocypack::Builder& builder,
      std::underlying_type<arangodb::Index::Serialize>::type) const override;

  void toVelocyPackFigures(velocypack::Builder& builder) const final {
    IResearchDataStore::toVelocyPackStats(builder);
  }

  IndexType type() const override { return IResearchLink::type(); }

  char const* typeName() const override { return IResearchLink::typeName(); }

  void unload() override {
    auto res = IResearchLink::unload();

    if (!res.ok()) {
      THROW_ARANGO_EXCEPTION(res);
    }
  }

  static std::function<irs::directory_attributes()> InitCallback;
};

}  // namespace iresearch
}  // namespace arangodb
