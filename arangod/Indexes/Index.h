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
/// @author Jan Steemann
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include <iosfwd>
#include <string_view>

#include "Aql/AstNode.h"
#include "Basics/AttributeNameParser.h"
#include "Basics/Common.h"
#include "Basics/Exceptions.h"
#include "Basics/Result.h"
#include "Basics/StaticStrings.h"
#include "Containers/FlatHashSet.h"
#include "VocBase/Identifiers/IndexId.h"
#include "VocBase/Identifiers/LocalDocumentId.h"
#include "VocBase/voc-types.h"
#include "VocBase/vocbase.h"

namespace arangodb {
class IndexIterator;
class LogicalCollection;
struct IndexIteratorOptions;
struct ResourceMonitor;

namespace velocypack {
class Builder;
class Slice;
}  // namespace velocypack

namespace aql {
struct AttributeNamePath;
class Projections;
class SortCondition;
struct Variable;
}  // namespace aql

namespace transaction {
class Methods;
}

/// @brief static limits for fulltext index
struct FulltextIndexLimits {
  /// @brief maximum length of an indexed word in characters
  static constexpr int maxWordLength = 40;
  /// @brief default minimum word length for a fulltext index
  static constexpr int minWordLengthDefault = 2;
  /// @brief maximum number of search words in a query
  static constexpr int maxSearchWords = 32;
};

class Index {
 public:
  Index() = delete;
  Index(Index const&) = delete;
  Index& operator=(Index const&) = delete;

  Index(IndexId iid, LogicalCollection& collection, std::string const& name,
        std::vector<std::vector<basics::AttributeName>> const& fields,
        bool unique, bool sparse);

  Index(IndexId iid, LogicalCollection& collection, velocypack::Slice slice);

  virtual ~Index();

  static std::vector<std::vector<basics::AttributeName>> const
      emptyCoveredFields;

  /// @brief index types
  enum IndexType {
    TRI_IDX_TYPE_UNKNOWN = 0,
    TRI_IDX_TYPE_PRIMARY_INDEX,
    TRI_IDX_TYPE_GEO_INDEX,
    TRI_IDX_TYPE_GEO1_INDEX,
    TRI_IDX_TYPE_GEO2_INDEX,
    TRI_IDX_TYPE_HASH_INDEX,
    TRI_IDX_TYPE_EDGE_INDEX,
    TRI_IDX_TYPE_FULLTEXT_INDEX,
    TRI_IDX_TYPE_SKIPLIST_INDEX,
    TRI_IDX_TYPE_TTL_INDEX,
    TRI_IDX_TYPE_PERSISTENT_INDEX,
    TRI_IDX_TYPE_IRESEARCH_LINK,
    TRI_IDX_TYPE_NO_ACCESS_INDEX,
    TRI_IDX_TYPE_ZKD_INDEX,
    TRI_IDX_TYPE_INVERTED_INDEX
  };

  /// @brief: helper struct returned by index methods that determine the costs
  /// of index usage for filtering
  struct FilterCosts {
    /// @brief whether or not the index supports the filter condition
    bool supportsCondition = false;

    /// @brief number of attributes of filter condition covered by this index
    size_t coveredAttributes = 0;

    /// @brief estimated items to be returned for this condition.
    size_t estimatedItems = 0;

    /// @brief estimated costs for this filter condition
    double estimatedCosts = 0.0;

    static FilterCosts zeroCosts();

    static FilterCosts defaultCosts(size_t itemsInIndex, size_t numLookups = 1);
  };

  /// @brief: helper struct returned by index methods that determine the costs
  /// of index usage
  struct SortCosts {
    /// @brief whether or not the index supports the sort clause
    bool supportsCondition = false;

    /// @brief number of attributes of the sort clause covered by this index
    size_t coveredAttributes = 0;

    /// @brief estimated costs for this sort clause
    double estimatedCosts = 0.0;

    static SortCosts zeroCosts(size_t coveredAttributes);

    static SortCosts defaultCosts(size_t itemsInIndex);
  };

  /// @brief return the index id
  IndexId id() const { return _iid; }

  /// @brief return the index name
  std::string const& name() const {
    if (_name == StaticStrings::IndexNameEdgeFrom ||
        _name == StaticStrings::IndexNameEdgeTo) {
      return StaticStrings::IndexNameEdge;
    }
    return _name;
  }

