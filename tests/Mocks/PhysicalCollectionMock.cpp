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
/// @author Andrey Abramov
/// @author Vasiliy Nabatchikov
////////////////////////////////////////////////////////////////////////////////

#include "PhysicalCollectionMock.h"

#include "ApplicationFeatures/ApplicationServer.h"
#include "Basics/DownCast.h"
#include "Basics/StaticStrings.h"
#include "Basics/VelocyPackHelper.h"
#include "ClusterEngine/ClusterEngine.h"
#include "Indexes/SimpleAttributeEqualityMatcher.h"
#include "Indexes/SortedIndexAttributeMatcher.h"
#include "IResearch/IResearchCommon.h"
#include "IResearch/IResearchFeature.h"
#include "Logger/LogMacros.h"
#include "StorageEngine/EngineSelectorFeature.h"
#include "Transaction/Helpers.h"
#include "Transaction/OperationOrigin.h"
#include "Transaction/StandaloneContext.h"
#include "Utils/SingleCollectionTransaction.h"
#include "VocBase/LogicalCollection.h"

#include "Mocks/IResearchLinkMock.h"
#include "Mocks/IResearchInvertedIndexMock.h"
#include "Mocks/StorageEngineMock.h"

#include <velocypack/Iterator.h>

namespace {

arangodb::LocalDocumentId generateDocumentId(
    arangodb::LogicalCollection const& collection,
    arangodb::RevisionId revisionId, uint64_t& documentId) {
  bool useRev = collection.usesRevisionsAsDocumentIds();
  return useRev ? arangodb::LocalDocumentId::create(revisionId)
                : arangodb::LocalDocumentId::create(++documentId);
}

/// @brief hard-coded vector of the index attributes
/// note that the attribute names must be hard-coded here to avoid an init-order
/// fiasco with StaticStrings::FromString etc.
std::vector<std::vector<arangodb::basics::AttributeName>> const IndexAttributes{
    {arangodb::basics::AttributeName(std::string_view("_from"), false)},
    {arangodb::basics::AttributeName(std::string_view("_to"), false)}};

/// @brief add a single value node to the iterator's keys
void handleValNode(VPackBuilder* keys, arangodb::aql::AstNode const* valNode) {
  if (!valNode->isStringValue() || valNode->getStringLength() == 0) {
    return;
  }

  keys->openObject();
  keys->add(arangodb::StaticStrings::IndexEq,
            VPackValuePair(valNode->getStringValue(),
                           valNode->getStringLength(), VPackValueType::String));
  keys->close();

  TRI_IF_FAILURE("EdgeIndex::collectKeys") {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_DEBUG);
  }
}

class EdgeIndexIteratorMock final : public arangodb::IndexIterator {
 public:
  typedef std::unordered_multimap<std::string, arangodb::LocalDocumentId> Map;

  EdgeIndexIteratorMock(arangodb::LogicalCollection* collection,
                        arangodb::transaction::Methods* trx,
                        arangodb::Index const* index, Map const& map,
                        std::unique_ptr<VPackBuilder>&& keys, bool isFrom)
      : IndexIterator(collection, trx, arangodb::ReadOwnWrites::no),
        _map(map),
        _begin(_map.end()),
        _end(_map.end()),
        _keys(std::move(keys)),
        _keysIt(_keys->slice()) {}

  bool prepareNextRange() {
    if (_keysIt.valid()) {
      auto key = _keysIt.value();

      if (key.isObject()) {
        key = key.get(arangodb::StaticStrings::IndexEq);
      }

      std::tie(_begin, _end) = _map.equal_range(key.toString());
      ++_keysIt;
      return true;
    } else {
      // Just make sure begin and end are equal
      _begin = _map.end();
      _end = _map.end();
      return false;
    }
  }

  std::string_view typeName() const noexcept final {
    return "edge-index-iterator-mock";
  }

  bool nextImpl(LocalDocumentIdCallback const& cb, uint64_t limit) override {
    // We can at most return limit
    for (uint64_t l = 0; l < limit; ++l) {
      while (_begin == _end) {
        if (!prepareNextRange()) {
          return false;
        }
      }
      TRI_ASSERT(_begin != _end);
      cb(_begin->second);
      ++_begin;
    }
    // Returned due to limit.
    if (_begin == _end) {
      // Out limit hit the last index entry
      // Return false if we do not have another range
      return prepareNextRange();
    }
    return true;
  }

  void resetImpl() override {
    _keysIt.reset();
    _begin = _map.begin();
    _end = _map.end();
  }

 private:
  Map const& _map;
  Map::const_iterator _begin;
  Map::const_iterator _end;
  std::unique_ptr<VPackBuilder> _keys;
  arangodb::velocypack::ArrayIterator _keysIt;
};  // EdgeIndexIteratorMock

class AllIteratorMock final : public arangodb::IndexIterator {
 public:
  AllIteratorMock(
      std::unordered_map<std::string_view,
                         PhysicalCollectionMock::DocElement> const& data,
      arangodb::LogicalCollection& coll, arangodb::transaction::Methods* trx,
      arangodb::ReadOwnWrites readOwnWrites)
      : arangodb::IndexIterator(&coll, trx, readOwnWrites),
        _data(data),
        _ref(readOwnWrites == arangodb::ReadOwnWrites::yes ? data : _data),
        _it{_ref.begin()} {}

  std::string_view typeName() const noexcept final { return "AllIteratorMock"; }

  void resetImpl() override { _it = _ref.begin(); }

  bool nextImpl(LocalDocumentIdCallback const& callback,
                uint64_t limit) override {
    while (_it != _ref.end() && limit != 0) {
      callback(_it->second.docId());
      ++_it;
      --limit;
    }
    return 0 == limit;
  }

 private:
  // we need to take a copy of the incoming data here, so we can iterate over
  // the original data safely while the collection data is being modified
  std::unordered_map<std::string_view, PhysicalCollectionMock::DocElement>
      _data;
  // we also need to keep a reference to the original map in order to satisfy
  // read-your-own-writes queries
  std::unordered_map<std::string_view,
                     PhysicalCollectionMock::DocElement> const& _ref;
  std::unordered_map<std::string_view,
                     PhysicalCollectionMock::DocElement>::const_iterator _it;
};  // AllIteratorMock

class EdgeIndexMock final : public arangodb::Index {
 public:
  static std::shared_ptr<arangodb::Index> make(
      arangodb::IndexId iid, arangodb::LogicalCollection& collection,
      arangodb::velocypack::Slice const& definition) {
    auto const typeSlice = definition.get("type");

    if (typeSlice.isNone()) {
      return nullptr;
    }

    auto const type = arangodb::basics::VelocyPackHelper::getStringView(
        typeSlice, std::string_view());

    if (type.compare("edge") != 0) {
      return nullptr;
    }

    return std::make_shared<EdgeIndexMock>(iid, collection);
  }

