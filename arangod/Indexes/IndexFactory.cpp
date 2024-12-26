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
/// @author Michael Hackstein
////////////////////////////////////////////////////////////////////////////////

#include "IndexFactory.h"
#include "ApplicationFeatures/ApplicationServer.h"
#include "Basics/AttributeNameParser.h"
#include "Basics/Exceptions.h"
#include "Basics/FloatingPoint.h"
#include "Basics/Result.h"
#include "Basics/StaticStrings.h"
#include "Basics/StringUtils.h"
#include "Basics/VelocyPackHelper.h"
#include "Cluster/ServerState.h"
#include "Indexes/Index.h"
#include "Indexes/VectorIndexDefinition.h"
#include "IResearch/IResearchCommon.h"
#include "Inspection/VPack.h"
#include "RestServer/BootstrapFeature.h"
#include "RestServer/DatabaseFeature.h"
#include "Utilities/NameValidator.h"
#include "VocBase/LogicalCollection.h"

#include <velocypack/Builder.h>
#include <velocypack/Iterator.h>
#include <velocypack/Slice.h>

#include <absl/strings/numbers.h>
#include <absl/strings/str_cat.h>

#include <limits.h>
#include <velocypack/Value.h>

#include <regex>
#include <string_view>

namespace {

using namespace arangodb;

struct InvalidIndexFactory : public IndexTypeFactory {
  InvalidIndexFactory(ArangodServer& server) : IndexTypeFactory(server) {}

  bool equal(velocypack::Slice, velocypack::Slice,
             std::string const&) const override {
    return false;  // invalid definitions are never equal
  }

  std::shared_ptr<Index> instantiate(LogicalCollection&,
                                     velocypack::Slice definition, IndexId,
                                     bool) const override {
    std::string type = basics::VelocyPackHelper::getStringValue(
        definition, StaticStrings::IndexType, "");
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_BAD_PARAMETER,
                                   "invalid index type '" + type + "'");
  }

  Result normalize(velocypack::Builder&, velocypack::Slice definition, bool,
                   TRI_vocbase_t const&) const override {
    std::string type = basics::VelocyPackHelper::getStringValue(
        definition, StaticStrings::IndexType, "");
    return Result(TRI_ERROR_BAD_PARAMETER, "invalid index type '" + type + "'");
  }
};

}  // namespace