  /// @brief set the name, if it is currently unset
  void name(std::string const&);

  /// @brief return the index fields
  std::vector<std::vector<basics::AttributeName>> const& fields() const {
    return _fields;
  }

  /// @brief return the fields covered by this index.
  ///        Typically just the fields, but e.g. EdgeIndex on _from also covers
  ///        _to
  virtual std::vector<std::vector<basics::AttributeName>> const& coveredFields()
      const {
    return _fields;
  }

  /// @brief return the index fields names
  std::vector<std::vector<std::string>> fieldNames() const {
    std::vector<std::vector<std::string>> result;
    result.reserve(_fields.size());

    for (auto const& it : _fields) {
      std::vector<std::string> parts;
      parts.reserve(it.size());
      for (auto const& it2 : it) {
        parts.emplace_back(it2.name);
      }
      result.emplace_back(std::move(parts));
    }
    return result;
  }

  /// @brief whether or not the ith attribute is expanded (somewhere)
  inline bool isAttributeExpanded(size_t i) const {
    if (i >= _fields.size()) {
      return false;
    }
    return TRI_AttributeNamesHaveExpansion(_fields[i]);
  }

  /// @brief whether or not any attribute is expanded
  inline bool isAttributeExpanded(
      std::vector<basics::AttributeName> const& attribute) const {
    for (auto const& it : _fields) {
      if (!basics::AttributeName::namesMatch(attribute, it)) {
        continue;
      }
      return TRI_AttributeNamesHaveExpansion(it);
    }
    return false;
  }

  /// @brief whether or not any attribute is expanded
  inline bool attributeMatches(
      std::vector<basics::AttributeName> const& attribute,
      bool isPrimary = false) const {
    for (auto const& it : _fields) {
      if (basics::AttributeName::isIdentical(attribute, it, true)) {
        return true;
      }
    }
    if (isPrimary) {
      static std::vector<basics::AttributeName> const vec_id{
          {StaticStrings::IdString, false}};
      return basics::AttributeName::isIdentical(attribute, vec_id, true);
    }
    return false;
  }

  /// @brief whether or not any attribute is expanded
  inline bool hasExpansion() const { return _useExpansion; }

  /// @brief if index needs explicit reversal and wouldn`t be reverted by
  /// storage rollback
  virtual bool needsReversal() const { return false; }

  /// @brief whether or not the index covers all the attributes passed in.
  /// the function may modify the projections by setting the
  /// coveringIndexPosition value in it.
  virtual bool covers(aql::Projections& projections) const;

  virtual size_t numFieldsToConsiderInIndexSelection() const noexcept {
    return _fields.size();
  }

  /// @brief return the underlying collection
  inline LogicalCollection& collection() const { return _collection; }

  /// @brief return a contextual string for logging
  std::string context() const;

  /// @brief whether or not the index is sparse
  inline bool sparse() const { return _sparse; }

  /// @brief whether or not the index is unique
  inline bool unique() const { return _unique; }

  /// @brief validates that field names don't start or end with ":"
  static void validateFieldsWithSpecialCase(velocypack::Slice fields);

  /// @brief validate fields from slice
  static void validateFields(velocypack::Slice slice);

  /// @brief return the name of the index
  char const* oldtypeName() const { return oldtypeName(type()); }

  /// @brief return the index type based on a type name
  static IndexType type(std::string_view type);

  /// @brief checks if the index could be used without explicit hint
  static bool onlyHintForced(IndexType type);

  virtual char const* typeName() const = 0;

  static bool allowExpansion(IndexType type) {
    return (type == TRI_IDX_TYPE_HASH_INDEX ||
            type == TRI_IDX_TYPE_SKIPLIST_INDEX ||
            type == TRI_IDX_TYPE_PERSISTENT_INDEX ||
            type == TRI_IDX_TYPE_INVERTED_INDEX);
  }

  virtual IndexType type() const = 0;

  /// @brief return the name of an index type
  static char const* oldtypeName(IndexType);

  /// @brief validate an index handle (collection name + / + index id)
  static bool validateHandle(bool extendedNames,
                             std::string_view handle) noexcept;