  IndexType type() const override { return Index::TRI_IDX_TYPE_EDGE_INDEX; }

  char const* typeName() const override { return "edge"; }

  std::vector<std::vector<arangodb::basics::AttributeName>> const&
  coveredFields() const override {
    // index does not cover the index attribute!
    return Index::emptyCoveredFields;
  }

  bool canBeDropped() const override { return false; }

  bool isHidden() const override { return false; }

  bool isSorted() const override { return false; }

  bool hasSelectivityEstimate() const override { return false; }

  size_t memory() const override { return sizeof(EdgeIndexMock); }

  void load() override {}
  void unload() override {}

  void toVelocyPack(VPackBuilder& builder,
                    std::underlying_type<arangodb::Index::Serialize>::type
                        flags) const override {
    builder.openObject();
    Index::toVelocyPack(builder, flags);
    // hard-coded
    builder.add("unique", VPackValue(false));
    builder.add("sparse", VPackValue(false));
    builder.close();
  }

  void toVelocyPackFigures(VPackBuilder& builder) const override {
    Index::toVelocyPackFigures(builder);

    builder.add("from", VPackValue(VPackValueType::Object));
    //_edgesFrom->appendToVelocyPack(builder);
    builder.close();

    builder.add("to", VPackValue(VPackValueType::Object));
    //_edgesTo->appendToVelocyPack(builder);
    builder.close();
  }

  arangodb::Result insert(arangodb::transaction::Methods& trx,
                          arangodb::LocalDocumentId documentId,
                          arangodb::velocypack::Slice doc) {
    if (!doc.isObject()) {
      return {TRI_ERROR_INTERNAL};
    }

    VPackSlice const fromValue(
        arangodb::transaction::helpers::extractFromFromDocument(doc));

    if (!fromValue.isString()) {
      return {TRI_ERROR_INTERNAL};
    }

    VPackSlice const toValue(
        arangodb::transaction::helpers::extractToFromDocument(doc));

    if (!toValue.isString()) {
      return {TRI_ERROR_INTERNAL};
    }

    _edgesFrom.emplace(fromValue.toString(), documentId);
    _edgesTo.emplace(toValue.toString(), documentId);

    return {};  // ok
  }

  arangodb::Result remove(arangodb::transaction::Methods& trx,
                          arangodb::LocalDocumentId,
                          arangodb::velocypack::Slice doc,
                          arangodb::IndexOperationMode) {
    if (!doc.isObject()) {
      return {TRI_ERROR_INTERNAL};
    }

    VPackSlice const fromValue(
        arangodb::transaction::helpers::extractFromFromDocument(doc));

    if (!fromValue.isString()) {
      return {TRI_ERROR_INTERNAL};
    }

    VPackSlice const toValue(
        arangodb::transaction::helpers::extractToFromDocument(doc));

    if (!toValue.isString()) {
      return {TRI_ERROR_INTERNAL};
    }

    _edgesFrom.erase(fromValue.toString());
    _edgesTo.erase(toValue.toString());

    return {};  // ok
  }

  Index::FilterCosts supportsFilterCondition(
      arangodb::transaction::Methods& /*trx*/,
      std::vector<std::shared_ptr<arangodb::Index>> const& /*allIndexes*/,
      arangodb::aql::AstNode const* node,
      arangodb::aql::Variable const* reference,
      size_t itemsInIndex) const override {
    arangodb::SimpleAttributeEqualityMatcher matcher(IndexAttributes);
    return matcher.matchOne(this, node, reference, itemsInIndex);
  }

  std::unique_ptr<arangodb::IndexIterator> iteratorForCondition(
      arangodb::ResourceMonitor& monitor, arangodb::transaction::Methods* trx,
      arangodb::aql::AstNode const* node, arangodb::aql::Variable const*,
      arangodb::IndexIteratorOptions const&, arangodb::ReadOwnWrites,
      int) override {
    TRI_ASSERT(node->type == arangodb::aql::NODE_TYPE_OPERATOR_NARY_AND);

    TRI_ASSERT(node->numMembers() == 1);

    auto comp = node->getMember(0);

    // assume a.b == value
    auto attrNode = comp->getMember(0);
    auto valNode = comp->getMember(1);

    if (attrNode->type != arangodb::aql::NODE_TYPE_ATTRIBUTE_ACCESS) {
      // got value == a.b  -> flip sides
      std::swap(attrNode, valNode);
    }
    TRI_ASSERT(attrNode->type == arangodb::aql::NODE_TYPE_ATTRIBUTE_ACCESS);

    if (comp->type == arangodb::aql::NODE_TYPE_OPERATOR_BINARY_EQ) {
      // a.b == value
      return createEqIterator(trx, attrNode, valNode);
    }

    if (comp->type == arangodb::aql::NODE_TYPE_OPERATOR_BINARY_IN) {
      // a.b IN values
      if (!valNode->isArray()) {
        // a.b IN non-array
        return std::make_unique<arangodb::EmptyIndexIterator>(&_collection,
                                                              trx);
      }

      return createInIterator(trx, attrNode, valNode);
    }

    // operator type unsupported
    return std::make_unique<arangodb::EmptyIndexIterator>(&_collection, trx);
  }

  arangodb::aql::AstNode* specializeCondition(
      arangodb::transaction::Methods& /*trx*/, arangodb::aql::AstNode* node,
      arangodb::aql::Variable const* reference) const override {
    arangodb::SimpleAttributeEqualityMatcher matcher(IndexAttributes);

    return matcher.specializeOne(this, node, reference);
  }

  EdgeIndexMock(arangodb::IndexId iid, arangodb::LogicalCollection& collection)
      : arangodb::Index(iid, collection, arangodb::StaticStrings::IndexNameEdge,
                        {{arangodb::basics::AttributeName(
                             arangodb::StaticStrings::FromString, false)},
                         {arangodb::basics::AttributeName(
                             arangodb::StaticStrings::ToString, false)}},
                        true, false) {}

  std::unique_ptr<arangodb::IndexIterator> createEqIterator(
      arangodb::transaction::Methods* trx,
      arangodb::aql::AstNode const* attrNode,
      arangodb::aql::AstNode const* valNode) const {
    // lease builder, but immediately pass it to the unique_ptr so we don't leak
    arangodb::transaction::BuilderLeaser builder(trx);
    std::unique_ptr<VPackBuilder> keys(builder.steal());
    keys->openArray();

    handleValNode(keys.get(), valNode);
    TRI_IF_FAILURE("EdgeIndex::noIterator") {
      THROW_ARANGO_EXCEPTION(TRI_ERROR_DEBUG);
    }
    keys->close();

    // _from or _to?
    bool const isFrom =
        (attrNode->stringEquals(arangodb::StaticStrings::FromString));

    return std::make_unique<EdgeIndexIteratorMock>(
        &_collection, trx, this, isFrom ? _edgesFrom : _edgesTo,
        std::move(keys), isFrom);
  }