namespace arangodb {
namespace helpers {

IndexId extractId(velocypack::Slice slice) noexcept {
  auto id = IndexId::none();
  if (ADB_UNLIKELY(!slice.isObject())) {
    return id;
  }
  auto value = slice.get(StaticStrings::IndexId);
  if (value.isNumber()) {
    id = IndexId{value.getNumber<IndexId::BaseType>()};
  } else if (value.isString()) {
    IndexId::BaseType base;
    if (absl::SimpleAtoi(value.stringView(), &base)) {
      id = IndexId{base};
    }
  }
  return id;
}

std::string_view extractName(velocypack::Slice slice) noexcept {
  if (ADB_UNLIKELY(!slice.isObject())) {
    return {};
  }
  if (auto value = slice.get(StaticStrings::IndexName); value.isString()) {
    return value.stringView();
  }
  return {};
}

}  // namespace helpers

IndexTypeFactory::IndexTypeFactory(ArangodServer& server) : _server(server) {}

bool IndexTypeFactory::equal(Index::IndexType type, velocypack::Slice lhs,
                             velocypack::Slice rhs,
                             bool attributeOrderMatters) const {
  // unique must be identical if present
  bool lhsUnique = basics::VelocyPackHelper::getBooleanValue(
      lhs, StaticStrings::IndexUnique, false);
  bool rhsUnique = basics::VelocyPackHelper::getBooleanValue(
      rhs, StaticStrings::IndexUnique, false);
  if (lhsUnique != rhsUnique) {
    return false;
  }

  // sparse must be identical if present
  if (Index::IndexType::TRI_IDX_TYPE_GEO2_INDEX != type &&
      Index::IndexType::TRI_IDX_TYPE_GEO1_INDEX != type &&
      Index::IndexType::TRI_IDX_TYPE_GEO_INDEX != type &&
      Index::IndexType::TRI_IDX_TYPE_FULLTEXT_INDEX != type) {
    bool lhsSparse = basics::VelocyPackHelper::getBooleanValue(
        lhs, StaticStrings::IndexSparse, false);
    bool rhsSparse = basics::VelocyPackHelper::getBooleanValue(
        rhs, StaticStrings::IndexSparse, false);
    if (lhsSparse != rhsSparse) {
      return false;
    }
  }

  VPackSlice value;

  if (Index::IndexType::TRI_IDX_TYPE_GEO1_INDEX == type ||
      Index::IndexType::TRI_IDX_TYPE_GEO_INDEX == type) {
    // geoJson must be identical if present
    value = lhs.get("geoJson");

    if (value.isBoolean() &&
        !basics::VelocyPackHelper::equal(value, rhs.get("geoJson"), false)) {
      return false;
    }
  } else if (Index::IndexType::TRI_IDX_TYPE_FULLTEXT_INDEX == type) {
    // minLength
    value = lhs.get("minLength");

    if (value.isNumber() &&
        !basics::VelocyPackHelper::equal(value, rhs.get("minLength"), false)) {
      return false;
    }
  } else if (Index::IndexType::TRI_IDX_TYPE_TTL_INDEX == type) {
    value = lhs.get(StaticStrings::IndexExpireAfter);

    if (value.isNumber() &&
        rhs.get(StaticStrings::IndexExpireAfter).isNumber()) {
      double const expireAfter = value.getNumber<double>();
      value = rhs.get(StaticStrings::IndexExpireAfter);

      if (!FloatingPoint<double>{expireAfter}.AlmostEquals(
              FloatingPoint<double>{value.getNumber<double>()})) {
        return false;
      }
    }
  } else if (Index::IndexType::TRI_IDX_TYPE_MDI_PREFIXED_INDEX == type) {
    value = lhs.get(StaticStrings::IndexPrefixFields);

    if (value.isArray() &&
        !basics::VelocyPackHelper::equal(
            value, rhs.get(StaticStrings::IndexPrefixFields), false)) {
      return false;
    }
  } else if (Index::IndexType::TRI_IDX_TYPE_VECTOR_INDEX == type) {
    // check if the parameters are the same
    if (!basics::VelocyPackHelper::equal(lhs.get("params"), rhs.get("params"),
                                         false)) {
      return false;
    }
  }

  // other index types: fields must be identical if present
  value = lhs.get(StaticStrings::IndexFields);

  if (value.isArray()) {
    if (!attributeOrderMatters) {
      // attributes can be specified in any order
      velocypack::ValueLength const nv = value.length();

      // compare fields in arbitrary order
      auto r = rhs.get(StaticStrings::IndexFields);

      if (!r.isArray() || nv != r.length()) {
        return false;
      }

      for (size_t i = 0; i < nv; ++i) {
        velocypack::Slice const v = value.at(i);

        bool found = false;

        for (VPackSlice vr : VPackArrayIterator(r)) {
          if (basics::VelocyPackHelper::equal(v, vr, false)) {
            found = true;
            break;
          }
        }

        if (!found) {
          return false;
        }
      }
    } else {
      // attribute order matters
      if (!basics::VelocyPackHelper::equal(
              value, rhs.get(StaticStrings::IndexFields), false)) {
        return false;
      }
    }
  }

  return true;
}

IndexFactory::IndexFactory(ArangodServer& server)
    : _server(server),
      _factories(),
      _invalid(std::make_unique<InvalidIndexFactory>(server)) {}

void IndexFactory::clear() { _factories.clear(); }

Result IndexFactory::emplace(std::string const& type,
                             IndexTypeFactory const& factory) {
  if (_server.hasFeature<BootstrapFeature>()) {
    auto& feature = _server.getFeature<BootstrapFeature>();
    // ensure new factories are not added at runtime since that would require
    // additional locks
    if (feature.isReady()) {
      return Result(TRI_ERROR_INTERNAL,
                    std::string("index factory registration is only "
                                "allowed during server startup"));
    }
  }

  if (!_factories.try_emplace(type, &factory).second) {
    return Result(
        TRI_ERROR_ARANGO_DUPLICATE_IDENTIFIER,
        std::string("index factory previously registered during index factory "
                    "registration for index type '") +
            type + "'");
  }

  return Result();
}

Result IndexFactory::enhanceIndexDefinition(  // normalize definition
    velocypack::Slice definition,             // source definition
    velocypack::Builder& normalized,  // normalized definition (out-param)
    bool isCreation,                  // definition for index creation
    TRI_vocbase_t const& vocbase      // index vocbase
) const {
  auto type = definition.get(StaticStrings::IndexType);

  if (!type.isString()) {
    return Result(TRI_ERROR_BAD_PARAMETER, "invalid index type");
  }

  auto& factory = IndexFactory::factory(type.copyString());

  TRI_ASSERT(normalized.isEmpty());

  try {
    velocypack::ObjectBuilder b(&normalized);
    auto const id = helpers::extractId(definition);
    if (id.isSet()) {
      absl::AlphaNum toStr{id.id()};
      normalized.add(StaticStrings::IndexId, velocypack::Value{toStr.Piece()});
    }

    std::string name{helpers::extractName(definition)};

    if (name.empty()) {
      // we should set the name for special types explicitly elsewhere,
      // but just in case...
      if (auto t = Index::type(type.stringView());
          t == Index::IndexType::TRI_IDX_TYPE_PRIMARY_INDEX) {
        name = StaticStrings::IndexNamePrimary;
      } else if (t == Index::IndexType::TRI_IDX_TYPE_EDGE_INDEX) {
        name = StaticStrings::IndexNameEdge;
      } else {
        // generate a name
        name = absl::StrCat("idx_", TRI_HybridLogicalClock());
      }
    }

    if (!name.empty()) {
      bool extendedNames = vocbase.extendedNames();
      if (auto res = IndexNameValidator::validateName(extendedNames, name);
          res.fail()) {
        return res;
      }
    }

    normalized.add(StaticStrings::IndexName, velocypack::Value(name));

    return factory.normalize(normalized, definition, isCreation, vocbase);
  } catch (basics::Exception const& ex) {
    return Result(ex.code(), ex.what());
  } catch (std::exception const& ex) {
    return Result(TRI_ERROR_INTERNAL, ex.what());
  } catch (...) {
    return Result(TRI_ERROR_INTERNAL, "unknown exception");
  }
}

IndexTypeFactory const& IndexFactory::factory(
    std::string const& type) const noexcept {
  auto itr = _factories.find(type);
  TRI_ASSERT(
      itr == _factories.end() ||
      false ==
          !(itr->second));  // IndexFactory::emplace(...) inserts non-nullptr

  return itr == _factories.end() ? *_invalid : *(itr->second);
}

std::shared_ptr<Index> IndexFactory::prepareIndexFromSlice(
    velocypack::Slice definition, bool generateKey,
    LogicalCollection& collection, bool isClusterConstructor) const {
  auto id = validateSlice(definition, generateKey, isClusterConstructor);
  auto type = definition.get(StaticStrings::IndexType);

  if (!type.isString()) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_BAD_PARAMETER,
                                   "invalid index type definition");
  }

  auto& factory = IndexFactory::factory(type.copyString());
  std::shared_ptr<Index> index =
      factory.instantiate(collection, definition, id, isClusterConstructor);

  if (!index) {
    THROW_ARANGO_EXCEPTION_MESSAGE(
        TRI_ERROR_INTERNAL,
        "failed to instantiate index, factory returned null instance");
  }

  return index;
}