  /// @brief validate an index handle (by name) (collection name + / + index
  /// name)
  static bool validateHandleName(bool extendedNames,
                                 std::string_view name) noexcept;

  /// @brief validate an index id (i.e. ^[0-9]+$)
  static bool validateId(std::string_view id);

  /// @brief generate a new index id
  static IndexId generateId();

  /// @brief check if two index definitions share any identifiers (_id, name)
  static bool CompareIdentifiers(velocypack::Slice const& lhs,
                                 velocypack::Slice const& rhs);

  /// @brief index comparator, used by the coordinator to detect if two index
  /// contents are the same
  static bool Compare(StorageEngine&, velocypack::Slice const& lhs,
                      velocypack::Slice const& rhs, std::string const& dbname);

  static void normalizeFilterCosts(Index::FilterCosts& costs,
                                   Index const* index, size_t itemsInIndex,
                                   size_t invocations);

  virtual bool canBeDropped() const = 0;

  /// @brief Checks if this index is identical to the given definition
  virtual bool matchesDefinition(velocypack::Slice const&) const;

  /// @brief whether or not the index is sorted
  virtual bool isSorted() const = 0;

  /// @brief if true this index should not be shown externally
  virtual bool isHidden() const = 0;

  /// @brief if true this index should not be shown externally
  virtual bool inProgress() const { return false; }

  /// @brief whether or not the index has a selectivity estimate
  virtual bool hasSelectivityEstimate() const = 0;

  /// @brief return the selectivity estimate of the index
  /// must only be called if hasSelectivityEstimate() returns true
  ///
  /// The extra std::string_view is only used in the edge index
  /// as direction attribute attribute, a Slice would be more flexible.
  virtual double selectivityEstimate(
      std::string_view extra = std::string_view()) const;

  /// @brief update the cluster selectivity estimate
  virtual void updateClusterSelectivityEstimate(double /*estimate*/) {
    TRI_ASSERT(false);  // should never be called except on Coordinator
  }

  /// @brief whether or not the index is implicitly unique
  /// this can be the case if the index is not declared as unique,
  /// but contains a unique attribute such as _key
  bool implicitlyUnique() const;

  virtual size_t memory() const = 0;

  /// @brief serialization flags for indexes.
  /// note that these must be mutually exclusive when bit-ORed
  enum class Serialize : uint8_t {
    /// @brief serialize index
    Basics = 0,
    /// @brief serialize figures
    Figures = 2,
    /// @brief serialize selectivity estimates
    Estimates = 4,
    /// @brief serialize for persistence
    Internals = 8,
    /// @brief serialize for inventory
    Inventory = 16,
  };

  /// @brief helper for building flags
  template<typename... Args>
  static inline constexpr std::underlying_type<Serialize>::type makeFlags(
      Serialize flag, Args... args) {
    return static_cast<std::underlying_type<Serialize>::type>(flag) +
           makeFlags(args...);
  }

  static inline constexpr std::underlying_type<Serialize>::type makeFlags() {
    return static_cast<std::underlying_type<Serialize>::type>(
        Serialize::Basics);
  }

  static inline constexpr bool hasFlag(
      std::underlying_type<Serialize>::type flags, Serialize aflag) {
    return (flags &
            static_cast<std::underlying_type<Serialize>::type>(aflag)) != 0;
  }

  /// serialize an index to velocypack, using the serialization flags above
  virtual void toVelocyPack(
      velocypack::Builder&,
      std::underlying_type<Index::Serialize>::type flags) const;
  std::shared_ptr<velocypack::Builder> toVelocyPack(
      std::underlying_type<Serialize>::type flags) const;

  virtual void toVelocyPackFigures(velocypack::Builder&) const;

  virtual void load() = 0;
  virtual void unload() = 0;

  // called when the index is dropped
  virtual Result drop();

  virtual bool canBeUpdated() const noexcept { return false; }

  virtual Result updateProperties(VPackSlice) {
    return {TRI_ERROR_NOT_IMPLEMENTED};
  }

  /// @brief called after the collection was truncated
  /// @param tick at which truncate was applied
  virtual void afterTruncate(TRI_voc_tick_t, transaction::Methods*) {}