  /// @brief create the iterator
  std::unique_ptr<arangodb::IndexIterator> createInIterator(
      arangodb::transaction::Methods* trx,
      arangodb::aql::AstNode const* attrNode,
      arangodb::aql::AstNode const* valNode) const {
    // lease builder, but immediately pass it to the unique_ptr so we don't leak
    arangodb::transaction::BuilderLeaser builder(trx);
    std::unique_ptr<VPackBuilder> keys(builder.steal());
    keys->openArray();

    size_t const n = valNode->numMembers();
    for (size_t i = 0; i < n; ++i) {
      handleValNode(keys.get(), valNode->getMemberUnchecked(i));
      TRI_IF_FAILURE("EdgeIndex::iteratorValNodes") {
        THROW_ARANGO_EXCEPTION(TRI_ERROR_DEBUG);
      }
    }

    TRI_IF_FAILURE("EdgeIndex::noIterator") {
      THROW_ARANGO_EXCEPTION(TRI_ERROR_DEBUG);
    }
    keys->close();

    // _from or _to?
    bool const isFrom =
        (attrNode->stringEquals(arangodb::StaticStrings::FromString));

    return std::make_unique<EdgeIndexIteratorMock>(
        &_collection, trx, this, isFrom ? _edgesFrom : _edgesTo,
        std::move(keys), isFrom);
  }

  /// @brief the hash table for _from
  EdgeIndexIteratorMock::Map _edgesFrom;
  /// @brief the hash table for _to
  EdgeIndexIteratorMock::Map _edgesTo;
};  // EdgeIndexMock

class HashIndexMap {
  struct VPackBuilderHasher {
    std::size_t operator()(VPackBuilder const& builder) const {
      return std::hash<VPackSlice>()(builder.slice());
    }
  };

  struct VPackBuilderComparator {
    bool operator()(VPackBuilder const& builder1,
                    VPackBuilder const& builder2) const {
      return ::arangodb::basics::VelocyPackHelper::compare(
                 builder1.slice(), builder2.slice(), true) == 0;
    }
  };

  using ValueMap =
      std::unordered_multimap<VPackBuilder, arangodb::LocalDocumentId,
                              VPackBuilderHasher, VPackBuilderComparator>;
  using DocumentsIndexMap =
      std::unordered_map<arangodb::LocalDocumentId, VPackBuilder>;

  arangodb::velocypack::Slice getSliceByField(
      arangodb::velocypack::Slice const& doc, size_t i) {
    TRI_ASSERT(i < _fields.size());
    TRI_ASSERT(!doc.isNone());
    auto slice = doc;
    for (auto const& f : _fields[i]) {
      slice = slice.get(f.name);
      if (slice.isNone() || slice.isNull()) {
        break;
      }
    }
    return slice;
  }

  void insertSlice(arangodb::LocalDocumentId documentId,
                   arangodb::velocypack::Slice const& slice, size_t i) {
    VPackBuilder builder;
    if (slice.isNone() || slice.isNull()) {
      builder.add(VPackSlice::nullSlice());
    } else {
      builder.add(slice);
    }
    _valueMaps[i].emplace(std::move(builder), documentId);
  }

 public:
  HashIndexMap(
      std::vector<std::vector<arangodb::basics::AttributeName>> const& fields)
      : _fields(fields), _valueMaps(fields.size()) {
    TRI_ASSERT(!_fields.empty());
  }

  void insert(arangodb::LocalDocumentId documentId,
              arangodb::velocypack::Slice const& doc) {
    VPackBuilder builder;
    builder.openArray();
    auto toClose = true;
    // find fields for the index
    for (size_t i = 0; i < _fields.size(); ++i) {
      auto slice = doc;
      auto isExpansion = false;
      for (auto fieldIt = _fields[i].begin(); fieldIt != _fields[i].end();
           ++fieldIt) {
        TRI_ASSERT(slice.isObject() || slice.isArray());
        if (slice.isObject()) {
          slice = slice.get(fieldIt->name);
          if ((fieldIt->shouldExpand && slice.isObject()) ||
              (!fieldIt->shouldExpand && slice.isArray())) {
            slice = VPackSlice::nullSlice();
            break;
          }
          if (slice.isNone() || slice.isNull()) {
            break;
          }
        } else {  // expansion
          isExpansion = slice.isArray();
          TRI_ASSERT(isExpansion);
          auto found = false;
          for (auto sliceIt = arangodb::velocypack::ArrayIterator(slice);
               sliceIt != sliceIt.end(); ++sliceIt) {
            auto subSlice = sliceIt.value();
            if (!(subSlice.isNone() || subSlice.isNull())) {
              for (auto fieldItForArray = fieldIt;
                   fieldItForArray != _fields[i].end(); ++fieldItForArray) {
                TRI_ASSERT(subSlice.isObject());
                subSlice = subSlice.get(fieldItForArray->name);
                if (subSlice.isNone() || subSlice.isNull()) {
                  break;
                }
              }
              if (!(subSlice.isNone() || subSlice.isNull())) {
                insertSlice(documentId, subSlice, i);
                builder.add(subSlice);
                found = true;
                break;
              }
            }
          }
          if (!found) {
            insertSlice(documentId, VPackSlice::nullSlice(), i);
            builder.add(VPackSlice::nullSlice());
          }
          break;
        }
      }
      if (!isExpansion) {
        // if the last expansion (at the end) leave the array open
        if (slice.isArray() && i == _fields.size() - 1) {
          auto found = false;
          auto wasNull = false;
          for (auto sliceIt = arangodb::velocypack::ArrayIterator(slice);
               sliceIt != sliceIt.end(); ++sliceIt) {
            auto subSlice = sliceIt.value();
            if (!(subSlice.isNone() || subSlice.isNull())) {
              insertSlice(documentId, subSlice, i);
              found = true;
            } else {
              wasNull = true;
            }
          }
          if (!found || wasNull) {
            insertSlice(documentId, VPackSlice::nullSlice(), i);
          }
          toClose = false;
        } else {  // object
          insertSlice(documentId, slice, i);
          builder.add(slice);
        }
      }
    }
    if (toClose) {
      builder.close();
    }
    _docIndexMap.try_emplace(documentId, std::move(builder));
  }