/// same for both storage engines
std::vector<std::string_view> IndexFactory::supportedIndexes() const {
  return {
      "primary",
      "edge",
      "hash",
      "skiplist",
      "ttl",
      "persistent",
      "geo",
      "fulltext",
      "mdi",
      "mdi-prefixed",
      arangodb::iresearch::IRESEARCH_INVERTED_INDEX_TYPE,
      "vector",
  };
}

std::vector<std::pair<std::string_view, std::string_view>>
IndexFactory::indexAliases() const {
  return {};
}

IndexId IndexFactory::validateSlice(velocypack::Slice info, bool generateKey,
                                    bool isClusterConstructor) {
  if (!info.isObject()) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_BAD_PARAMETER,
                                   "expecting object for index definition");
  }
  auto const id = helpers::extractId(info);
  if (id.isSet()) {
    return id;
  }
  if (!generateKey) {
    // In the restore case it is forbidden to NOT have id
    THROW_ARANGO_EXCEPTION_MESSAGE(
        TRI_ERROR_BAD_PARAMETER,
        "cannot restore index without index identifier");
  }
  if (!isClusterConstructor) {
    // Restore is not allowed to generate an id
    VPackSlice type = info.get(StaticStrings::IndexType);
    // dont generate ids for indexes of type "primary"
    // id 0 is expected for primary indexes
    if (!type.isString() || !type.isEqualString("primary")) {
      TRI_ASSERT(generateKey);
      return Index::generateId();
    }
  }
  return IndexId::none();
}

