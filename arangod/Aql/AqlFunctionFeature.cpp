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
/// @author Michael Hackstein
////////////////////////////////////////////////////////////////////////////////

#include "AqlFunctionFeature.h"

#include "ApplicationFeatures/ApplicationServer.h"
#include "Aql/AstNode.h"
#include "Aql/Function.h"
#include "Basics/StringUtils.h"
#include "Cluster/ServerState.h"
#include "StorageEngine/EngineSelectorFeature.h"
#include "StorageEngine/StorageEngine.h"

using namespace arangodb::application_features;

namespace arangodb {
namespace aql {

using FF = Function::Flags;

AqlFunctionFeature::AqlFunctionFeature(Server& server)
    : ArangodFeature{server, *this} {
  setOptional(false);
  startsAfter<V8FeaturePhase>();
  startsAfter<AqlFeature>();
}

void AqlFunctionFeature::prepare() {
  /// @brief Add all AQL functions to the FunctionDefintions map
  addTypeCheckFunctions();
  addTypeCastFunctions();
  addStringFunctions();
  addNumericFunctions();
  addListFunctions();
  addDocumentFunctions();
  addGeoFunctions();
  addGeometryConstructors();
  addDateFunctions();
  addMiscFunctions();
}

void AqlFunctionFeature::add(Function const& func) {
  TRI_ASSERT(func.name == basics::StringUtils::toupper(func.name));
  TRI_ASSERT(_functionNames.find(func.name) == _functionNames.end());
  // add function to the map
  _functionNames.try_emplace(func.name, func);
}

void AqlFunctionFeature::addAlias(std::string const& alias,
                                  std::string const& original) {
  auto it = _functionNames.find(original);
  TRI_ASSERT(it != _functionNames.end());

  // intentionally copy original function, as we want to give it another name
  Function aliasFunction = (*it).second;
  aliasFunction.name = alias;

  add(aliasFunction);
}

void AqlFunctionFeature::toVelocyPack(VPackBuilder& builder) const {
  builder.openArray();
  for (auto const& it : _functionNames) {
    if (it.second.hasFlag(FF::Internal)) {
      // don't serialize internal functions
      continue;
    }
    it.second.toVelocyPack(builder);
  }
  builder.close();
}

bool AqlFunctionFeature::exists(std::string const& name) const {
  auto it = _functionNames.find(name);

  return it != _functionNames.end();
}

Function const* AqlFunctionFeature::byName(std::string const& name) const {
  auto it = _functionNames.find(name);

  if (it == _functionNames.end()) {
    THROW_ARANGO_EXCEPTION_PARAMS(TRI_ERROR_QUERY_FUNCTION_NAME_UNKNOWN,
                                  name.c_str());
  }

  // return the address of the function
  return &((*it).second);
}

// meanings of the symbols in the function arguments list
// ------------------------------------------------------
//
// . = argument of any type (except collection)
// c = collection name, will be converted into list with documents
// h = collection name, will be converted into string
// , = next argument
// | = separates mandatory from optional arguments
// + = unlimited number of optional arguments of any type

void AqlFunctionFeature::addTypeCheckFunctions() {
  // common flags for all these functions
  auto flags = Function::makeFlags(
      FF::Deterministic, FF::Cacheable, FF::CanRunOnDBServerCluster,
      FF::CanRunOnDBServerOneShard, FF::CanUseInAnalyzer);

  // type check functions
  add({"IS_NULL", ".", flags, &Functions::IsNull});
  add({"IS_BOOL", ".", flags, &Functions::IsBool});
  add({"IS_NUMBER", ".", flags, &Functions::IsNumber});
  add({"IS_STRING", ".", flags, &Functions::IsString});
  add({"IS_ARRAY", ".", flags, &Functions::IsArray});
  // IS_LIST is an alias for IS_ARRAY
  addAlias("IS_LIST", "IS_ARRAY");
  add({"IS_OBJECT", ".", flags, &Functions::IsObject});
  // IS_DOCUMENT is an alias for IS_OBJECT
  addAlias("IS_DOCUMENT", "IS_OBJECT");

  add({"IS_DATESTRING", ".", flags, &Functions::IsDatestring});
  add({"IS_IPV4", ".", flags, &Functions::IsIpV4});
  add({"IS_KEY", ".", flags, &Functions::IsKey});
  add({"TYPENAME", ".", flags, &Functions::Typename});
}

void AqlFunctionFeature::addTypeCastFunctions() {
  // common flags for all these functions
  auto flags = Function::makeFlags(
      FF::Deterministic, FF::Cacheable, FF::CanRunOnDBServerCluster,
      FF::CanRunOnDBServerOneShard, FF::CanUseInAnalyzer);

  // type cast functions
  add({"TO_NUMBER", ".", flags, &Functions::ToNumber});
  add({"TO_STRING", ".", flags, &Functions::ToString});
  add({"TO_BOOL", ".", flags, &Functions::ToBool});
  add({"TO_ARRAY", ".", flags, &Functions::ToArray});
  // TO_LIST is an alias for TO_ARRAY
  addAlias("TO_LIST", "TO_ARRAY");
}

void AqlFunctionFeature::addStringFunctions() {
  // common flags for all these functions
  auto flags = Function::makeFlags(
      FF::Deterministic, FF::Cacheable, FF::CanRunOnDBServerCluster,
      FF::CanRunOnDBServerOneShard, FF::CanUseInAnalyzer);

  // string functions
  add({"CONCAT", ".|+", flags, &Functions::Concat});
  add({"CONCAT_SEPARATOR", ".,.|+", flags, &Functions::ConcatSeparator});
  add({"CHAR_LENGTH", ".", flags, &Functions::CharLength});
  add({"LOWER", ".", flags, &Functions::Lower});
  add({"UPPER", ".", flags, &Functions::Upper});
  add({"SUBSTRING", ".,.|.", flags, &Functions::Substring});
  add({"CONTAINS", ".,.|.", flags, &Functions::Contains});
  add({"LIKE", ".,.|.", flags, &Functions::Like});
  add({"REGEX_MATCHES", ".,.|.", flags, &Functions::RegexMatches});
  add({"REGEX_SPLIT", ".,.|.,.", flags, &Functions::RegexSplit});
  add({"REGEX_TEST", ".,.|.", flags, &Functions::RegexTest});
  add({"REGEX_REPLACE", ".,.,.|.", flags, &Functions::RegexReplace});
  add({"LEFT", ".,.", flags, &Functions::Left});
  add({"RIGHT", ".,.", flags, &Functions::Right});
  add({"TRIM", ".|.", flags, &Functions::Trim});
  add({"LTRIM", ".|.", flags, &Functions::LTrim});
  add({"RTRIM", ".|.", flags, &Functions::RTrim});
  add({"FIND_FIRST", ".,.|.,.", flags, &Functions::FindFirst});
  add({"FIND_LAST", ".,.|.,.", flags, &Functions::FindLast});
  add({"SPLIT", ".|.,.", flags, &Functions::Split});
  add({"SUBSTITUTE", ".,.|.,.", flags, &Functions::Substitute});
  add({"IPV4_TO_NUMBER", ".", flags, &Functions::IpV4ToNumber});
  add({"IPV4_FROM_NUMBER", ".", flags, &Functions::IpV4FromNumber});
  add({"MD5", ".", flags, &Functions::Md5});
  add({"SHA1", ".", flags, &Functions::Sha1});
  add({"SHA512", ".", flags, &Functions::Sha512});
  add({"CRC32", ".", flags, &Functions::Crc32});
  add({"FNV64", ".", flags, &Functions::Fnv64});
  add({"HASH", ".", flags, &Functions::Hash});
  add({"TO_BASE64", ".", flags, &Functions::ToBase64});
  add({"TO_HEX", ".", flags, &Functions::ToHex});
  add({"ENCODE_URI_COMPONENT", ".", flags, &Functions::EncodeURIComponent});
  add({"SOUNDEX", ".", flags, &Functions::Soundex});
  add({"LEVENSHTEIN_DISTANCE", ".,.", flags, &Functions::LevenshteinDistance});
  add({"LEVENSHTEIN_MATCH", ".,.,.|.,.,.", flags,
       &Functions::LevenshteinMatch});  // (attribute, target, max distance,
                                        // [include transpositions, max terms,
                                        // prefix])

  auto flagsNoAnalyzer = Function::makeFlags(FF::Deterministic, FF::Cacheable,
                                             FF::CanRunOnDBServerCluster,
                                             FF::CanRunOnDBServerOneShard);

  add({"NGRAM_MATCH", ".,.|.,.", flagsNoAnalyzer,
       &Functions::NgramMatch});  // (attribute, target, [threshold, analyzer])
                                  // OR (attribute, target, [analyzer])
  add({"NGRAM_SIMILARITY", ".,.,.", flags,
       &Functions::NgramSimilarity});  // (attribute, target, ngram size)
  add({"NGRAM_POSITIONAL_SIMILARITY", ".,.,.", flags,
       &Functions::NgramPositionalSimilarity});  // (attribute, target, ngram
                                                 // size)
  add({"IN_RANGE", ".,.,.,.,.", flags,
       &Functions::InRange});  // (attribute, lower, upper, include lower,
                               // include upper)

  // special flags:
  // not deterministic and not cacheable
  auto nonDeterministicFlags =
      Function::makeFlags(FF::CanRunOnDBServerCluster,
                          FF::CanRunOnDBServerOneShard, FF::CanUseInAnalyzer);
  add({"RANDOM_TOKEN", ".", nonDeterministicFlags, &Functions::RandomToken});
  add({"UUID", "", nonDeterministicFlags, &Functions::Uuid});
}

void AqlFunctionFeature::addNumericFunctions() {
  // common flags for all these functions
  auto flags = Function::makeFlags(
      FF::Deterministic, FF::Cacheable, FF::CanRunOnDBServerCluster,
      FF::CanRunOnDBServerOneShard, FF::CanUseInAnalyzer);

  // numeric functions
  add({"FLOOR", ".", flags, &Functions::Floor});
  add({"CEIL", ".", flags, &Functions::Ceil});
  add({"ROUND", ".|.,.", flags, &Functions::Round});
  add({"ABS", ".", flags, &Functions::Abs});
  add({"SQRT", ".", flags, &Functions::Sqrt});
  add({"POW", ".,.", flags, &Functions::Pow});
  add({"LOG", ".", flags, &Functions::Log});
  add({"LOG2", ".", flags, &Functions::Log2});
  add({"LOG10", ".", flags, &Functions::Log10});
  add({"EXP", ".", flags, &Functions::Exp});
  add({"EXP2", ".", flags, &Functions::Exp2});
  add({"SIN", ".", flags, &Functions::Sin});
  add({"COS", ".", flags, &Functions::Cos});
  add({"TAN", ".", flags, &Functions::Tan});
  add({"ASIN", ".", flags, &Functions::Asin});
  add({"ACOS", ".", flags, &Functions::Acos});
  add({"ATAN", ".", flags, &Functions::Atan});
  add({"ATAN2", ".,.", flags, &Functions::Atan2});
  add({"RADIANS", ".", flags, &Functions::Radians});
  add({"DEGREES", ".", flags, &Functions::Degrees});
  add({"PI", "", flags, &Functions::Pi});

  add({"BIT_AND", ".|.", flags, &Functions::BitAnd});
  add({"BIT_OR", ".|.", flags, &Functions::BitOr});
  add({"BIT_XOR", ".|.", flags, &Functions::BitXOr});
  add({"BIT_NEGATE", ".,.", flags, &Functions::BitNegate});
  add({"BIT_TEST", ".,.", flags, &Functions::BitTest});
  add({"BIT_POPCOUNT", ".", flags, &Functions::BitPopcount});
  add({"BIT_SHIFT_LEFT", ".,.,.", flags, &Functions::BitShiftLeft});
  add({"BIT_SHIFT_RIGHT", ".,.,.", flags, &Functions::BitShiftRight});
  add({"BIT_CONSTRUCT", ".", flags, &Functions::BitConstruct});
  add({"BIT_DECONSTRUCT", ".", flags, &Functions::BitDeconstruct});
  add({"BIT_TO_STRING", ".,.", flags, &Functions::BitToString});
  add({"BIT_FROM_STRING", ".", flags, &Functions::BitFromString});

  // special flags:
  // not deterministic and not cacheable
  auto nonDeterministicFlags =
      Function::makeFlags(FF::CanRunOnDBServerCluster,
                          FF::CanRunOnDBServerOneShard, FF::CanUseInAnalyzer);
  add({"RAND", "", nonDeterministicFlags, &Functions::Rand});
}

void AqlFunctionFeature::addListFunctions() {
  // common flags for all these functions
  auto flags = Function::makeFlags(
      FF::Deterministic, FF::Cacheable, FF::CanRunOnDBServerCluster,
      FF::CanRunOnDBServerOneShard, FF::CanUseInAnalyzer);

  // list functions
  add({"RANGE", ".,.|.", flags, &Functions::Range});
  add({"UNION", ".,.|+", flags, &Functions::Union});
  add({"UNION_DISTINCT", ".,.|+", flags, &Functions::UnionDistinct});
  add({"MINUS", ".,.|+", flags, &Functions::Minus});
  add({"OUTERSECTION", ".,.|+", flags, &Functions::Outersection});
  add({"INTERSECTION", ".,.|+", flags, &Functions::Intersection});
  add({"JACCARD", ".,.", flags, &Functions::Jaccard});
  add({"FLATTEN", ".|.", flags, &Functions::Flatten});
  add({"LENGTH", ".", flags, &Functions::Length});
  // COUNT is an alias for LENGTH
  addAlias("COUNT", "LENGTH");
  add({"MIN", ".", flags, &Functions::Min});
  add({"MAX", ".", flags, &Functions::Max});
  add({"SUM", ".", flags, &Functions::Sum});
  add({"MEDIAN", ".", flags, &Functions::Median});
  add({"PERCENTILE", ".,.|.", flags, &Functions::Percentile});
  add({"AVERAGE", ".", flags, &Functions::Average});
  // AVG is an alias for AVERAGE
  addAlias("AVG", "AVERAGE");
  add({"VARIANCE_SAMPLE", ".", flags, &Functions::VarianceSample});
  add({"VARIANCE_POPULATION", ".", flags, &Functions::VariancePopulation});
  // VARIANCE is an alias for VARIANCE_POPULATION
  addAlias("VARIANCE", "VARIANCE_POPULATION");
  add({"STDDEV_SAMPLE", ".", flags, &Functions::StdDevSample});
  add({"STDDEV_POPULATION", ".", flags, &Functions::StdDevPopulation});
  // STDDEV is an alias for STDDEV_POPULATION
  addAlias("STDDEV", "STDDEV_POPULATION");
  add({"COUNT_DISTINCT", ".", flags, &Functions::CountDistinct});
  // COUNT_UNIQUE is an alias for COUNT_DISTINCT
  addAlias("COUNT_UNIQUE", "COUNT_DISTINCT");
  add({"PRODUCT", ".", flags, &Functions::Product});
  add({"UNIQUE", ".", flags, &Functions::Unique});
  add({"SORTED_UNIQUE", ".", flags, &Functions::SortedUnique});
  add({"SORTED", ".", flags, &Functions::Sorted});
  add({"SLICE", ".,.|.", flags, &Functions::Slice});
  add({"REVERSE", ".", flags, &Functions::Reverse});
  add({"FIRST", ".", flags, &Functions::First});
  add({"LAST", ".", flags, &Functions::Last});
  add({"NTH", ".,.", flags, &Functions::Nth});
  add({"POSITION", ".,.|.", flags, &Functions::Position});
  // CONTAINS_ARRAY is an alias for POSITION
  addAlias("CONTAINS_ARRAY", "POSITION");
  add({"PUSH", ".,.|.", flags, &Functions::Push});
  add({"APPEND", ".,.|.", flags, &Functions::Append});
  add({"POP", ".", flags, &Functions::Pop});
  add({"SHIFT", ".", flags, &Functions::Shift});
  add({"UNSHIFT", ".,.|.", flags, &Functions::Unshift});
  add({"REMOVE_VALUE", ".,.|.", flags, &Functions::RemoveValue});
  add({"REMOVE_VALUES", ".,.", flags, &Functions::RemoveValues});
  add({"REMOVE_NTH", ".,.", flags, &Functions::RemoveNth});
  add({"REPLACE_NTH", ".,.,.|.", flags, &Functions::ReplaceNth});
  add({"INTERLEAVE", ".,.|+", flags, &Functions::Interleave});

  add({"DECAY_GAUSS", ".,.,.,.,.,", flags, &Functions::DecayGauss});
  add({"DECAY_EXP", ".,.,.,.,.,", flags, &Functions::DecayExp});
  add({"DECAY_LINEAR", ".,.,.,.,.,", flags, &Functions::DecayLinear});

  add({"COSINE_SIMILARITY", ".,.", flags, &Functions::CosineSimilarity});
  add({"L1_DISTANCE", ".,.", flags, &Functions::L1Distance});
  add({"L2_DISTANCE", ".,.", flags, &Functions::L2Distance});
  // special flags:
  // CALL and APPLY will always run on the coordinator and are not deterministic
  // and not cacheable, as we don't know what function is actually gonna be
  // called. in addition, this may call any user-defined function, so we cannot
  // run this on DB servers
  add({"CALL", ".|.+", Function::makeFlags(), &Functions::Call});
  add({"APPLY", ".|.", Function::makeFlags(), &Functions::Apply});
}

void AqlFunctionFeature::addDocumentFunctions() {
  // common flags for all these functions
  auto flags = Function::makeFlags(
      FF::Deterministic, FF::Cacheable, FF::CanRunOnDBServerCluster,
      FF::CanRunOnDBServerOneShard, FF::CanUseInAnalyzer);

  // document functions
  add({"HAS", ".,.", flags, &Functions::Has});
  add({"ATTRIBUTES", ".|.,.", flags, &Functions::Attributes});
  // KEYS is an alias for ATTRIBUTES
  addAlias("KEYS", "ATTRIBUTES");
  add({"VALUES", ".|.", flags, &Functions::Values});
  add({"MERGE", ".|+", flags, &Functions::Merge});
  add({"MERGE_RECURSIVE", ".|+", flags, &Functions::MergeRecursive});
  add({"MATCHES", ".,.|.", flags, &Functions::Matches});
  add({"UNSET", ".,.|+", flags, &Functions::Unset});
  add({"UNSET_RECURSIVE", ".,.|+", flags, &Functions::UnsetRecursive});
  add({"KEEP", ".,.|+", flags, &Functions::Keep});
  add({"KEEP_RECURSIVE", ".,.|+", flags, &Functions::KeepRecursive});
  add({"TRANSLATE", ".,.|.", flags, &Functions::Translate});
  add({"ZIP", ".,.", flags, &Functions::Zip});
  add({"JSON_STRINGIFY", ".", flags, &Functions::JsonStringify});
  add({"JSON_PARSE", ".", flags, &Functions::JsonParse});

  // special flags:
  // not deterministic and non-cacheable, can only run on DB servers in OneShard
  // mode. not usable in analyzers
  auto documentFlags =
      Function::makeFlags(FF::CanRunOnDBServerOneShard, FF::CanReadDocuments);
  add({"DOCUMENT", "h.|.", documentFlags, &Functions::Document});
}

void AqlFunctionFeature::addGeoFunctions() {
  // common flags for all these functions
  auto flags = Function::makeFlags(
      FF::Deterministic, FF::Cacheable, FF::CanRunOnDBServerCluster,
      FF::CanRunOnDBServerOneShard, FF::CanUseInAnalyzer);

  // geo functions
  add({"DISTANCE", ".,.,.,.", flags, &Functions::Distance});
  add({"IS_IN_POLYGON", ".,.|.", flags, &Functions::IsInPolygon});
  add({"GEO_DISTANCE", ".,.|.", flags, &Functions::GeoDistance});
  add({"GEO_CONTAINS", ".,.", flags, &Functions::GeoContains});
  add({"GEO_INTERSECTS", ".,.", flags, &Functions::GeoIntersects});
  add({"GEO_EQUALS", ".,.", flags, &Functions::GeoEquals});
  add({"GEO_AREA", ".|.", flags, &Functions::GeoArea});

  // (point0, point1, lower, upper[, includeLower = true, includeUpper = true,
  // ellipsoid = "shpere"])
  add({"GEO_IN_RANGE", ".,.,.,.|.,.,.", flags, &Functions::GeoInRange});
}

void AqlFunctionFeature::addGeometryConstructors() {
  // common flags for all these functions
  auto flags = Function::makeFlags(
      FF::Deterministic, FF::Cacheable, FF::CanRunOnDBServerCluster,
      FF::CanRunOnDBServerOneShard, FF::CanUseInAnalyzer);

  // geometry types
  add({"GEO_POINT", ".,.", flags, &Functions::GeoPoint});
  add({"GEO_MULTIPOINT", ".", flags, &Functions::GeoMultiPoint});
  add({"GEO_POLYGON", ".", flags, &Functions::GeoPolygon});
  add({"GEO_MULTIPOLYGON", ".", flags, &Functions::GeoMultiPolygon});
  add({"GEO_LINESTRING", ".", flags, &Functions::GeoLinestring});
  add({"GEO_MULTILINESTRING", ".", flags, &Functions::GeoMultiLinestring});
}

void AqlFunctionFeature::addDateFunctions() {
  // common flags for all these functions
  auto flags = Function::makeFlags(
      FF::Deterministic, FF::Cacheable, FF::CanRunOnDBServerCluster,
      FF::CanRunOnDBServerOneShard, FF::CanUseInAnalyzer);

  // date functions
  add({"DATE_TIMESTAMP", ".|.,.,.,.,.,.", flags, &Functions::DateTimestamp});
  add({"DATE_ISO8601", ".|.,.,.,.,.,.", flags, &Functions::DateIso8601});
  add({"DATE_DAYOFWEEK", ".", flags, &Functions::DateDayOfWeek});
  add({"DATE_YEAR", ".", flags, &Functions::DateYear});
  add({"DATE_MONTH", ".", flags, &Functions::DateMonth});
  add({"DATE_DAY", ".", flags, &Functions::DateDay});
  add({"DATE_HOUR", ".", flags, &Functions::DateHour});
  add({"DATE_MINUTE", ".", flags, &Functions::DateMinute});
  add({"DATE_SECOND", ".", flags, &Functions::DateSecond});
  add({"DATE_MILLISECOND", ".", flags, &Functions::DateMillisecond});
  add({"DATE_DAYOFYEAR", ".", flags, &Functions::DateDayOfYear});
  add({"DATE_ISOWEEK", ".", flags, &Functions::DateIsoWeek});
  add({"DATE_LEAPYEAR", ".", flags, &Functions::DateLeapYear});
  add({"DATE_QUARTER", ".", flags, &Functions::DateQuarter});
  add({"DATE_DAYS_IN_MONTH", ".", flags, &Functions::DateDaysInMonth});
  add({"DATE_ADD", ".,.|.", flags, &Functions::DateAdd});
  add({"DATE_SUBTRACT", ".,.|.", flags, &Functions::DateSubtract});
  add({"DATE_DIFF", ".,.,.|.", flags, &Functions::DateDiff});
  add({"DATE_COMPARE", ".,.,.|.", flags, &Functions::DateCompare});
  add({"DATE_FORMAT", ".,.", flags, &Functions::DateFormat});
  add({"DATE_TRUNC", ".,.", flags, &Functions::DateTrunc});
  add({"DATE_UTCTOLOCAL", ".,.|.", flags, &Functions::DateUtcToLocal});
  add({"DATE_LOCALTOUTC", ".,.|.", flags, &Functions::DateLocalToUtc});
  add({"DATE_TIMEZONE", "", flags, &Functions::DateTimeZone});
  add({"DATE_TIMEZONES", "", flags, &Functions::DateTimeZones});
  add({"DATE_ROUND", ".,.,.", flags, &Functions::DateRound});

  // special flags:
  add({"DATE_NOW", "",
       Function::makeFlags(FF::Deterministic, FF::CanRunOnDBServerCluster,
                           FF::CanRunOnDBServerOneShard, FF::CanUseInAnalyzer),
       &Functions::DateNow});  // deterministic, but not cacheable!
}

void AqlFunctionFeature::addMiscFunctions() {
  // common flags for most of these functions
  auto flags = Function::makeFlags(
      FF::Deterministic, FF::Cacheable, FF::CanRunOnDBServerCluster,
      FF::CanRunOnDBServerOneShard, FF::CanUseInAnalyzer);

  // misc functions
  add({"PASSTHRU", ".", flags, &Functions::Passthru});
  add({"NOT_NULL", ".|+", flags, &Functions::NotNull});
  add({"FIRST_LIST", ".|+", flags, &Functions::FirstList});
  add({"FIRST_DOCUMENT", ".|+", flags, &Functions::FirstDocument});
  add({"PARSE_IDENTIFIER", ".", flags, &Functions::ParseIdentifier});
  add({"IS_SAME_COLLECTION", ".h,.h", flags, &Functions::IsSameCollection});
  add({"DECODE_REV", ".", flags, &Functions::DecodeRev});

  // cannot be used in analyzers
  add({"SHARD_ID", ".,.",
       Function::makeFlags(FF::CanRunOnDBServerCluster,
                           FF::CanRunOnDBServerOneShard),
       &Functions::ShardId});

  // only function without a C++ implementation. not usable in analyzers
  add({"V8", ".", Function::makeFlags(FF::Deterministic, FF::Cacheable),
       nullptr});

  // the following functions are not eligible to run on DB servers and not
  // in analyzers
  auto validationFlags = Function::makeFlags(FF::None);
  add({"SCHEMA_GET", ".", validationFlags, &Functions::SchemaGet});
  add({"SCHEMA_VALIDATE", ".,.", validationFlags, &Functions::SchemaValidate});

  // Call AIR. not usable in analyzers
  add({"CALL_GREENSPUN", ".|+",
       Function::makeFlags(FF::CanRunOnDBServerCluster,
                           FF::CanRunOnDBServerOneShard),
       &Functions::CallGreenspun});

  // special flags:
  // deterministic, not cacheable. only on coordinator. not in analyzers
  add({"VERSION", "", Function::makeFlags(FF::Deterministic),
       &Functions::Version});
  add({"FAIL", "|.",
       Function::makeFlags(FF::CanRunOnDBServerCluster,
                           FF::CanRunOnDBServerOneShard, FF::CanUseInAnalyzer),
       &Functions::Fail});  // not deterministic and not cacheable
  add({"NOOPT", ".",
       Function::makeFlags(FF::CanRunOnDBServerCluster,
                           FF::CanRunOnDBServerOneShard, FF::NoEval,
                           FF::CanUseInAnalyzer),
       &Functions::Passthru});  // prevents all optimizations!
  add({"NOEVAL", ".",
       Function::makeFlags(FF::Deterministic, FF::CanRunOnDBServerCluster,
                           FF::CanRunOnDBServerOneShard, FF::NoEval,
                           FF::CanUseInAnalyzer),
       &Functions::Passthru});  // prevents all optimizations!
  add({"SLEEP", ".",
       Function::makeFlags(FF::CanRunOnDBServerCluster,
                           FF::CanRunOnDBServerOneShard),
       &Functions::Sleep});  // not deterministic and not cacheable. not in
                             // analyzers
  add({"COLLECTIONS", "", Function::makeFlags(),
       &Functions::Collections});  // not deterministic and not cacheable
  add({"CURRENT_USER", "", Function::makeFlags(FF::Deterministic),
       &Functions::CurrentUser});  // deterministic, but not cacheable
  add({"CURRENT_DATABASE", "", Function::makeFlags(FF::Deterministic),
       &Functions::CurrentDatabase});  // deterministic, but not cacheable
  add({"CHECK_DOCUMENT", ".",
       Function::makeFlags(FF::CanRunOnDBServerCluster,
                           FF::CanRunOnDBServerOneShard, FF::CanUseInAnalyzer),
       &Functions::CheckDocument});  // not deterministic and not cacheable
  add({"COLLECTION_COUNT", ".h", Function::makeFlags(FF::CanReadDocuments),
       &Functions::CollectionCount});  // not deterministic and not cacheable
  add({"PREGEL_RESULT", ".|.",
       Function::makeFlags(FF::CanReadDocuments, FF::CanRunOnDBServerCluster,
                           FF::CanRunOnDBServerOneShard),
       &Functions::PregelResult});  // not deterministic and not cacheable
  add({"ASSERT", ".,.",
       Function::makeFlags(FF::CanRunOnDBServerCluster,
                           FF::CanRunOnDBServerOneShard, FF::CanUseInAnalyzer),
       &Functions::Assert});  // not deterministic and not cacheable
  add({"WARN", ".,.",
       Function::makeFlags(FF::CanRunOnDBServerCluster,
                           FF::CanRunOnDBServerOneShard, FF::CanUseInAnalyzer),
       &Functions::Warn});  // not deterministic and not cacheable

  // NEAR, WITHIN, WITHIN_RECTANGLE and FULLTEXT are replaced by the AQL
  // optimizer with collection-/index-based subqueries. they are all
  // marked as deterministic and cacheable here as they are just
  // placeholders for collection/index accesses nowaways.
  add({"NEAR", ".h,.,.|.,.", Function::makeFlags(FF::Cacheable),
       &Functions::NotImplemented});
  add({"WITHIN", ".h,.,.,.|.", Function::makeFlags(FF::Cacheable),
       &Functions::NotImplemented});
  add({"WITHIN_RECTANGLE", "h.,.,.,.,.", Function::makeFlags(FF::Cacheable),
       &Functions::NotImplemented});
  add({"FULLTEXT", ".h,.,.|.", Function::makeFlags(FF::Cacheable),
       &Functions::NotImplemented});

  add({"MAKE_DISTRIBUTE_INPUT", ".,.",
       Function::makeFlags(FF::Deterministic, FF::Cacheable, FF::Internal,
                           FF::CanRunOnDBServerCluster,
                           FF::CanRunOnDBServerOneShard),
       &Functions::MakeDistributeInput});
  add({"MAKE_DISTRIBUTE_INPUT_WITH_KEY_CREATION", ".,.,.",
       Function::makeFlags(FF::Internal),
       &Functions::MakeDistributeInputWithKeyCreation});
  add({"MAKE_DISTRIBUTE_GRAPH_INPUT", ".",
       Function::makeFlags(FF::Deterministic, FF::Cacheable, FF::Internal,
                           FF::CanRunOnDBServerCluster,
                           FF::CanRunOnDBServerOneShard),
       &Functions::MakeDistributeGraphInput});
#ifdef USE_ENTERPRISE
  add({"SELECT_SMART_DISTRIBUTE_GRAPH_INPUT", ".,.",
       Function::makeFlags(FF::Deterministic, FF::Cacheable, FF::Internal,
                           FF::CanRunOnDBServerCluster,
                           FF::CanRunOnDBServerOneShard),
       &Functions::SelectSmartDistributeGraphInput});
#endif

  // this is an internal function that is only here for testing. it cannot
  // be invoked by end users, because refering to internal functions from user
  // queries will pretend these functions do not exist.
  add({"INTERNAL", "", Function::makeFlags(FF::Internal),
       &Functions::NotImplemented});
}

}  // namespace aql
}  // namespace arangodb