  /// @brief whether or not the filter condition is supported by the index
  /// returns detailed information about the costs associated with using this
  /// index
  virtual FilterCosts supportsFilterCondition(
      transaction::Methods& trx,
      std::vector<std::shared_ptr<Index>> const& allIndexes,
      aql::AstNode const* node, aql::Variable const* reference,
      size_t itemsInIndex) const;

  /// @brief whether or not the sort condition is supported by the index
  /// returns detailed information about the costs associated with using this
  /// index
  virtual SortCosts supportsSortCondition(
      aql::SortCondition const* sortCondition, aql::Variable const* reference,
      size_t itemsInIndex) const;

  /// @brief specialize the condition for use with this index. this will remove
  /// all elements from the condition that are not supported by the index. for
  /// example, if the condition is `doc.value1 == 38 && doc.value2 > 9`, but the
  /// index is only on `doc.value1`, this will return a new AstNode that points
  /// to just the condition `doc.value1 == 38`. must only be called if
  /// supportsFilterCondition has indicated that the index supports at least a
  /// part of the filter condition
  virtual aql::AstNode* specializeCondition(
      transaction::Methods& trx, aql::AstNode* node,
      aql::Variable const* reference) const;

  /// @brief create a new index iterator for the (specialized) condition
  virtual std::unique_ptr<IndexIterator> iteratorForCondition(
      ResourceMonitor& monitor, transaction::Methods* trx,
      aql::AstNode const* node, aql::Variable const* reference,
      IndexIteratorOptions const& opts, ReadOwnWrites readOwnWrites,
      int mutableConditionIdx);

  bool canUseConditionPart(
      aql::AstNode const* access, aql::AstNode const* other,
      aql::AstNode const* op, aql::Variable const* reference,
      containers::FlatHashSet<std::string>& nonNullAttributes, bool) const;

  virtual Result scheduleWarmup();

  static size_t sortWeight(aql::AstNode const* node);

 protected:
  static std::vector<std::vector<basics::AttributeName>> parseFields(
      velocypack::Slice fields, bool allowEmpty, bool allowExpansion);

  static std::vector<std::vector<basics::AttributeName>> mergeFields(
      std::vector<std::vector<basics::AttributeName>> const& fields1,
      std::vector<std::vector<basics::AttributeName>> const& fields2);

  /// @brief return the name of the (sole) index attribute
  /// it is only allowed to call this method if the index contains a
  /// single attribute
  std::string const& getAttribute() const;

  /// @brief generate error result
  /// @param code the error key
  /// @param key the conflicting key
  Result& addErrorMsg(Result& r, ErrorCode code,
                      std::string const& key = "") const {
    if (code != TRI_ERROR_NO_ERROR) {
      r.reset(code);
      return addErrorMsg(r, key);
    }
    return r;
  }

  /// @brief generate error result
  /// @param key the conflicting key
  Result& addErrorMsg(Result& r, std::string const& key = "") const;
  void addErrorMsg(result::Error& err, std::string const& key) const;

  /// @brief extracts a timestamp value from a document
  /// returns a negative value if the document does not contain the specified
  /// attribute, or the attribute does not contain a valid timestamp or date
  /// string
  double getTimestamp(velocypack::Slice const& doc,
                      std::string const& attributeName) const;

  IndexId const _iid;
  LogicalCollection& _collection;
  std::string _name;
  std::vector<std::vector<basics::AttributeName>> const _fields;
  bool const _useExpansion;

  mutable bool _unique;
  mutable bool _sparse;
};

/// @brief simple struct that takes an AstNode of type comparison and
/// splits it into the comparison operator, the attribute access and the
/// lookup value parts
/// only works for conditions such as  a.b == 2   or   45 < a.xx.c
/// the collection variable (a in the above examples) is passed in "variable"
struct AttributeAccessParts {
  AttributeAccessParts(aql::AstNode const* comparison,
                       aql::Variable const* variable);

  /// @brief comparison operation, e.g. NODE_TYPE_OPERATOR_BINARY_EQ
  aql::AstNode const* comparison;

  /// @brief attribute access node
  aql::AstNode const* attribute;

  /// @brief lookup value
  aql::AstNode const* value;

  /// @brief operation type
  aql::AstNodeType opType;
};

}  // namespace arangodb

std::ostream& operator<<(std::ostream&, arangodb::Index const*);
std::ostream& operator<<(std::ostream&, arangodb::Index const&);