Result IndexFactory::validateFieldsDefinition(
    VPackSlice definition, std::string const& attributeName, size_t minFields,
    size_t maxFields, bool allowSubAttributes, bool allowIdAttribute) {
  if (basics::VelocyPackHelper::getBooleanValue(definition,
                                                StaticStrings::Error, false)) {
    // We have an error here.
    return Result(TRI_ERROR_BAD_PARAMETER);
  }

  std::unordered_set<std::string_view> fields;
  auto fieldsSlice = definition.get(attributeName);
  auto const idxType = Index::type(
      definition.get(arangodb::StaticStrings::IndexType).stringView());
  auto const isInverted = Index::TRI_IDX_TYPE_INVERTED_INDEX == idxType;

  if (fieldsSlice.isArray()) {
    // "fields" is a list of fields
    for (VPackSlice it : VPackArrayIterator(fieldsSlice)) {
      if (!isInverted && it.isObject()) {
        return Result(TRI_ERROR_BAD_PARAMETER,
                      "index field names must be only strings");
      }
      auto fieldName = it.isObject() ? it.get("name") : it;
      if (!fieldName.isString()) {
        return Result(TRI_ERROR_BAD_PARAMETER,
                      "index field names must be non-empty strings");
      }

      std::string_view f = fieldName.stringView();

      if (f.empty()) {
        return Result(TRI_ERROR_BAD_PARAMETER,
                      "index field names must be non-empty strings");
      }

      if (fields.find(f) != fields.end()) {
        // duplicate attribute name
        return Result(TRI_ERROR_BAD_PARAMETER,
                      "duplicate attribute name in index fields list");
      }

      if (!allowSubAttributes && f.find('.') != std::string::npos) {
        return Result(TRI_ERROR_BAD_PARAMETER,
                      "cannot index a sub-attribute in this type of index");
      }

      if (!isInverted && !allowIdAttribute) {
        static std::regex const idRegex(
            "^(.+\\.)?" + StaticStrings::IdString + "$",
            std::regex::ECMAScript);
        if (std::regex_match(f.begin(), f.end(), idRegex)) {
          return Result(TRI_ERROR_BAD_PARAMETER,
                        "_id attribute cannot be indexed");
        }
      }

      fields.insert(f);
    }
  }

  size_t cc = fields.size();
  if (cc < minFields || cc > maxFields) {
    return Result(TRI_ERROR_BAD_PARAMETER,
                  "invalid number of index attributes");
  }

  return Result();
}

