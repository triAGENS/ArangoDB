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
/// @author Andrei Lobov
////////////////////////////////////////////////////////////////////////////////

#include "IResearch/IResearchInvertedIndex.h"
#include "IResearch/AqlHelper.h"
#include "IResearch/IResearchFilterFactory.h"
#include "Basics/AttributeNameParser.h"
#include "Cluster/ServerState.h"
#include "RocksDBEngine/RocksDBColumnFamilyManager.h"


namespace {
using namespace arangodb;
using namespace arangodb::iresearch;

struct CheckFieldsAccess {
  CheckFieldsAccess(QueryContext const& ctx,
                    aql::Variable const& ref,
                    std::vector<std::vector<basics::AttributeName>> const& fields)
    : _ctx(ctx), _ref(ref) {
    _fields.insert(std::begin(fields), std::end(fields));
  }

  bool operator()(std::string_view name) const {
    try {
      _parsed.clear();
      TRI_ParseAttributeString(name, _parsed, false);
      if (_fields.find(_parsed) == _fields.end()) {
        LOG_TOPIC("bf92f", TRACE, arangodb::iresearch::TOPIC)
            << "Attribute '" << name << "' is not covered by index";
        return false;
      }
    } catch (basics::Exception const& ex) {
      // we can`t handle expansion in ArangoSearch index
      LOG_TOPIC("2ec9a", TRACE, arangodb::iresearch::TOPIC)
          << "Failed to parse attribute access: " << ex.message();
      return false;
    }
    return true;
  }

  mutable std::vector<arangodb::basics::AttributeName> _parsed;
  QueryContext const& _ctx;
  aql::Variable const& _ref;
  using atr_ref = std::reference_wrapper<std::vector<basics::AttributeName> const>;
  std::unordered_set<atr_ref,
                     std::hash<std::vector<basics::AttributeName>>,
                     std::equal_to<std::vector<basics::AttributeName>>> _fields;
};

template<bool first>
void traverseFields(
    irs::hashed_string_ref const myName,
    FieldMeta const& meta, std::vector<basics::AttributeName> current, // intentional copy
    std::vector<std::vector<basics::AttributeName>>& total) {
  if constexpr (!first) {
     current.push_back(
         basics::AttributeName(
           velocypack::StringRef(myName.c_str(), myName.size()), false));
  }
  if (meta._fields.empty()) {
    // reached the bottom
    total.push_back(current);
    return;
  }
  for (auto const& f : meta._fields) {
    traverseFields<false>(f.key(),  *f.value().get(), current, total);
  }
}

void traverseMetaFields(
    FieldMeta const& meta,
    std::vector<std::vector<basics::AttributeName>>& total) {
  total.clear();
  if (!meta._fields.empty()) {
    std::vector<basics::AttributeName> current;
    traverseFields<true>(irs::hashed_string_ref(0, irs::string_ref::NIL), meta,  current, total);
  }
}
} // namespace