  bool remove(arangodb::LocalDocumentId documentId,
              arangodb::velocypack::Slice doc) {
    size_t i = 0;
    auto documentRemoved = false;
    for (auto& map : _valueMaps) {
      auto slice = getSliceByField(doc, i++);
      auto [begin, end] = map.equal_range(VPackBuilder(slice));
      for (; begin != end; ++begin) {
        if (begin->second == documentId) {
          map.erase(begin);
          documentRemoved = true;
          // not break because of expansions
        }
      }
    }
    _docIndexMap.erase(documentId);
    return documentRemoved;
  }

  void clear() {
    _valueMaps.clear();
    _docIndexMap.clear();
  }

  std::unordered_map<arangodb::LocalDocumentId, VPackBuilder> find(
      std::unique_ptr<VPackBuilder>&& keys) const {
    std::unordered_map<arangodb::LocalDocumentId, VPackBuilder const*> found;
    TRI_ASSERT(keys->slice().isArray());
    auto sliceIt = arangodb::velocypack::ArrayIterator(keys->slice());
    if (!sliceIt.valid()) {
      return std::unordered_map<arangodb::LocalDocumentId, VPackBuilder>();
    }
    for (auto const& map : _valueMaps) {
      auto [begin, end] = map.equal_range(VPackBuilder(sliceIt.value()));
      if (begin == end) {
        return std::unordered_map<arangodb::LocalDocumentId, VPackBuilder>();
      }
      if (found.empty()) {
        std::transform(begin, end, std::inserter(found, found.end()),
                       [](auto const& item) {
                         return std::make_pair(item.second, &item.first);
                       });
      } else {
        std::unordered_map<arangodb::LocalDocumentId, VPackBuilder const*>
            tmpFound;
        for (; begin != end; ++begin) {
          if (found.find(begin->second) != found.cend()) {
            tmpFound.try_emplace(begin->second, &begin->first);
          }
        }
        if (tmpFound.empty()) {
          return std::unordered_map<arangodb::LocalDocumentId, VPackBuilder>();
        }
        found.swap(tmpFound);
      }
      if (!(++sliceIt).valid()) {
        break;
      }
    }
    std::unordered_map<arangodb::LocalDocumentId, VPackBuilder>
        foundWithCovering;
    for (auto const& d : found) {
      auto doc = _docIndexMap.find(d.first);
      TRI_ASSERT(doc != _docIndexMap.cend());
      auto builder = doc->second;
      // the array was left open for the last expansion (at the end)
      if (doc->second.isOpenArray()) {
        builder.add(d.second->slice());
        builder.close();
      }
      foundWithCovering.try_emplace(doc->first, std::move(builder));
    }
    return foundWithCovering;
  }

 private:
  std::vector<std::vector<arangodb::basics::AttributeName>> const& _fields;
  std::vector<ValueMap> _valueMaps;
  DocumentsIndexMap _docIndexMap;
};

class HashIndexIteratorMock final : public arangodb::IndexIterator {
 public:
  HashIndexIteratorMock(arangodb::LogicalCollection* collection,
                        arangodb::transaction::Methods* trx,
                        arangodb::Index const* index, HashIndexMap const& map,
                        std::unique_ptr<VPackBuilder>&& keys)
      : IndexIterator(collection, trx, arangodb::ReadOwnWrites::no), _map(map) {
    _documents = _map.find(std::move(keys));
    _begin = _documents.begin();
    _end = _documents.end();
  }

  std::string_view typeName() const noexcept final {
    return "hash-index-iterator-mock";
  }

  bool nextCoveringImpl(CoveringCallback const& cb, uint64_t limit) override {
    while (limit && _begin != _end) {
      auto data = SliceCoveringData(_begin->second.slice());
      cb(_begin->first, data);
      ++_begin;
      --limit;
    }

    return _begin != _end;
  }

  bool nextImpl(LocalDocumentIdCallback const& cb, uint64_t limit) override {
    while (limit && _begin != _end) {
      cb(_begin->first);
      ++_begin;
      --limit;
    }

    return _begin != _end;
  }

  void resetImpl() override {
    _documents.clear();
    _begin = _documents.begin();
    _end = _documents.end();
  }

 private:
  HashIndexMap const& _map;
  std::unordered_map<arangodb::LocalDocumentId, VPackBuilder> _documents;
  std::unordered_map<arangodb::LocalDocumentId, VPackBuilder>::const_iterator
      _begin;
  std::unordered_map<arangodb::LocalDocumentId, VPackBuilder>::const_iterator
      _end;
};  // HashIndexIteratorMock

class HashIndexMock final : public arangodb::Index {
 public:
  static std::shared_ptr<arangodb::Index> make(
      arangodb::IndexId iid, arangodb::LogicalCollection& collection,
      arangodb::velocypack::Slice const& definition) {
    auto const typeSlice = definition.get("type");

    if (typeSlice.isNone()) {
      return nullptr;
    }

    auto const type = arangodb::basics::VelocyPackHelper::getStringView(
        typeSlice, std::string_view());

    if (type.compare("hash") != 0) {
      return nullptr;
    }

    return std::make_shared<HashIndexMock>(iid, collection, definition);
  }

  IndexType type() const override { return Index::TRI_IDX_TYPE_HASH_INDEX; }

  char const* typeName() const override { return "hash"; }

  bool canBeDropped() const override { return false; }

  bool isHidden() const override { return false; }

  bool isSorted() const override { return false; }

  bool hasSelectivityEstimate() const override { return false; }

  size_t memory() const override { return sizeof(HashIndexMock); }

  void load() override {}

  void unload() override {}

  void toVelocyPack(VPackBuilder& builder,
                    std::underlying_type<arangodb::Index::Serialize>::type
                        flags) const override {
    builder.openObject();
    Index::toVelocyPack(builder, flags);
    builder.add("sparse", VPackValue(sparse()));
    builder.add("unique", VPackValue(unique()));
    builder.close();
  }

  void toVelocyPackFigures(VPackBuilder& builder) const override {
    Index::toVelocyPackFigures(builder);
  }

  arangodb::Result insert(arangodb::transaction::Methods&,
                          arangodb::LocalDocumentId documentId,
                          arangodb::velocypack::Slice const doc) {
    if (!doc.isObject()) {
      return {TRI_ERROR_INTERNAL};
    }

    _hashData.insert(documentId, doc);

    return {};  // ok
  }

  arangodb::Result remove(arangodb::transaction::Methods&,
                          arangodb::LocalDocumentId documentId,
                          arangodb::velocypack::Slice doc,
                          arangodb::OperationOptions const& /*options*/) {
    if (!doc.isObject()) {
      return {TRI_ERROR_INTERNAL};
    }

    _hashData.remove(documentId, doc);

    return {};  // ok
  }

  Index::FilterCosts supportsFilterCondition(
      arangodb::transaction::Methods& /*trx*/,
      std::vector<std::shared_ptr<arangodb::Index>> const& allIndexes,
      arangodb::aql::AstNode const* node,
      arangodb::aql::Variable const* reference,
      size_t itemsInIndex) const override {
    return arangodb::SortedIndexAttributeMatcher::supportsFilterCondition(
        allIndexes, this, node, reference, itemsInIndex);
  }