/// @brief process the fields list, deduplicate it, and add it to the json
Result IndexFactory::processIndexFields(VPackSlice definition,
                                        VPackBuilder& builder, size_t minFields,
                                        size_t maxFields, bool create,
                                        bool allowExpansion,
                                        bool allowSubAttributes,
                                        bool allowIdAttribute) {
  TRI_ASSERT(builder.isOpenObject());

  Result res = validateFieldsDefinition(definition, StaticStrings::IndexFields,
                                        minFields, maxFields,
                                        allowSubAttributes, allowIdAttribute);

  if (res.ok()) {
    auto fieldsSlice = definition.get(StaticStrings::IndexFields);

    TRI_ASSERT(fieldsSlice.isArray());

    builder.add(velocypack::Value(StaticStrings::IndexFields));
    builder.openArray();

    // "fields" is a list of fields when we have got here
    for (VPackSlice it : VPackArrayIterator(fieldsSlice)) {
      std::vector<basics::AttributeName> temp;
      TRI_ParseAttributeString(it.stringView(), temp, allowExpansion);

      builder.add(it);
    }

    builder.close();
  }

  return res;
}

/// process the stored values list, deduplicate it, and add it to the
/// json
Result IndexFactory::processIndexStoredValues(VPackSlice definition,
                                              VPackBuilder& builder,
                                              size_t minFields,
                                              size_t maxFields, bool create,
                                              bool allowSubAttributes,
                                              bool allowOverlappingFields) {
  TRI_ASSERT(builder.isOpenObject());

  Result res;

  auto fieldsSlice = definition.get(StaticStrings::IndexStoredValues);

  // storedValues are fully optional
  if (!fieldsSlice.isNone()) {
    if (fieldsSlice.isArray()) {
      res = validateFieldsDefinition(
          definition, StaticStrings::IndexStoredValues, minFields, maxFields,
          allowSubAttributes, /*allowIdAttribute*/ true);
      if (res.ok() && fieldsSlice.length() > 0) {
        std::unordered_set<std::string_view> fields;
        for (VPackSlice it : VPackArrayIterator(fieldsSlice)) {
          fields.insert(it.stringView());
        }
        auto normalFields = definition.get(StaticStrings::IndexFields);
        TRI_ASSERT(normalFields.isArray());
        for (VPackSlice it : VPackArrayIterator(normalFields)) {
          if (!allowOverlappingFields &&
              !fields.insert(it.stringView()).second) {
            res.reset(TRI_ERROR_BAD_PARAMETER,
                      "duplicate attribute name (overlap between index fields "
                      "and index "
                      "stored values list)");
          }
        }

        builder.add(velocypack::Value(StaticStrings::IndexStoredValues));
        builder.openArray();

        for (VPackSlice it : VPackArrayIterator(fieldsSlice)) {
          std::vector<basics::AttributeName> temp;
          TRI_ParseAttributeString(it.stringView(), temp,
                                   /*allowExpansion*/ false);

          builder.add(it);
        }

        builder.close();
      }
    } else {
      res.reset(TRI_ERROR_BAD_PARAMETER, "storedValues must be an array");
    }
  }

  return res.mapError([&](result::Error const& error) {
    return result::Error(
        error.errorNumber(),
        basics::StringUtils::concatT("when parsing fields for stored values: ",
                                     error.errorMessage()));
  });
}

void IndexFactory::processIndexCacheEnabled(VPackSlice definition,
                                            VPackBuilder& builder) {
  bool cacheEnabled = basics::VelocyPackHelper::getBooleanValue(
      definition, StaticStrings::CacheEnabled, false);
  builder.add(StaticStrings::CacheEnabled, VPackValue(cacheEnabled));
}

void IndexFactory::processIndexInBackground(VPackSlice definition,
                                            VPackBuilder& builder) {
  bool const bck = basics::VelocyPackHelper::getBooleanValue(
      definition, StaticStrings::IndexInBackground, false);
  builder.add(StaticStrings::IndexInBackground, VPackValue(bck));
}

void IndexFactory::processIndexParallelism(VPackSlice definition,
                                           VPackBuilder& builder) {
  if (definition.hasKey(StaticStrings::IndexParallelism)) {
    size_t const parallelism = std::clamp(
        basics::VelocyPackHelper::getNumericValue(
            definition, StaticStrings::IndexParallelism, kDefaultParallelism),
        size_t{1}, kMaxParallelism);

    builder.add(StaticStrings::IndexParallelism, VPackValue(parallelism));
  }
}