namespace arangodb {
namespace iresearch {

arangodb::iresearch::IResearchInvertedIndex::IResearchInvertedIndex(
    IResearchInvertedIndexMeta&& meta) : _meta(meta) {
}

void IResearchInvertedIndex::toVelocyPack(
    application_features::ApplicationServer& server,
    TRI_vocbase_t const* defaultVocbase,
    velocypack::Builder& builder,
    bool forPersistence) const {
  if (_meta.json(server, defaultVocbase, builder, forPersistence).fail()) {
    THROW_ARANGO_EXCEPTION(Result(
        TRI_ERROR_INTERNAL,
        std::string("Failed to generate inverted index definition")));
  }
}

std::unique_ptr<IndexIterator> arangodb::iresearch::IResearchInvertedIndex::iteratorForCondition(
    LogicalCollection* collection, transaction::Methods* trx, aql::AstNode const* node, aql::Variable const* reference,
    IndexIteratorOptions const& opts) {
  return std::make_unique<EmptyIndexIterator>(collection, trx);
}

Index::SortCosts IResearchInvertedIndex::supportsSortCondition(
    arangodb::aql::SortCondition const* sortCondition, arangodb::aql::Variable const* reference,
    size_t itemsInIndex) const {
  return Index::SortCosts();
}

Index::FilterCosts IResearchInvertedIndex::supportsFilterCondition(
    IndexId id,
    std::vector<std::vector<arangodb::basics::AttributeName>> const& fields,
    std::vector<std::shared_ptr<arangodb::Index>> const& allIndexes,
    arangodb::aql::AstNode const* node, arangodb::aql::Variable const* reference,
    size_t itemsInIndex) const {
  TRI_ASSERT(node);
  TRI_ASSERT(reference);
  auto filterCosts = Index::FilterCosts::defaultCosts(itemsInIndex);

  // non-deterministic condition will mean full-scan. So we should
  // not use index here.
  // FIXME: maybe in the future we will be able to optimize just deterministic part?
  if (!node->isDeterministic()) {
    LOG_TOPIC("750e6", TRACE, iresearch::TOPIC)
             << "Found non-deterministic condition. Skipping index " << id.id();
    return  filterCosts;
  }

  // We don`t want byExpression filters
  // and can`t apply index if we are not sure what attribute is
  // accessed so we provide QueryContext which is unable to
  // execute expressions and only allow to pass conditions with constant
  // attributes access/values. Otherwise if we have say d[a.smth] where 'a' is a variable from
  // the upstream loop we may get here a field we don`t have in the index.
  QueryContext const queryCtx = {nullptr, nullptr, nullptr,
                                 nullptr, nullptr, reference};


  // check that only covered attributes are referenced
  if (!visitAllAttributeAccess(node, *reference, queryCtx, CheckFieldsAccess(queryCtx, *reference, fields))) {
    LOG_TOPIC("d2beb", TRACE, iresearch::TOPIC)
             << "Found unknown attribute access. Skipping index " << id.id();
    return  filterCosts;
  }

  auto rv = FilterFactory::filter(nullptr, queryCtx, *node, false);
  LOG_TOPIC_IF("ee0f7", TRACE, iresearch::TOPIC, rv.fail())
             << "Failed to build filter with error'" << rv.errorMessage() <<"' Skipping index " << id.id();

  if (rv.ok()) {
    filterCosts.supportsCondition = true;
    filterCosts.coveredAttributes = 0; // FIXME: we may use stored values!
  }
  return filterCosts;
}

aql::AstNode* IResearchInvertedIndex::specializeCondition(aql::AstNode* node,
                                                          aql::Variable const* reference) const {
  return node;
}

IResearchInvertedIndexFactory::IResearchInvertedIndexFactory(application_features::ApplicationServer& server)
    : IndexTypeFactory(server) {
}

bool IResearchInvertedIndexFactory::equal(velocypack::Slice const& lhs,
                                          velocypack::Slice const& rhs,
                                          std::string const& dbname) const {
  return false;
}

std::shared_ptr<Index> IResearchInvertedIndexFactory::instantiate(LogicalCollection& collection,
                                                                  velocypack::Slice const& definition,
                                                                  IndexId id,
                                                                  bool isClusterConstructor) const {
  IResearchInvertedIndexMeta meta;
  // FIXME: for cluster  - where to get actual collection name? Pre-store in definition I guess!
  auto res = meta.init(_server, &collection.vocbase(), definition, isClusterConstructor);
  if (res.fail()) {
        LOG_TOPIC("18c17", ERR, iresearch::TOPIC)
             << "Filed to create index '" << id.id()
             << "' error:" << res.errorMessage();
    return nullptr;
  }
  return std::make_shared<IResearchRocksDBInvertedIndex>(id, collection, std::move(meta));
}

Result IResearchInvertedIndexFactory::normalize(velocypack::Builder& normalized, velocypack::Slice definition,
                                                bool isCreation, TRI_vocbase_t const& vocbase) const {
  TRI_ASSERT(normalized.isOpenObject());
  Result res = IResearchInvertedIndexMeta::normalize(_server, &vocbase, normalized, definition);
  if (res.ok()) {
    normalized.add(arangodb::StaticStrings::IndexType,
                   arangodb::velocypack::Value(arangodb::Index::oldtypeName(
                   arangodb::Index::TRI_IDX_TYPE_INVERTED_INDEX)));

    if (isCreation && !arangodb::ServerState::instance()->isCoordinator() &&
        !definition.hasKey("objectId")) {
        normalized.add("objectId", VPackValue(std::to_string(TRI_NewTickServer())));
    }
    normalized.add(arangodb::StaticStrings::IndexSparse, arangodb::velocypack::Value(true));
    normalized.add(arangodb::StaticStrings::IndexUnique, arangodb::velocypack::Value(false));

    // FIXME: make indexing true background?
    bool bck = basics::VelocyPackHelper::getBooleanValue(
        definition, arangodb::StaticStrings::IndexInBackground, false);
    normalized.add(arangodb::StaticStrings::IndexInBackground, VPackValue(bck));
  }
  return res;
}

Result IResearchInvertedIndexMeta::init(
    application_features::ApplicationServer& server,
    TRI_vocbase_t const* defaultVocbase,
    velocypack::Slice const info, bool isClusterConstructor) {
  std::string errField;
  if (!_indexMeta.init(info, errField)) {
     return arangodb::Result(
        TRI_ERROR_BAD_PARAMETER,
        errField.empty()
        ? (std::string("failed to initialize index from definition: ") + info.toString())
        : (std::string("failed to initialize index from definition, error in attribute '")
          + errField + "': " + info.toString()));
  }
  if (!_fieldsMeta.init(server, info, false, errField,
                        defaultVocbase ? defaultVocbase->name() : irs::string_ref::NIL,
                        IResearchLinkMeta::DEFAULT(), nullptr, true)) {
    return arangodb::Result(
        TRI_ERROR_BAD_PARAMETER,
        errField.empty()
        ? (std::string("failed to initialize index from definition: ") + info.toString())
        : (std::string("failed to initialize index from definition, error in attribute '")
          + errField + "': " + info.toString()));
  }
  auto nameSlice = info.get(arangodb::StaticStrings::IndexName);
  if (nameSlice.isString() && nameSlice.getStringLength() > 0) {
    _name = nameSlice.copyString();
  } else if (!nameSlice.isNone()) {
    return arangodb::Result(
        TRI_ERROR_BAD_PARAMETER,
        std::string("failed to initialize index from definition, error in attribute '")
        + arangodb::StaticStrings::IndexName + "': "
        + info.toString());
  }
  return {};
}

Result IResearchInvertedIndexMeta::normalize(
    application_features::ApplicationServer& server,
    TRI_vocbase_t const* defaultVocbase,
    velocypack::Builder& normalized, velocypack::Slice definition) {
  IResearchViewMeta tmpMeta;
  std::string errField;
  if (!tmpMeta.init(definition, errField)) {
     return arangodb::Result(
        TRI_ERROR_BAD_PARAMETER,
        errField.empty()
        ? (std::string("failed to initialize index from definition: ") + definition.toString())
        : (std::string("failed to initialize index from definition, error in attribute '")
          + errField + "': " + definition.toString()));
  }
  if (!tmpMeta.json(normalized)) {
    return arangodb::Result(
        TRI_ERROR_BAD_PARAMETER,
        std::string("failed to initialize index from definition: ") + definition.toString());
  }
  IResearchLinkMeta tmpLinkMeta;
  if (!tmpLinkMeta.init(server, definition, false, errField,
                        defaultVocbase ? defaultVocbase->name() : irs::string_ref::NIL,
                        IResearchLinkMeta::DEFAULT(), nullptr, true)) {
    return arangodb::Result(
        TRI_ERROR_BAD_PARAMETER,
        errField.empty()
        ? (std::string("failed to initialize index from definition: ") + definition.toString())
        : (std::string("failed to initialize index from definition, error in attribute '")
          + errField + "': " + definition.toString()));
  }
  if (!tmpLinkMeta.json(server, normalized, false, nullptr, defaultVocbase, nullptr, true)) {
    return arangodb::Result(
        TRI_ERROR_BAD_PARAMETER,
        std::string("failed to initialize index from definition: ") + definition.toString());
  }
  auto nameSlice = definition.get(arangodb::StaticStrings::IndexName);
  if (nameSlice.isString() && nameSlice.getStringLength() > 0) {
    normalized.add(arangodb::StaticStrings::IndexName, nameSlice);
  } else if (!nameSlice.isNone()) {
    return arangodb::Result(
        TRI_ERROR_BAD_PARAMETER,
        std::string("failed to initialize index from definition, error in attribute '")
        + arangodb::StaticStrings::IndexName + "': "
        + definition.toString());
  }
  return {};
}

// Analyzer names storing
//  - forPersistence ::<analyzer> from system and <analyzer> for local and definitions are stored.
//  - For user -> database-name qualified names. No definitions are stored
Result IResearchInvertedIndexMeta::json(application_features::ApplicationServer & server,
                                        TRI_vocbase_t const * defaultVocbase,
                                        velocypack::Builder & builder,
                                        bool forPersistence) const
{
  if (_indexMeta.json(builder, nullptr, nullptr) &&
    _fieldsMeta.json(server, builder, forPersistence, nullptr, defaultVocbase, nullptr, true)) {
    return {};
  } else {
    return Result(TRI_ERROR_BAD_PARAMETER);
  }
}

std::vector<std::vector<arangodb::basics::AttributeName>> IResearchInvertedIndexMeta::fields() const {
  std::vector<std::vector<arangodb::basics::AttributeName>> res;
  traverseMetaFields(_fieldsMeta, res);
  return res;
}

IResearchRocksDBInvertedIndex::IResearchRocksDBInvertedIndex(IndexId id, LogicalCollection& collection, IResearchInvertedIndexMeta&& meta)
  : RocksDBIndex(id, collection, meta._name, meta.fields(), true, false,
    RocksDBColumnFamilyManager::get(RocksDBColumnFamilyManager::Family::Invalid),
    meta._objectId, false),
  IResearchInvertedIndex(std::move(meta)) {}

void IResearchRocksDBInvertedIndex::toVelocyPack(VPackBuilder & builder,
                                                 std::underlying_type<Index::Serialize>::type flags) const {
  auto const forPersistence = Index::hasFlag(flags, Index::Serialize::Internals);
  VPackObjectBuilder objectBuilder(&builder);
  IResearchInvertedIndex::toVelocyPack(collection().vocbase().server(),
                                       &collection().vocbase(), builder, forPersistence);
  if (Index::hasFlag(flags, Index::Serialize::Internals)) {
    TRI_ASSERT(objectId() != 0);  // If we store it, it cannot be 0
    builder.add("objectId", VPackValue(std::to_string(objectId())));
  }
  // can't use Index::toVelocyPack as it will try to output 'fields'
  // but we have custom storage format
  builder.add(arangodb::StaticStrings::IndexId,
              arangodb::velocypack::Value(std::to_string(_iid.id())));
  builder.add(arangodb::StaticStrings::IndexType,
              arangodb::velocypack::Value(oldtypeName(type())));
  builder.add(arangodb::StaticStrings::IndexName, arangodb::velocypack::Value(name()));
}

} // namespace iresearch
} // namespace arangodb