  Index::SortCosts supportsSortCondition(
      arangodb::aql::SortCondition const* sortCondition,
      arangodb::aql::Variable const* reference,
      size_t itemsInIndex) const override {
    return arangodb::SortedIndexAttributeMatcher::supportsSortCondition(
        this, sortCondition, reference, itemsInIndex);
  }

  arangodb::aql::AstNode* specializeCondition(
      arangodb::transaction::Methods& /*trx*/, arangodb::aql::AstNode* node,
      arangodb::aql::Variable const* reference) const override {
    return arangodb::SortedIndexAttributeMatcher::specializeCondition(
        this, node, reference);
  }

  std::unique_ptr<arangodb::IndexIterator> iteratorForCondition(
      arangodb::ResourceMonitor& monitor, arangodb::transaction::Methods* trx,
      arangodb::aql::AstNode const* node, arangodb::aql::Variable const*,
      arangodb::IndexIteratorOptions const&, arangodb::ReadOwnWrites,
      int) override {
    arangodb::transaction::BuilderLeaser builder(trx);
    std::unique_ptr<VPackBuilder> keys(builder.steal());
    keys->openArray();
    if (nullptr == node) {
      keys->close();
      return std::make_unique<HashIndexIteratorMock>(
          &_collection, trx, this, _hashData, std::move(keys));
    }
    TRI_ASSERT(node->type == arangodb::aql::NODE_TYPE_OPERATOR_NARY_AND);

    std::vector<std::pair<std::vector<arangodb::basics::AttributeName>,
                          arangodb::aql::AstNode*>>
        allAttributes;
    for (size_t i = 0; i < node->numMembers(); ++i) {
      auto comp = node->getMember(i);
      // a.b == value
      if (!(comp->type == arangodb::aql::NODE_TYPE_OPERATOR_BINARY_EQ ||
            comp->type == arangodb::aql::NODE_TYPE_OPERATOR_BINARY_IN)) {
        // operator type unsupported
        return std::make_unique<arangodb::EmptyIndexIterator>(&_collection,
                                                              trx);
      }

      // assume a.b == value
      auto attrNode = comp->getMember(0);
      auto valNode = comp->getMember(1);

      if (!(attrNode->type == arangodb::aql::NODE_TYPE_ATTRIBUTE_ACCESS ||
            attrNode->type == arangodb::aql::NODE_TYPE_EXPANSION)) {
        // got value == a.b -> flip sides
        std::swap(attrNode, valNode);
      }
      TRI_ASSERT(attrNode->type == arangodb::aql::NODE_TYPE_ATTRIBUTE_ACCESS ||
                 attrNode->type == arangodb::aql::NODE_TYPE_EXPANSION);

      std::vector<arangodb::basics::AttributeName> attributes;
      if (attrNode->type == arangodb::aql::NODE_TYPE_ATTRIBUTE_ACCESS) {
        do {
          attributes.emplace_back(std::string(attrNode->getStringValue(),
                                              attrNode->getStringLength()),
                                  false);
          attrNode = attrNode->getMember(0);
        } while (attrNode->type == arangodb::aql::NODE_TYPE_ATTRIBUTE_ACCESS);
        std::reverse(attributes.begin(), attributes.end());
      } else {  // expansion
        TRI_ASSERT(attrNode->type == arangodb::aql::NODE_TYPE_EXPANSION);
        auto expNode = attrNode;
        TRI_ASSERT(expNode->numMembers() >= 2);
        auto left = expNode->getMember(0);
        TRI_ASSERT(left->type == arangodb::aql::NODE_TYPE_ITERATOR);
        attrNode = left->getMember(1);
        TRI_ASSERT(attrNode->type == arangodb::aql::NODE_TYPE_ATTRIBUTE_ACCESS);
        do {
          attributes.emplace_back(std::string(attrNode->getStringValue(),
                                              attrNode->getStringLength()),
                                  false);
          attrNode = attrNode->getMember(0);
        } while (attrNode->type == arangodb::aql::NODE_TYPE_ATTRIBUTE_ACCESS);
        attributes.front().shouldExpand = true;
        std::reverse(attributes.begin(), attributes.end());

        std::vector<arangodb::basics::AttributeName> attributesRight;
        attrNode = expNode->getMember(1);
        TRI_ASSERT(attrNode->type ==
                       arangodb::aql::NODE_TYPE_ATTRIBUTE_ACCESS ||
                   attrNode->type == arangodb::aql::NODE_TYPE_REFERENCE);
        while (attrNode->type == arangodb::aql::NODE_TYPE_ATTRIBUTE_ACCESS) {
          attributesRight.emplace_back(std::string(attrNode->getStringValue(),
                                                   attrNode->getStringLength()),
                                       false);
          attrNode = attrNode->getMember(0);
        }
        attributes.insert(attributes.end(), attributesRight.crbegin(),
                          attributesRight.crend());
      }
      allAttributes.emplace_back(std::move(attributes), valNode);
    }
    size_t nullsCount = 0;
    for (auto const& f : _fields) {
      auto it =
          std::find_if(allAttributes.cbegin(), allAttributes.cend(),
                       [&f](auto const& attrs) {
                         return arangodb::basics::AttributeName::isIdentical(
                             attrs.first, f, true);
                       });
      if (it != allAttributes.cend()) {
        while (nullsCount > 0) {
          keys->add(VPackSlice::nullSlice());
          --nullsCount;
        }
        it->second->toVelocyPackValue(*keys);
      } else {
        ++nullsCount;
      }
    }
    keys->close();

    return std::make_unique<HashIndexIteratorMock>(&_collection, trx, this,
                                                   _hashData, std::move(keys));
  }

  HashIndexMock(arangodb::IndexId iid, arangodb::LogicalCollection& collection,
                VPackSlice const& slice)
      : arangodb::Index(iid, collection, slice), _hashData(_fields) {}

  /// @brief the hash table for data
  HashIndexMap _hashData;
};  // HashIndexMock

}  // namespace

PhysicalCollectionMock::DocElement::DocElement(
    std::shared_ptr<arangodb::velocypack::Buffer<uint8_t>> data, uint64_t docId)
    : _data(data), _docId(docId) {}

arangodb::velocypack::Slice PhysicalCollectionMock::DocElement::data() const {
  return arangodb::velocypack::Slice(_data->data());
}

std::shared_ptr<arangodb::velocypack::Buffer<uint8_t>>
PhysicalCollectionMock::DocElement::rawData() const {
  return _data;
}