/// @brief process the unique flag and add it to the json
void IndexFactory::processIndexUniqueFlag(VPackSlice definition,
                                          VPackBuilder& builder) {
  bool unique = basics::VelocyPackHelper::getBooleanValue(
      definition, StaticStrings::IndexUnique.c_str(), false);

  builder.add(StaticStrings::IndexUnique, velocypack::Value(unique));
}

/// @brief process the sparse flag and add it to the json
void IndexFactory::processIndexSparseFlag(VPackSlice definition,
                                          VPackBuilder& builder, bool create) {
  if (definition.hasKey(StaticStrings::IndexSparse)) {
    bool sparseBool = basics::VelocyPackHelper::getBooleanValue(
        definition, StaticStrings::IndexSparse.c_str(), false);

    builder.add(StaticStrings::IndexSparse, velocypack::Value(sparseBool));
  } else if (create) {
    // not set. now add a default value
    builder.add(StaticStrings::IndexSparse, velocypack::Value(false));
  }
}

/// @brief process the deduplicate flag and add it to the json
void IndexFactory::processIndexDeduplicateFlag(VPackSlice definition,
                                               VPackBuilder& builder) {
  bool dup = basics::VelocyPackHelper::getBooleanValue(
      definition, StaticStrings::IndexDeduplicate, true);
  builder.add(StaticStrings::IndexDeduplicate, VPackValue(dup));
}

/// @brief process the geojson flag and add it to the json
void IndexFactory::processIndexGeoJsonFlag(VPackSlice definition,
                                           VPackBuilder& builder) {
  auto fieldsSlice = definition.get(StaticStrings::IndexFields);

  if (fieldsSlice.isArray() && fieldsSlice.length() == 1) {
    // only add geoJson for indexes with a single field (with needs to be an
    // array)
    bool geoJson =
        basics::VelocyPackHelper::getBooleanValue(definition, "geoJson", false);

    builder.add("geoJson", VPackValue(geoJson));
  }
}

/// @brief process the legacyPolygons flag and add it to the json
void IndexFactory::processIndexLegacyPolygonsFlag(VPackSlice definition,
                                                  VPackBuilder& builder) {
  bool legacyPolygons = basics::VelocyPackHelper::getBooleanValue(
      definition, StaticStrings::IndexLegacyPolygons, false);

  builder.add(StaticStrings::IndexLegacyPolygons, VPackValue(legacyPolygons));
}

/// @brief enhances the json of a persistent index (hash and skiplist are
/// aliases for this too)
Result IndexFactory::enhanceJsonIndexGeneric(VPackSlice definition,
                                             VPackBuilder& builder,
                                             bool create) {
  // "fields"
  Result res =
      processIndexFields(definition, builder, 1, INT_MAX, create,
                         /*allowExpansion*/ true, /*allowSubAttributes*/ true,
                         /*allowIdAttribute*/ false);

  if (res.ok()) {
    // "storedValues"
    res = processIndexStoredValues(definition, builder, 1, 32, create,
                                   /*allowSubAttributes*/ true,
                                   /* allowOverlappingFields */ false);
  }

  if (res.ok()) {
    // "sparse"
    processIndexSparseFlag(definition, builder, create);
    // "unique"
    processIndexUniqueFlag(definition, builder);
    // "deduplicate"
    processIndexDeduplicateFlag(definition, builder);
    // "inBackground"
    processIndexInBackground(definition, builder);
    // "parallelism"
    processIndexParallelism(definition, builder);
    // "cacheEnabled"
    processIndexCacheEnabled(definition, builder);
  }

  return res;
}

/// @brief enhances the json of a ttl index
Result IndexFactory::enhanceJsonIndexTtl(VPackSlice definition,
                                         VPackBuilder& builder, bool create) {
  Result res = processIndexFields(definition, builder, 1, 1, create,
                                  /*allowExpansion*/ false,
                                  /*allowSubAttributes*/ false,
                                  /*allowIdAttribute*/ false);

  auto value = definition.get(StaticStrings::IndexUnique);
  if (value.isBoolean() && value.getBoolean()) {
    return Result(TRI_ERROR_BAD_PARAMETER, "a TTL index cannot be unique");
  }

  if (res.ok()) {
    // a TTL index is always non-unique but sparse!
    builder.add(StaticStrings::IndexUnique, velocypack::Value(false));
    builder.add(StaticStrings::IndexSparse, velocypack::Value(true));

    VPackSlice v = definition.get(StaticStrings::IndexExpireAfter);
    if (!v.isNumber()) {
      return Result(TRI_ERROR_BAD_PARAMETER,
                    "expireAfter attribute must be a number");
    }
    double d = v.getNumericValue<double>();
    if (d < 0.0) {
      return Result(TRI_ERROR_BAD_PARAMETER,
                    "expireAfter attribute must greater equal to zero");
    }
    builder.add(StaticStrings::IndexExpireAfter, v);
    processIndexInBackground(definition, builder);
  }

  return res;
}

/// @brief enhances the json of a geo, geo1 or geo2 index
Result IndexFactory::enhanceJsonIndexGeo(VPackSlice definition,
                                         VPackBuilder& builder, bool create,
                                         int minFields, int maxFields) {
  Result res =
      processIndexFields(definition, builder, minFields, maxFields, create,
                         /*allowExpansion*/ false, /*allowSubAttributes*/ true,
                         /*allowIdAttribute*/ false);

  if (res.ok()) {
    builder.add(StaticStrings::IndexSparse, velocypack::Value(true));
    builder.add(StaticStrings::IndexUnique, velocypack::Value(false));
    IndexFactory::processIndexGeoJsonFlag(definition, builder);
    IndexFactory::processIndexLegacyPolygonsFlag(definition, builder);

    processIndexInBackground(definition, builder);
  }

  return res;
}

/// @brief enhances the json of a fulltext index
Result IndexFactory::enhanceJsonIndexFulltext(VPackSlice definition,
                                              VPackBuilder& builder,
                                              bool create) {
  Result res =
      processIndexFields(definition, builder, 1, 1, create,
                         /*allowExpansion*/ false, /*allowSubAttributes*/ true,
                         /*allowIdAttribute*/ false);

  if (res.ok()) {
    // hard-coded defaults
    builder.add(StaticStrings::IndexSparse, velocypack::Value(true));
    builder.add(StaticStrings::IndexUnique, velocypack::Value(false));

    // handle "minLength" attribute
    int minWordLength = FulltextIndexLimits::minWordLengthDefault;
    VPackSlice minLength = definition.get("minLength");

    if (minLength.isNumber()) {
      minWordLength = minLength.getNumericValue<int>();
    } else if (!minLength.isNull() && !minLength.isNone()) {
      return Result(TRI_ERROR_BAD_PARAMETER);
    }

    if (minWordLength <= 0) {
      minWordLength = 1;
    }

    builder.add("minLength", VPackValue(minWordLength));
    processIndexInBackground(definition, builder);
  }

  return res;
}

namespace {
Result processIndexSortedPrefixFields(VPackSlice definition,
                                      VPackBuilder& builder, size_t minFields,
                                      size_t maxFields, bool create,
                                      bool allowSubAttributes) {
  TRI_ASSERT(builder.isOpenObject());

  Result res;

  auto prefixFieldsSlice = definition.get(StaticStrings::IndexPrefixFields);

  if (prefixFieldsSlice.isArray() && !prefixFieldsSlice.isEmptyArray()) {
    res = IndexFactory::validateFieldsDefinition(
        definition, StaticStrings::IndexPrefixFields, minFields, maxFields,
        allowSubAttributes, /*allowIdAttribute*/ true);
    if (res.ok()) {
      std::unordered_set<std::string_view> fields;
      for (VPackSlice it : VPackArrayIterator(prefixFieldsSlice)) {
        fields.insert(it.stringView());
      }
      auto storedValues = definition.get(StaticStrings::IndexStoredValues);
      if (!storedValues.isNone()) {
        TRI_ASSERT(storedValues.isArray());
        for (VPackSlice it : VPackArrayIterator(storedValues)) {
          if (!fields.insert(it.stringView()).second) {
            res.reset(TRI_ERROR_BAD_PARAMETER,
                      "duplicate attribute name (overlap between index sorted "
                      "prefix fields "
                      "and index "
                      "stored values list)");
            break;
          }
        }
      }

      builder.add(velocypack::Value(StaticStrings::IndexPrefixFields));
      builder.openArray();

      for (VPackSlice it : VPackArrayIterator(prefixFieldsSlice)) {
        std::vector<basics::AttributeName> temp;
        TRI_ParseAttributeString(it.stringView(), temp,
                                 /*allowExpansion*/ false);

        builder.add(it);
      }

      builder.close();
    }
  } else {
    res.reset(TRI_ERROR_BAD_PARAMETER,
              "prefixFields is required for `mdi-prefixed`");
  }

  return res.mapError([&](result::Error const& error) {
    return result::Error(
        error.errorNumber(),
        basics::StringUtils::concatT("when parsing prefix fields: ",
                                     error.errorMessage()));
  });
}

}  // namespace