void PhysicalCollectionMock::DocElement::swapBuffer(
    std::shared_ptr<arangodb::velocypack::Buffer<uint8_t>>& newData) {
  _data.swap(newData);
}

arangodb::LocalDocumentId PhysicalCollectionMock::DocElement::docId() const {
  return arangodb::LocalDocumentId::create(_docId);
}

uint8_t const* PhysicalCollectionMock::DocElement::vptr() const {
  return _data->data();
}

std::function<void()> PhysicalCollectionMock::before = []() -> void {};

PhysicalCollectionMock::PhysicalCollectionMock(
    arangodb::LogicalCollection& collection)
    : PhysicalCollection(collection), _lastDocumentId{0} {}

arangodb::futures::Future<std::shared_ptr<arangodb::Index>>
PhysicalCollectionMock::createIndex(
    arangodb::velocypack::Slice info, bool restore, bool& created,
    std::shared_ptr<std::function<arangodb::Result(double)>> progress,
    arangodb::replication2::replicated_state::document::Replication2Callback
        replicationCb) {
  before();

  std::vector<std::pair<arangodb::LocalDocumentId, arangodb::velocypack::Slice>>
      docs;
  docs.reserve(_documents.size());
  for (auto const& entry : _documents) {
    auto& doc = entry.second;
    docs.emplace_back(doc.docId(), doc.data());
  }

  struct IndexFactory : public arangodb::IndexFactory {
    using arangodb::IndexFactory::validateSlice;
  };
  auto id = IndexFactory::validateSlice(
      info, true, false);  // true+false to ensure id generation if missing

  auto const type = arangodb::basics::VelocyPackHelper::getStringView(
      info.get("type"), std::string_view());

  std::shared_ptr<arangodb::Index> index;

  if (type == "edge") {
    index = EdgeIndexMock::make(id, _logicalCollection, info);
  } else if (type == "hash") {
    index = HashIndexMock::make(id, _logicalCollection, info);
  } else if (type == "inverted") {
    index =
        StorageEngineMock::buildInvertedIndexMock(id, _logicalCollection, info);
  } else if (type == arangodb::iresearch::StaticStrings::ViewArangoSearchType) {
    try {
      auto& server = _logicalCollection.vocbase().server();
      if (arangodb::ServerState::instance()->isCoordinator()) {
        auto& factory =
            server.getFeature<arangodb::iresearch::IResearchFeature>()
                .factory<arangodb::ClusterEngine>();
        index = factory.instantiate(_logicalCollection, info, id, false);
      } else {
        index = StorageEngineMock::buildLinkMock(id, _logicalCollection, info);
      }
    } catch (std::exception const& ex) {
      // ignore the details of all errors here
      LOG_DEVEL << "caught: " << ex.what();
    }
  }

  if (!index) {
    return std::shared_ptr<arangodb::Index>{nullptr};
  }

  TRI_vocbase_t& vocbase = _logicalCollection.vocbase();
  arangodb::SingleCollectionTransaction trx(
      arangodb::transaction::StandaloneContext::create(
          vocbase, arangodb::transaction::OperationOriginTestCase{}),
      _logicalCollection, arangodb::AccessMode::Type::WRITE);

  auto res = trx.begin();
  TRI_ASSERT(res.ok());

  if (index->type() == arangodb::Index::TRI_IDX_TYPE_EDGE_INDEX) {
    auto* l = dynamic_cast<EdgeIndexMock*>(index.get());
    TRI_ASSERT(l != nullptr);
    for (auto const& pair : docs) {
      l->insert(trx, pair.first, pair.second);
    }
  } else if (index->type() == arangodb::Index::TRI_IDX_TYPE_HASH_INDEX) {
    auto* l = dynamic_cast<HashIndexMock*>(index.get());
    TRI_ASSERT(l != nullptr);
    for (auto const& pair : docs) {
      l->insert(trx, pair.first, pair.second);
    }
  } else if (index->type() == arangodb::Index::TRI_IDX_TYPE_IRESEARCH_LINK) {
    auto* l =
        dynamic_cast<arangodb::iresearch::IResearchLinkMock*>(index.get());
    TRI_ASSERT(l != nullptr);
    for (auto const& pair : docs) {
      l->insert(trx, pair.first, pair.second);
    }
  } else if (index->type() == arangodb::Index::TRI_IDX_TYPE_INVERTED_INDEX) {
    auto* l = dynamic_cast<arangodb::iresearch::IResearchInvertedIndexMock*>(
        index.get());
    TRI_ASSERT(l != nullptr);
    for (auto const& pair : docs) {
      l->insert(trx, pair.first, pair.second);
    }
  } else {
    TRI_ASSERT(false);
  }

  _indexes.emplace(index);
  created = true;

  res = trx.commit();
  TRI_ASSERT(res.ok());

  if (index->type() == arangodb::Index::TRI_IDX_TYPE_INVERTED_INDEX) {
    auto* l = dynamic_cast<arangodb::iresearch::IResearchInvertedIndexMock*>(
        index.get());
    TRI_ASSERT(l != nullptr);
    auto commitRes = l->commit();
    TRI_ASSERT(commitRes.ok());
  }

  return index;
}

void PhysicalCollectionMock::deferDropCollection(
    std::function<bool(arangodb::LogicalCollection&)> const& callback) {
  before();

  callback(_logicalCollection);  // assume noone is using this collection (drop
                                 // immediately)
}

arangodb::Result PhysicalCollectionMock::dropIndex(arangodb::IndexId iid) {
  before();

  for (auto itr = _indexes.begin(), end = _indexes.end(); itr != end; ++itr) {
    if ((*itr)->id() == iid) {
      if ((*itr)->drop().ok()) {
        _indexes.erase(itr);
        return {};
      }
    }
  }

  return {TRI_ERROR_INTERNAL};
}

void PhysicalCollectionMock::figuresSpecific(bool /*details*/,
                                             arangodb::velocypack::Builder&) {
  before();
  TRI_ASSERT(false);
}

std::unique_ptr<arangodb::IndexIterator> PhysicalCollectionMock::getAllIterator(
    arangodb::transaction::Methods* trx,
    arangodb::ReadOwnWrites readOwnWrites) const {
  before();

  return std::make_unique<AllIteratorMock>(_documents, this->_logicalCollection,
                                           trx, readOwnWrites);
}

std::unique_ptr<arangodb::IndexIterator> PhysicalCollectionMock::getAnyIterator(
    arangodb::transaction::Methods* trx) const {
  before();
  return std::make_unique<AllIteratorMock>(_documents, this->_logicalCollection,
                                           trx, arangodb::ReadOwnWrites::no);
}

std::unique_ptr<arangodb::ReplicationIterator>
PhysicalCollectionMock::getReplicationIterator(
    arangodb::ReplicationIterator::Ordering, uint64_t) {
  return nullptr;
}

void PhysicalCollectionMock::getPropertiesVPack(
    arangodb::velocypack::Builder&) const {
  before();
}

arangodb::Result PhysicalCollectionMock::insert(
    arangodb::transaction::Methods& trx,
    arangodb::IndexesSnapshot const& /*indexesSnapshot*/,
    arangodb::RevisionId newRevisionId, arangodb::velocypack::Slice newDocument,
    arangodb::OperationOptions const& options) {
  before();

  TRI_ASSERT(newDocument.isObject());
  TRI_ASSERT(newDocument.get(arangodb::StaticStrings::KeyString).isString());
  VPackSlice newKey = newDocument.get(arangodb::StaticStrings::KeyString);
  if (newKey.isString() && _documents.contains(newKey.stringView())) {
    return {TRI_ERROR_ARANGO_UNIQUE_CONSTRAINT_VIOLATED};
  }

  std::string_view key{newKey.stringView()};

  auto buffer = std::make_shared<arangodb::velocypack::Buffer<uint8_t>>();
  buffer->append(newDocument.start(), newDocument.byteSize());
  // key is a string_view, and it must point into the storage space that
  // we own and that keeps valid
  key = arangodb::velocypack::Slice(buffer->data())
            .get(arangodb::StaticStrings::KeyString)
            .stringView();

  arangodb::LocalDocumentId id =
      ::generateDocumentId(_logicalCollection, newRevisionId, _lastDocumentId);
  auto const& [ref, didInsert] =
      _documents.emplace(key, DocElement{std::move(buffer), id.id()});
  TRI_ASSERT(didInsert);

  for (auto& index : _indexes) {
    if (index->type() == arangodb::Index::TRI_IDX_TYPE_EDGE_INDEX) {
      auto* l = static_cast<EdgeIndexMock*>(index.get());
      if (!l->insert(trx, id, newDocument).ok()) {
        return {TRI_ERROR_BAD_PARAMETER};
      }
      continue;
    } else if (index->type() == arangodb::Index::TRI_IDX_TYPE_HASH_INDEX) {
      auto* l = static_cast<HashIndexMock*>(index.get());
      if (!l->insert(trx, id, newDocument).ok()) {
        return {TRI_ERROR_BAD_PARAMETER};
      }
      continue;
    } else if (index->type() == arangodb::Index::TRI_IDX_TYPE_IRESEARCH_LINK) {
      auto* l =
          static_cast<arangodb::iresearch::IResearchLinkMock*>(index.get());
      if (!l->insert(trx, id, newDocument).ok()) {
        return {TRI_ERROR_BAD_PARAMETER};
      }
      continue;
    } else if (index->type() == arangodb::Index::TRI_IDX_TYPE_INVERTED_INDEX) {
      auto* l = static_cast<arangodb::iresearch::IResearchInvertedIndexMock*>(
          index.get());
      if (!l->insert(trx, ref->second.docId(), newDocument).ok()) {
        return {TRI_ERROR_BAD_PARAMETER};
      }
      continue;
    }
    TRI_ASSERT(false);
  }
  auto* state = arangodb::basics::downCast<TransactionStateMock>(trx.state());
  TRI_ASSERT(state != nullptr);
  state->incrementInsert();

  return {};
}

arangodb::Result PhysicalCollectionMock::lookupKey(
    arangodb::transaction::Methods*, std::string_view key,
    std::pair<arangodb::LocalDocumentId, arangodb::RevisionId>& result,
    arangodb::ReadOwnWrites) const {
  before();

  auto it = _documents.find(key);
  if (it != _documents.end()) {
    result.first = it->second.docId();
    result.second = arangodb::RevisionId::fromSlice(it->second.data());
    return {};
  }

  result.first = arangodb::LocalDocumentId::none();
  result.second = arangodb::RevisionId::none();
  return {TRI_ERROR_ARANGO_DOCUMENT_NOT_FOUND};
}

arangodb::Result PhysicalCollectionMock::lookupKeyForUpdate(
    arangodb::transaction::Methods* methods, std::string_view key,
    std::pair<arangodb::LocalDocumentId, arangodb::RevisionId>& result) const {
  return lookupKey(methods, key, result, arangodb::ReadOwnWrites::yes);
}

uint64_t PhysicalCollectionMock::numberDocuments(
    arangodb::transaction::Methods*) const {
  before();
  return _documents.size();
}

bool PhysicalCollectionMock::addIndex(std::shared_ptr<arangodb::Index> idx) {
  auto const id = idx->id();
  for (auto const& it : _indexes) {
    if (it->id() == id) {
      // already have this particular index. do not add it again
      return false;
    }
  }

  TRI_UpdateTickServer(static_cast<TRI_voc_tick_t>(id.id()));

  _indexes.emplace(idx);
  return true;
}

void PhysicalCollectionMock::prepareIndexes(
    arangodb::velocypack::Slice indexesSlice) {
  before();

  auto& engine = _logicalCollection.vocbase()
                     .server()
                     .getFeature<arangodb::EngineSelectorFeature>()
                     .engine();
  auto& idxFactory = engine.indexFactory();

  for (VPackSlice v : VPackArrayIterator(indexesSlice)) {
    if (arangodb::basics::VelocyPackHelper::getBooleanValue(v, "error",
                                                            false)) {
      // We have an error here.
      // Do not add index.
      continue;
    }

    try {
      auto idx =
          idxFactory.prepareIndexFromSlice(v, false, _logicalCollection, true);

      if (!idx) {
        continue;
      }

      if (!addIndex(idx)) {
        return;
      }
    } catch (std::exception const&) {
      // error is just ignored here
    }
  }
}

arangodb::IndexEstMap PhysicalCollectionMock::clusterIndexEstimates(
    bool allowUpdating, arangodb::TransactionId tid) {
  TRI_ASSERT(arangodb::ServerState::instance()->isCoordinator());
  arangodb::IndexEstMap estimates;
  for (auto const& it : _indexes) {
    std::string id = std::to_string(it->id().id());
    if (it->hasSelectivityEstimate()) {
      // Note: This may actually be bad, as this instance cannot
      // have documents => The estimate is off.
      estimates.emplace(std::move(id), it->selectivityEstimate());
    } else {
      // Random hardcoded estimate. We do not actually know anything
      estimates.emplace(std::move(id), 0.25);
    }
  }
  return estimates;
}
arangodb::Result PhysicalCollectionMock::lookup(
    arangodb::transaction::Methods* trx, std::string_view key,
    arangodb::IndexIterator::DocumentCallback const& cb,
    LookupOptions options) const {
  before();
  auto it = _documents.find(key);
  if (it != _documents.end()) {
    cb(it->second.docId(), nullptr,
       arangodb::velocypack::Slice(it->second.vptr()));
    return arangodb::Result(TRI_ERROR_NO_ERROR);
  }
  return arangodb::Result(TRI_ERROR_ARANGO_DOCUMENT_NOT_FOUND);
}
arangodb::Result PhysicalCollectionMock::lookup(
    arangodb::transaction::Methods* trx, arangodb::LocalDocumentId token,
    arangodb::IndexIterator::DocumentCallback const& cb, LookupOptions options,
    arangodb::StorageSnapshot const* snapshot) const {
  before();
  for (auto const& entry : _documents) {
    auto& doc = entry.second;
    if (doc.docId() == token) {
      cb(token, nullptr, doc.data());
      return arangodb::Result{};
    }
  }
  return arangodb::Result{TRI_ERROR_ARANGO_DOCUMENT_NOT_FOUND};
}