/// @brief enhances the json of a mdi index
Result IndexFactory::enhanceJsonIndexMdi(VPackSlice definition,
                                         VPackBuilder& builder, bool create) {
  if (auto fieldValueTypes = definition.get("fieldValueTypes");
      !fieldValueTypes.isString() || !fieldValueTypes.isEqualString("double")) {
    return Result(
        TRI_ERROR_BAD_PARAMETER,
        "mdi requires `fieldValueTypes` to be set to `double` - future "
        "releases might lift this requirement");
  }

  builder.add("fieldValueTypes", VPackValue("double"));
  Result res =
      processIndexFields(definition, builder, 1, INT_MAX, create,
                         /*allowExpansion*/ false, /*allowSubAttributes*/ true,
                         /*allowIdAttribute*/ false);

  if (res.ok()) {
    // "storedValues"
    // Since the indexed attributes are encoded and then bit interleaves
    // they can not be used for projections. Thus, we allow the same fields
    // to appear in the stored values for them to be projected out of the index.
    res = processIndexStoredValues(definition, builder, 1, 32, create,
                                   /*allowSubAttributes*/ true,
                                   /* allowOverlappingFields */ true);
  }

  if (res.ok()) {
    processIndexSparseFlag(definition, builder, create);
    processIndexUniqueFlag(definition, builder);
    processIndexInBackground(definition, builder);
  }

  return res;
}

/// @brief enhances the json of a mdi
Result IndexFactory::enhanceJsonIndexMdiPrefixed(VPackSlice definition,
                                                 VPackBuilder& builder,
                                                 bool create) {
  auto res = enhanceJsonIndexMdi(definition, builder, create);
  if (res.ok()) {
    res = processIndexSortedPrefixFields(definition, builder, 1, 32, create,
                                         true);
  }

  return res;
}

/// @brief enhances the json of a vector
Result IndexFactory::enhanceJsonIndexVector(
    arangodb::velocypack::Slice definition,
    arangodb::velocypack::Builder& builder, bool create) {
  auto const paramsSlice = definition.get("params");
  UserVectorIndexDefinition vectorIndexDefinition;
  if (auto const res =
          velocypack::deserializeWithStatus(paramsSlice, vectorIndexDefinition);
      !res.ok()) {
    return Result(TRI_ERROR_BAD_PARAMETER,
                  fmt::format("error: {}, path: {}", res.error(), res.path()));
  }

  if (definition.get("unique").isTrue()) {
    return {TRI_ERROR_BAD_PARAMETER, "Vector index cannot be unique"};
  }

  builder.add(VPackValue("params"));
  velocypack::serialize(builder, vectorIndexDefinition);
  Result const res =
      processIndexFields(definition, builder, 1, 1, create,
                         /*allowExpansion*/ false, /*allowSubAttributes*/ true,
                         /*allowIdAttribute*/ false);
  if (res.ok()) {
    // Vector index can be sparse
    processIndexSparseFlag(definition, builder, create);

    processIndexInBackground(definition, builder);

    processIndexParallelism(definition, builder);
  }

  return res;
}

}  // namespace arangodb