arangodb::Result PhysicalCollectionMock::lookup(
    arangodb::transaction::Methods* trx,
    std::span<arangodb::LocalDocumentId> tokens,
    MultiDocumentCallback const& cb, LookupOptions options) const {
  before();
  for (auto token : tokens) {
    bool found = false;
    for (auto const& entry : _documents) {
      auto& doc = entry.second;
      if (doc.docId() == token) {
        cb(arangodb::Result{}, token, nullptr, doc.data());
        found = true;
        break;
      }
    }
    if (!found) {
      cb(arangodb::Result{TRI_ERROR_ARANGO_DOCUMENT_NOT_FOUND}, token, nullptr,
         {});
    }
  }
  return arangodb::Result{};
}

arangodb::Result PhysicalCollectionMock::remove(
    arangodb::transaction::Methods& trx,
    arangodb::IndexesSnapshot const& /*indexesSnapshot*/,
    arangodb::LocalDocumentId previousDocumentId,
    arangodb::RevisionId previousRevisionId,
    arangodb::velocypack::Slice previousDocument,
    arangodb::OperationOptions const& options) {
  before();

  std::string_view key;
  if (previousDocument.isString()) {
    key = previousDocument.stringView();
  } else {
    key = previousDocument.get(arangodb::StaticStrings::KeyString).stringView();
  }
  auto old = _documents.find(key);
  if (old != _documents.end()) {
    TRI_ASSERT(previousRevisionId ==
               arangodb::RevisionId::fromSlice(old->second.data()));
    _documents.erase(old);
    // TODO: removing the document from the mock collection
    // does not remove it from any mock indexes

    // assume document was removed
    auto* state = arangodb::basics::downCast<TransactionStateMock>(trx.state());
    TRI_ASSERT(state != nullptr);
    state->incrementRemove();
    return {};
  }
  return {TRI_ERROR_ARANGO_DOCUMENT_NOT_FOUND};
}

arangodb::Result PhysicalCollectionMock::update(
    arangodb::transaction::Methods& trx,
    arangodb::IndexesSnapshot const& /*indexesSnapshot*/,
    arangodb::LocalDocumentId newDocumentId,
    arangodb::RevisionId previousRevisionId,
    arangodb::velocypack::Slice previousDocument,
    arangodb::RevisionId newRevisionId, arangodb::velocypack::Slice newDocument,
    arangodb::OperationOptions const& options) {
  return updateInternal(trx, newDocumentId, previousRevisionId,
                        previousDocument, newRevisionId, newDocument, options,
                        /*isUpdate*/ true);
}

arangodb::Result PhysicalCollectionMock::replace(
    arangodb::transaction::Methods& trx,
    arangodb::IndexesSnapshot const& /*indexesSnapshot*/,
    arangodb::LocalDocumentId newDocumentId,
    arangodb::RevisionId previousRevisionId,
    arangodb::velocypack::Slice previousDocument,
    arangodb::RevisionId newRevisionId, arangodb::velocypack::Slice newDocument,
    arangodb::OperationOptions const& options) {
  return updateInternal(trx, newDocumentId, previousRevisionId,
                        previousDocument, newRevisionId, newDocument, options,
                        /*isUpdate*/ false);
}

arangodb::RevisionId PhysicalCollectionMock::revision(
    arangodb::transaction::Methods*) const {
  before();
  TRI_ASSERT(false);
  return arangodb::RevisionId::none();
}

arangodb::Result PhysicalCollectionMock::truncate(
    arangodb::transaction::Methods& trx, arangodb::OperationOptions& options,
    bool& usedRangeDelete) {
  before();
  _documents.clear();

  // should not matter what we set here
  usedRangeDelete = true;
  return {};
}

arangodb::Result PhysicalCollectionMock::updateInternal(
    arangodb::transaction::Methods& trx,
    arangodb::LocalDocumentId /*newDocumentId*/,
    arangodb::RevisionId previousRevisionId,
    arangodb::velocypack::Slice /*previousDocument*/,
    arangodb::RevisionId /*newRevisionId*/,
    arangodb::velocypack::Slice newDocument,
    arangodb::OperationOptions const& options, bool isUpdate) {
  TRI_ASSERT(newDocument.isObject());
  auto keySlice = newDocument.get(arangodb::StaticStrings::KeyString);
  if (!keySlice.isString()) {
    return {TRI_ERROR_ARANGO_DOCUMENT_HANDLE_BAD};
  }

  before();
  std::string_view key{keySlice.stringView()};
  auto it = _documents.find(key);
  if (it != _documents.end()) {
    auto doc = it->second.data();
    TRI_ASSERT(doc.isObject());

    // replace document
    auto newBuffer = std::make_shared<arangodb::velocypack::Buffer<uint8_t>>();
    newBuffer->append(newDocument.start(), newDocument.byteSize());
    // key is a string_view, and it must point into the storage space that
    // we own and that keeps valid
    key = arangodb::velocypack::Slice(newBuffer->data())
              .get(arangodb::StaticStrings::KeyString)
              .stringView();

    auto docId = it->second.docId();
    // must remove and insert, because our map's key type is a string_view.
    // the string_view could point to invalid memory if we change a map
    // entry's contents.
    _documents.erase(it);
    auto const& [ref, didInsert] =
        _documents.emplace(key, DocElement{std::move(newBuffer), docId.id()});
    TRI_ASSERT(didInsert);

    auto* state = arangodb::basics::downCast<TransactionStateMock>(trx.state());
    TRI_ASSERT(state != nullptr);
    state->incrementRemove();
    state->incrementInsert();

    // TODO: mock index entries are not updated here
    return {};
  }
  return {TRI_ERROR_ARANGO_DOCUMENT_NOT_FOUND};
}

arangodb::Result PhysicalCollectionMock::updateProperties(
    arangodb::velocypack::Slice slice) {
  before();

  return arangodb::Result(
      TRI_ERROR_NO_ERROR);  // assume mock collection updated OK
}
