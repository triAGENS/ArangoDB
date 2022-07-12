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

#include "Functions.h"

#include "ApplicationFeatures/ApplicationServer.h"
#include "ApplicationFeatures/LanguageFeature.h"
#include "Aql/AqlFunctionFeature.h"
#include "Aql/AqlValueMaterializer.h"
#include "Aql/Expression.h"
#include "Aql/ExpressionContext.h"
#include "Aql/Function.h"
#include "Aql/Query.h"
#include "Aql/Range.h"
#include "Aql/V8Executor.h"
#include "Basics/Endian.h"
#include "Basics/Exceptions.h"
#include "Basics/HybridLogicalClock.h"
#include "Basics/Mutex.h"
#include "Basics/MutexLocker.h"
#include "Basics/NumberUtils.h"
#include "Basics/StringUtils.h"
#include "Basics/Utf8Helper.h"
#include "Basics/VelocyPackHelper.h"
#include "Basics/conversions.h"
#include "Basics/datetime.h"
#include "Basics/fpconv.h"
#include "Basics/hashes.h"
#include "Basics/system-functions.h"
#include "Basics/tri-strings.h"
#include "Containers/FlatHashSet.h"
#include "Geo/Ellipsoid.h"
#include "Geo/GeoJson.h"
#include "Geo/ShapeContainer.h"
#include "Geo/Utils.h"
#include "Greenspun/Interpreter.h"
#include "IResearch/IResearchAnalyzerFeature.h"
#include "IResearch/IResearchFilterFactory.h"
#include "IResearch/IResearchPDP.h"
#include "IResearch/VelocyPackHelper.h"
#include "Indexes/Index.h"
#include "Logger/Logger.h"
#include "Pregel/Conductor.h"
#include "Pregel/PregelFeature.h"
#include "Pregel/Worker.h"
#include "Random/UniformCharacter.h"
#include "Rest/Version.h"
#include "RestServer/SystemDatabaseFeature.h"
#include "Ssl/SslInterface.h"
#include "StorageEngine/TransactionState.h"
#include "Transaction/Context.h"
#include "Transaction/Helpers.h"
#include "Transaction/Methods.h"
#include "Utils/CollectionNameResolver.h"
#include "Utils/ExecContext.h"
#include "V8/v8-vpack.h"
#include "VocBase/KeyGenerator.h"
#include "VocBase/LogicalCollection.h"
#include "VocBase/Methods/Collections.h"
#include "VocBase/Validators.h"

#ifdef USE_ENTERPRISE
#include "Enterprise/VocBase/SmartGraphSchema.h"
#endif

#include "analysis/token_attributes.hpp"
#include "utils/levenshtein_utils.hpp"
#include "utils/ngram_match_utils.hpp"

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>

#include <date/date.h>
#include <date/iso_week.h>
#include <date/tz.h>
#include <s2/s2loop.h>

#include <unicode/schriter.h>
#include <unicode/stsearch.h>
#include <unicode/uchar.h>
#include <unicode/unistr.h>

#include <velocypack/Collection.h>
#include <velocypack/Dumper.h>
#include <velocypack/Iterator.h>
#include <velocypack/Sink.h>

#include <algorithm>

#ifdef __APPLE__
#include <regex>
#endif

#ifdef _WIN32
#include "Basics/win-utils.h"
#else
#include <arpa/inet.h>
#endif

using namespace arangodb;
using namespace basics;
using namespace aql;
using namespace std::chrono;

/*
- always specify your user facing function name MYFUNC in error generators
- errors are broadcasted like this:
    - Wrong parameter types: registerInvalidArgumentWarning(expressionContext,
"MYFUNC")
    - Generic errors: registerWarning(expressionContext, "MYFUNC",
TRI_ERROR_QUERY_INVALID_REGEX);
    - ICU related errors: if (U_FAILURE(status)) {
::registerICUWarning(expressionContext, "MYFUNC", status); }
    - close with: return AqlValue(AqlValueHintNull());
- specify the number of parameters you expect at least and at max using:
- if you support optional parameters, first check whether the count is
sufficient using parameters.size()
- fetch the values using:
  AqlValue value
  - Anonymous  = extractFunctionParameterValue(parameters, 0);
  - ::getBooleanParameter() if you expect a bool
  - Stringify() if you need a string.
  - ::extractKeys() if its an object and you need the keys
  - ::extractCollectionName() if you expect a collection
  - ::listContainsElement() search for a member
  - ::parameterToTimePoint / ::dateFromParameters get a time string as date.
- check the values whether they match your expectations i.e. using:
  - param.isNumber() then extract it using: param.toInt64()
- Available helper functions for working with parameters:
  - ::variance()
  - ::sortNumberList()
  - ::unsetOrKeep ()
  - ::getDocumentByIdentifier ()
  - ::mergeParameters()
  - ::flattenList()

- now do your work with the parameters
- build up a result using a VPackbuilder like you would with regular velocpyack.
- return it wrapping it into an AqlValue

 */

namespace {

/// @brief an empty AQL value
static AqlValue const emptyAqlValue;

#ifdef __APPLE__
std::regex const ipV4LeadingZerosRegex("^(.*?\\.)?0[0-9]+.*$",
                                       std::regex::optimize);
#endif

/// @brief mutex used to protect UUID generation
static Mutex uuidMutex;

enum DateSelectionModifier {
  INVALID = 0,
  MILLI,
  SECOND,
  MINUTE,
  HOUR,
  DAY,
  WEEK,
  MONTH,
  YEAR
};

static_assert(DateSelectionModifier::INVALID < DateSelectionModifier::MILLI,
              "incorrect date selection order");
static_assert(DateSelectionModifier::MILLI < DateSelectionModifier::SECOND,
              "incorrect date selection order");
static_assert(DateSelectionModifier::SECOND < DateSelectionModifier::MINUTE,
              "incorrect date selection order");
static_assert(DateSelectionModifier::MINUTE < DateSelectionModifier::HOUR,
              "incorrect date selection order");
static_assert(DateSelectionModifier::HOUR < DateSelectionModifier::DAY,
              "incorrect date selection order");
static_assert(DateSelectionModifier::DAY < DateSelectionModifier::WEEK,
              "incorrect date selection order");
static_assert(DateSelectionModifier::WEEK < DateSelectionModifier::MONTH,
              "incorrect date selection order");
static_assert(DateSelectionModifier::MONTH < DateSelectionModifier::YEAR,
              "incorrect date selection order");

/// @brief validates documents for duplicate attribute names
bool isValidDocument(VPackSlice slice) {
  slice = slice.resolveExternals();

  if (slice.isObject()) {
    containers::FlatHashSet<std::string_view> keys;

    auto it = VPackObjectIterator(slice, true);

    while (it.valid()) {
      if (!keys.emplace(it.key().stringView()).second) {
        // duplicate key
        return false;
      }

      // recurse into object values
      if (!isValidDocument(it.value())) {
        return false;
      }
      it.next();
    }
  } else if (slice.isArray()) {
    auto it = VPackArrayIterator(slice);

    while (it.valid()) {
      // recursively validate array values
      if (!isValidDocument(it.value())) {
        return false;
      }
      it.next();
    }
  }

  // all other types are considered valid
  return true;
}

void registerICUWarning(ExpressionContext* expressionContext,
                        char const* functionName, UErrorCode status) {
  std::string msg;
  msg.append("in function '");
  msg.append(functionName);
  msg.append("()': ");
  msg.append(basics::Exception::FillExceptionString(TRI_ERROR_ARANGO_ICU_ERROR,
                                                    u_errorName(status)));
  expressionContext->registerWarning(TRI_ERROR_ARANGO_ICU_ERROR, msg.c_str());
}

/// @brief extract a function parameter from the arguments
inline AqlValue const& extractFunctionParameterValue(
    VPackFunctionParametersView parameters, size_t position) {
  if (position >= parameters.size()) {
    // parameter out of range
    return ::emptyAqlValue;
  }
  return parameters[position];
}

/// @brief convert a number value into an AqlValue
AqlValue numberValue(double value, bool nullify) {
  if (std::isnan(value) || !std::isfinite(value) || value == HUGE_VAL ||
      value == -HUGE_VAL) {
    if (nullify) {
      // convert to null
      return AqlValue(AqlValueHintNull());
    }
    // convert to 0
    return AqlValue(AqlValueHintZero());
  }

  return AqlValue(AqlValueHintDouble(value));
}

/// @brief optimized version of datetime stringification
/// string format is hard-coded to YYYY-MM-DDTHH:MM:SS.XXXZ
AqlValue timeAqlValue(ExpressionContext* expressionContext, char const* AFN,
                      tp_sys_clock_ms const& tp, bool utc = true,
                      bool registerWarning = true) {
  char formatted[24];

  date::year_month_day ymd{floor<date::days>(tp)};
  auto day_time = date::make_time(tp - date::sys_days(ymd));

  auto y = static_cast<int>(ymd.year());
  // quick basic check here for dates outside the allowed range
  if (y < 0 || y > 9999) {
    if (registerWarning) {
      aql::registerWarning(expressionContext, AFN,
                           TRI_ERROR_QUERY_INVALID_DATE_VALUE);
    }
    return AqlValue(AqlValueHintNull());
  }

  formatted[0] = '0' + (y / 1000);
  formatted[1] = '0' + ((y % 1000) / 100);
  formatted[2] = '0' + ((y % 100) / 10);
  formatted[3] = '0' + (y % 10);
  formatted[4] = '-';
  auto m = static_cast<unsigned>(ymd.month());
  formatted[5] = '0' + (m / 10);
  formatted[6] = '0' + (m % 10);
  formatted[7] = '-';
  auto d = static_cast<unsigned>(ymd.day());
  formatted[8] = '0' + (d / 10);
  formatted[9] = '0' + (d % 10);
  formatted[10] = 'T';
  auto h = day_time.hours().count();
  formatted[11] = '0' + (h / 10);
  formatted[12] = '0' + (h % 10);
  formatted[13] = ':';
  auto i = day_time.minutes().count();
  formatted[14] = '0' + (i / 10);
  formatted[15] = '0' + (i % 10);
  formatted[16] = ':';
  auto s = day_time.seconds().count();
  formatted[17] = '0' + (s / 10);
  formatted[18] = '0' + (s % 10);
  formatted[19] = '.';
  uint64_t millis = day_time.subseconds().count();
  if (millis > 999) {
    millis = 999;
  }
  formatted[20] = '0' + (millis / 100);
  formatted[21] = '0' + ((millis % 100) / 10);
  formatted[22] = '0' + (millis % 10);
  formatted[23] = 'Z';

  return AqlValue(&formatted[0],
                  utc ? sizeof(formatted) : sizeof(formatted) - 1);
}

DateSelectionModifier parseDateModifierFlag(VPackSlice flag) {
  if (!flag.isString()) {
    return INVALID;
  }

  std::string flagStr = flag.copyString();
  if (flagStr.empty()) {
    return INVALID;
  }
  TRI_ASSERT(flagStr.size() >= 1);

  basics::StringUtils::tolowerInPlace(flagStr);
  switch (flagStr.front()) {
    case 'y':
      if (flagStr == "years" || flagStr == "year" || flagStr == "y") {
        return YEAR;
      }
      break;
    case 'w':
      if (flagStr == "weeks" || flagStr == "week" || flagStr == "w") {
        return WEEK;
      }
      break;
    case 'm':
      if (flagStr == "months" || flagStr == "month" || flagStr == "m") {
        return MONTH;
      }
      // Can be minute as well
      if (flagStr == "minutes" || flagStr == "minute") {
        return MINUTE;
      }
      // Can be millisecond as well
      if (flagStr == "milliseconds" || flagStr == "millisecond") {
        return MILLI;
      }
      break;
    case 'd':
      if (flagStr == "days" || flagStr == "day" || flagStr == "d") {
        return DAY;
      }
      break;
    case 'h':
      if (flagStr == "hours" || flagStr == "hour" || flagStr == "h") {
        return HOUR;
      }
      break;
    case 's':
      if (flagStr == "seconds" || flagStr == "second" || flagStr == "s") {
        return SECOND;
      }
      break;
    case 'i':
      if (flagStr == "i") {
        return MINUTE;
      }
      break;
    case 'f':
      if (flagStr == "f") {
        return MILLI;
      }
      break;
  }
  // If we get here the flag is invalid
  return INVALID;
}

AqlValue addOrSubtractUnitFromTimestamp(ExpressionContext* expressionContext,
                                        tp_sys_clock_ms const& tp,
                                        VPackSlice durationUnitsSlice,
                                        VPackSlice durationType,
                                        char const* AFN, bool isSubtract) {
  bool isInteger = durationUnitsSlice.isInteger();
  double durationUnits = durationUnitsSlice.getNumber<double>();
  std::chrono::duration<double, std::ratio<1l, 1000l>> ms{};
  date::year_month_day ymd{floor<date::days>(tp)};
  auto day_time = date::make_time(tp - date::sys_days(ymd));

  DateSelectionModifier flag = ::parseDateModifierFlag(durationType);
  double intPart = 0.0;
  if (durationUnits < 0.0) {
    // Make sure duration is always positive. So we flip isSubtract in this
    // case.
    isSubtract = !isSubtract;
    durationUnits *= -1.0;
  }
  TRI_ASSERT(durationUnits >= 0.0);

  // All Fallthroughs intentional. We still have some remainder
  switch (flag) {
    case YEAR:
      durationUnits = std::modf(durationUnits, &intPart);
      if (isSubtract) {
        ymd -= date::years{static_cast<int64_t>(intPart)};
      } else {
        ymd += date::years{static_cast<int64_t>(intPart)};
      }
      if (isInteger || durationUnits == 0.0) {
        break;  // We are done
      }
      durationUnits *= 12;
      [[fallthrough]];
    case MONTH:
      durationUnits = std::modf(durationUnits, &intPart);
      if (isSubtract) {
        ymd -= date::months{static_cast<int64_t>(intPart)};
      } else {
        ymd += date::months{static_cast<int64_t>(intPart)};
      }
      if (isInteger || durationUnits == 0.0) {
        break;  // We are done
      }
      durationUnits *= 30;  // 1 Month ~= 30 Days
      [[fallthrough]];
    // After this fall through the date may actually a bit off
    case DAY:
      // From here on we do not need leap-day handling
      ms = date::days{1};
      ms *= durationUnits;
      break;
    case WEEK:
      ms = date::weeks{1};
      ms *= durationUnits;
      break;
    case HOUR:
      ms = hours{1};
      ms *= durationUnits;
      break;
    case MINUTE:
      ms = minutes{1};
      ms *= durationUnits;
      break;
    case SECOND:
      ms = seconds{1};
      ms *= durationUnits;
      break;
    case MILLI:
      ms = milliseconds{1};
      ms *= durationUnits;
      break;
    default:
      aql::registerWarning(expressionContext, AFN,
                           TRI_ERROR_QUERY_INVALID_DATE_VALUE);
      return AqlValue(AqlValueHintNull());
  }
  // Here we reconstruct the timepoint again

  tp_sys_clock_ms resTime;
  if (isSubtract) {
    resTime = tp_sys_clock_ms{
        date::sys_days(ymd) + day_time.to_duration() -
        std::chrono::duration_cast<duration<int64_t, std::milli>>(ms)};
  } else {
    resTime = tp_sys_clock_ms{
        date::sys_days(ymd) + day_time.to_duration() +
        std::chrono::duration_cast<duration<int64_t, std::milli>>(ms)};
  }
  return ::timeAqlValue(expressionContext, AFN, resTime);
}

AqlValue addOrSubtractIsoDurationFromTimestamp(
    ExpressionContext* expressionContext, tp_sys_clock_ms const& tp,
    std::string_view duration, char const* AFN, bool isSubtract) {
  date::year_month_day ymd{floor<date::days>(tp)};
  auto day_time = date::make_time(tp - date::sys_days(ymd));

  basics::ParsedDuration parsedDuration;
  if (!basics::parseIsoDuration(duration, parsedDuration)) {
    aql::registerWarning(expressionContext, AFN,
                         TRI_ERROR_QUERY_INVALID_DATE_VALUE);
    return AqlValue(AqlValueHintNull());
  }

  if (isSubtract) {
    ymd -= date::years{parsedDuration.years};
    ymd -= date::months{parsedDuration.months};
  } else {
    ymd += date::years{parsedDuration.years};
    ymd += date::months{parsedDuration.months};
  }

  milliseconds ms{0};
  ms += date::weeks{parsedDuration.weeks};
  ms += date::days{parsedDuration.days};
  ms += hours{parsedDuration.hours};
  ms += minutes{parsedDuration.minutes};
  ms += seconds{parsedDuration.seconds};
  ms += milliseconds{parsedDuration.milliseconds};

  tp_sys_clock_ms resTime;
  if (isSubtract) {
    resTime =
        tp_sys_clock_ms{date::sys_days(ymd) + day_time.to_duration() - ms};
  } else {
    resTime =
        tp_sys_clock_ms{date::sys_days(ymd) + day_time.to_duration() + ms};
  }
  return ::timeAqlValue(expressionContext, AFN, resTime);
}

bool parameterToTimePoint(ExpressionContext* expressionContext,
                          VPackFunctionParametersView parameters,
                          tp_sys_clock_ms& tp, char const* AFN,
                          size_t parameterIndex) {
  AqlValue const& value =
      extractFunctionParameterValue(parameters, parameterIndex);

  if (value.isNumber()) {
    int64_t v = value.toInt64();
    if (ADB_UNLIKELY(v < -62167219200000 || v > 253402300799999)) {
      // check if value is between "0000-01-01T00:00:00.000Z" and
      // "9999-12-31T23:59:59.999Z" -62167219200000: "0000-01-01T00:00:00.000Z"
      // 253402300799999: "9999-12-31T23:59:59.999Z"
      aql::registerWarning(expressionContext, AFN,
                           TRI_ERROR_QUERY_INVALID_DATE_VALUE);
      return false;
    }
    tp = tp_sys_clock_ms(milliseconds(v));
    return true;
  }

  if (value.isString()) {
    if (!basics::parseDateTime(value.slice().stringView(), tp)) {
      aql::registerWarning(expressionContext, AFN,
                           TRI_ERROR_QUERY_INVALID_DATE_VALUE);
      return false;
    }
    return true;
  }

  registerInvalidArgumentWarning(expressionContext, AFN);
  return false;
}

/// @brief converts a value into a number value
double valueToNumber(VPackSlice const& slice, bool& isValid) {
  if (slice.isNull()) {
    isValid = true;
    return 0.0;
  }
  if (slice.isBoolean()) {
    isValid = true;
    return (slice.getBoolean() ? 1.0 : 0.0);
  }
  if (slice.isNumber()) {
    isValid = true;
    return slice.getNumericValue<double>();
  }
  if (slice.isString()) {
    std::string const str = slice.copyString();
    try {
      if (str.empty()) {
        isValid = true;
        return 0.0;
      }
      size_t behind = 0;
      double value = std::stod(str, &behind);
      while (behind < str.size()) {
        char c = str[behind];
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n' && c != '\f') {
          isValid = false;
          return 0.0;
        }
        ++behind;
      }
      isValid = true;
      return value;
    } catch (...) {
      size_t behind = 0;
      while (behind < str.size()) {
        char c = str[behind];
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n' && c != '\f') {
          isValid = false;
          return 0.0;
        }
        ++behind;
      }
      // A string only containing whitespae-characters is valid and should
      // return 0.0
      // It throws in std::stod
      isValid = true;
      return 0.0;
    }
  }
  if (slice.isArray()) {
    VPackValueLength const n = slice.length();
    if (n == 0) {
      isValid = true;
      return 0.0;
    }
    if (n == 1) {
      return ::valueToNumber(slice.at(0), isValid);
    }
  }

  // All other values are invalid
  isValid = false;
  return 0.0;
}

/// @brief extract a boolean parameter from an array
bool getBooleanParameter(VPackFunctionParametersView parameters,
                         size_t startParameter, bool defaultValue) {
  size_t const n = parameters.size();

  if (startParameter >= n) {
    return defaultValue;
  }

  return parameters[startParameter].toBoolean();
}

/// @brief extra a collection name from an AqlValue
std::string extractCollectionName(transaction::Methods* trx,
                                  VPackFunctionParametersView parameters,
                                  size_t position) {
  AqlValue const& value = extractFunctionParameterValue(parameters, position);

  std::string identifier;

  if (value.isString()) {
    // already a string
    identifier = value.slice().copyString();
  } else {
    AqlValueMaterializer materializer(&trx->vpackOptions());
    VPackSlice slice = materializer.slice(value, true);
    VPackSlice id = slice;

    if (slice.isObject()) {
      id = slice.get(StaticStrings::IdString);
    }
    if (id.isString()) {
      identifier = id.copyString();
    } else if (id.isCustom()) {
      identifier = trx->extractIdString(slice);
    }
  }

  if (!identifier.empty()) {
    size_t pos = identifier.find('/');

    if (pos != std::string::npos) {
      // this is superior to  identifier.substr(0, pos)
      identifier.resize(pos);
    }

    return identifier;
  }

  return StaticStrings::Empty;
}

/// @brief extract attribute names from the arguments
void extractKeys(containers::FlatHashSet<std::string>& names,
                 ExpressionContext* expressionContext,
                 VPackOptions const* vopts,
                 VPackFunctionParametersView parameters, size_t startParameter,
                 char const* functionName) {
  size_t const n = parameters.size();

  for (size_t i = startParameter; i < n; ++i) {
    AqlValue const& param = extractFunctionParameterValue(parameters, i);

    if (param.isString()) {
      names.emplace(param.slice().copyString());
    } else if (param.isNumber()) {
      double number = param.toDouble();

      if (std::isnan(number) || number == HUGE_VAL || number == -HUGE_VAL) {
        names.emplace("null");
      } else {
        char buffer[24];
        int length = fpconv_dtoa(number, &buffer[0]);
        names.emplace(&buffer[0], static_cast<size_t>(length));
      }
    } else if (param.isArray()) {
      AqlValueMaterializer materializer(vopts);
      VPackSlice s = materializer.slice(param, false);

      for (VPackSlice v : VPackArrayIterator(s)) {
        if (v.isString()) {
          names.emplace(v.copyString());
        } else {
          registerInvalidArgumentWarning(expressionContext, functionName);
        }
      }
    }
  }
}

/// @brief append the VelocyPack value to a string buffer
///        Note: Backwards compatibility. Is different than Slice.toJson()
void appendAsString(VPackOptions const& vopts, velocypack::StringSink& buffer,
                    AqlValue const& value) {
  AqlValueMaterializer materializer(&vopts);
  VPackSlice slice = materializer.slice(value, false);

  Functions::Stringify(&vopts, buffer, slice);
}

/// @brief Checks if the given list contains the element
bool listContainsElement(VPackOptions const* vopts, AqlValue const& list,
                         AqlValue const& testee, size_t& index) {
  TRI_ASSERT(list.isArray());
  AqlValueMaterializer materializer(vopts);
  VPackSlice slice = materializer.slice(list, false);

  AqlValueMaterializer testeeMaterializer(vopts);
  VPackSlice testeeSlice = testeeMaterializer.slice(testee, false);

  VPackArrayIterator it(slice);
  while (it.valid()) {
    if (basics::VelocyPackHelper::equal(testeeSlice, it.value(), false,
                                        vopts)) {
      index = static_cast<size_t>(it.index());
      return true;
    }
    it.next();
  }
  return false;
}

/// @brief Checks if the given list contains the element
/// DEPRECATED
bool listContainsElement(VPackOptions const* options, VPackSlice const& list,
                         VPackSlice const& testee, size_t& index) {
  TRI_ASSERT(list.isArray());
  for (size_t i = 0; i < static_cast<size_t>(list.length()); ++i) {
    if (basics::VelocyPackHelper::equal(testee, list.at(i), false, options)) {
      index = i;
      return true;
    }
  }
  return false;
}

bool listContainsElement(VPackOptions const* options, VPackSlice const& list,
                         VPackSlice const& testee) {
  size_t unused;
  return ::listContainsElement(options, list, testee, unused);
}

/// @brief Computes the Variance of the given list.
///        If successful value will contain the variance and count
///        will contain the number of elements.
///        If not successful value and count contain garbage.
bool variance(VPackOptions const* vopts, AqlValue const& values, double& value,
              size_t& count) {
  TRI_ASSERT(values.isArray());
  value = 0.0;
  count = 0;
  bool unused = false;
  double mean = 0.0;

  AqlValueMaterializer materializer(vopts);
  VPackSlice slice = materializer.slice(values, false);

  for (VPackSlice element : VPackArrayIterator(slice)) {
    if (!element.isNull()) {
      if (!element.isNumber()) {
        return false;
      }
      double current = ::valueToNumber(element, unused);
      count++;
      double delta = current - mean;
      mean += delta / count;
      value += delta * (current - mean);
    }
  }
  return true;
}

/// @brief Sorts the given list of Numbers in ASC order.
///        Removes all null entries.
///        Returns false if the list contains non-number values.
bool sortNumberList(VPackOptions const* vopts, AqlValue const& values,
                    std::vector<double>& result) {
  TRI_ASSERT(values.isArray());
  TRI_ASSERT(result.empty());
  bool unused;
  AqlValueMaterializer materializer(vopts);
  VPackSlice slice = materializer.slice(values, false);

  VPackArrayIterator it(slice);
  result.reserve(it.size());
  for (auto const& element : it) {
    if (!element.isNull()) {
      if (!element.isNumber()) {
        return false;
      }
      result.emplace_back(::valueToNumber(element, unused));
    }
  }
  std::sort(result.begin(), result.end());
  return true;
}

/// @brief Helper function to unset or keep all given names in the value.
///        Recursively iterates over sub-object and unsets or keeps their values
///        as well
void unsetOrKeep(transaction::Methods* trx, VPackSlice const& value,
                 containers::FlatHashSet<std::string> const& names,
                 bool unset,  // true means unset, false means keep
                 bool recursive, VPackBuilder& result) {
  TRI_ASSERT(value.isObject());
  VPackObjectBuilder b(&result);  // Close the object after this function
  for (auto const& entry : VPackObjectIterator(value, false)) {
    TRI_ASSERT(entry.key.isString());
    std::string key = entry.key.copyString();
    if ((names.find(key) == names.end()) == unset) {
      // not found and unset or found and keep
      if (recursive && entry.value.isObject()) {
        result.add(entry.key);  // Add the key
        ::unsetOrKeep(trx, entry.value, names, unset, recursive,
                      result);  // Adds the object
      } else {
        if (entry.value.isCustom()) {
          result.add(key, VPackValue(trx->extractIdString(value)));
        } else {
          result.add(key, entry.value);
        }
      }
    }
  }
}

/// @brief Helper function to get a document by it's identifier
///        Lazy Locks the collection if necessary.
void getDocumentByIdentifier(transaction::Methods* trx,
                             OperationOptions const& options,
                             std::string& collectionName,
                             std::string const& identifier, bool ignoreError,
                             VPackBuilder& result) {
  transaction::BuilderLeaser searchBuilder(trx);

  size_t pos = identifier.find('/');
  if (pos == std::string::npos) {
    searchBuilder->add(VPackValue(identifier));
  } else {
    if (collectionName.empty()) {
      char const* p = identifier.data() + pos + 1;
      size_t l = identifier.size() - pos - 1;
      searchBuilder->add(VPackValuePair(p, l, VPackValueType::String));
      collectionName = identifier.substr(0, pos);
    } else if (identifier.compare(0, pos, collectionName) != 0) {
      // Requesting an _id that cannot be stored in this collection
      if (ignoreError) {
        return;
      }
      THROW_ARANGO_EXCEPTION(TRI_ERROR_ARANGO_CROSS_COLLECTION_REQUEST);
    } else {
      char const* p = identifier.data() + pos + 1;
      size_t l = identifier.size() - pos - 1;
      searchBuilder->add(VPackValuePair(p, l, VPackValueType::String));
    }
  }

  Result res;
  try {
    res = trx->documentFastPath(collectionName, searchBuilder->slice(), options,
                                result);
  } catch (basics::Exception const& ex) {
    res.reset(ex.code());
  }

  if (!res.ok()) {
    if (ignoreError) {
      if (res.errorNumber() == TRI_ERROR_ARANGO_DOCUMENT_NOT_FOUND ||
          res.errorNumber() == TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND ||
          res.errorNumber() == TRI_ERROR_ARANGO_CROSS_COLLECTION_REQUEST) {
        return;
      }
    }
    if (res.is(TRI_ERROR_TRANSACTION_UNREGISTERED_COLLECTION)) {
      // special error message to indicate which collection was undeclared
      THROW_ARANGO_EXCEPTION_MESSAGE(
          res.errorNumber(),
          StringUtils::concatT(res.errorMessage(), ": ", collectionName, " [",
                               AccessMode::typeString(AccessMode::Type::READ),
                               "]"));
    }
    THROW_ARANGO_EXCEPTION(res);
  }
}

/// @brief Helper function to merge given parameters
///        Works for an array of objects as first parameter or arbitrary many
///        object parameters
AqlValue mergeParameters(ExpressionContext* expressionContext,
                         VPackFunctionParametersView parameters,
                         char const* funcName, bool recursive) {
  size_t const n = parameters.size();

  if (n == 0) {
    return AqlValue(AqlValueHintEmptyObject());
  }

  auto& vopts = expressionContext->trx().vpackOptions();

  // use the first argument as the preliminary result
  AqlValue const& initial = extractFunctionParameterValue(parameters, 0);
  AqlValueMaterializer materializer(&vopts);
  VPackSlice initialSlice = materializer.slice(initial, true);

  VPackBuilder builder;

  if (initial.isArray() && n == 1) {
    // special case: a single array parameter
    // Create an empty document as start point
    builder.openObject();
    builder.close();
    // merge in all other arguments
    for (VPackSlice it : VPackArrayIterator(initialSlice)) {
      if (!it.isObject()) {
        registerInvalidArgumentWarning(expressionContext, funcName);
        return AqlValue(AqlValueHintNull());
      }
      builder = velocypack::Collection::merge(builder.slice(), it,
                                              /*mergeObjects*/ recursive,
                                              /*nullMeansRemove*/ false);
    }
    return AqlValue(builder.slice(), builder.size());
  }

  if (!initial.isObject()) {
    registerInvalidArgumentWarning(expressionContext, funcName);
    return AqlValue(AqlValueHintNull());
  }

  // merge in all other arguments
  for (size_t i = 1; i < n; ++i) {
    AqlValue const& param = extractFunctionParameterValue(parameters, i);

    if (!param.isObject()) {
      registerInvalidArgumentWarning(expressionContext, funcName);
      return AqlValue(AqlValueHintNull());
    }

    AqlValueMaterializer materializer(&vopts);
    VPackSlice slice = materializer.slice(param, false);

    builder = velocypack::Collection::merge(initialSlice, slice,
                                            /*mergeObjects*/ recursive,
                                            /*nullMeansRemove*/ false);
    initialSlice = builder.slice();
  }
  if (n == 1) {
    // only one parameter. now add original document
    builder.add(initialSlice);
  }
  return AqlValue(builder.slice(), builder.size());
}

/// @brief internal recursive flatten helper
void flattenList(VPackSlice const& array, size_t maxDepth, size_t curDepth,
                 VPackBuilder& result) {
  TRI_ASSERT(result.isOpenArray());
  for (auto const& tmp : VPackArrayIterator(array)) {
    if (tmp.isArray() && curDepth < maxDepth) {
      ::flattenList(tmp, maxDepth, curDepth + 1, result);
    } else {
      // Copy the content of tmp into the result
      result.add(tmp);
    }
  }
}

/**
 * @brief Parses 1 or 3-7 input parameters and creates a Date object out of it.
 *        This object can either be a timestamp in milliseconds or an ISO_8601
 * DATE
 *
 * @param expressionContext The AQL expression context
 * @param trx The used transaction
 * @param parameters list of parameters, only 1 or 3-7 are allowed
 * @param asTimestamp If it should return a timestamp (true) or ISO_DATE (false)
 *
 * @return Returns a timestamp if asTimestamp is true, an ISO_DATE otherwise
 */
AqlValue dateFromParameters(ExpressionContext* expressionContext,
                            VPackFunctionParametersView parameters,
                            char const* AFN, bool asTimestamp) {
  tp_sys_clock_ms tp;
  duration<int64_t, std::milli> time;

  if (parameters.size() == 1) {
    if (!::parameterToTimePoint(expressionContext, parameters, tp, AFN, 0)) {
      return AqlValue(AqlValueHintNull());
    }
    time = tp.time_since_epoch();
  } else {
    if (parameters.size() < 3 || parameters.size() > 7) {
      // YMD is a must
      registerInvalidArgumentWarning(expressionContext, AFN);
      return AqlValue(AqlValueHintNull());
    }

    for (uint8_t i = 0; i < parameters.size(); i++) {
      AqlValue const& value = extractFunctionParameterValue(parameters, i);

      // All Parameters have to be a number or a string
      if (!value.isNumber() && !value.isString()) {
        registerInvalidArgumentWarning(expressionContext, AFN);
        return AqlValue(AqlValueHintNull());
      }
    }

    date::years y{extractFunctionParameterValue(parameters, 0).toInt64()};
    date::months m{extractFunctionParameterValue(parameters, 1).toInt64()};
    date::days d{extractFunctionParameterValue(parameters, 2).toInt64()};

    if ((y < date::years{0} || y > date::years{9999}) ||
        (m < date::months{0}) || (d < date::days{0})) {
      registerWarning(expressionContext, AFN,
                      TRI_ERROR_QUERY_INVALID_DATE_VALUE);
      return AqlValue(AqlValueHintNull());
    }
    date::year_month_day ymd = date::year{y.count()} / m.count() / d.count();

    // Parse the time
    hours h(0);
    minutes min(0);
    seconds s(0);
    milliseconds ms(0);

    if (parameters.size() >= 4) {
      h = hours((extractFunctionParameterValue(parameters, 3).toInt64()));
    }
    if (parameters.size() >= 5) {
      min = minutes((extractFunctionParameterValue(parameters, 4).toInt64()));
    }
    if (parameters.size() >= 6) {
      s = seconds((extractFunctionParameterValue(parameters, 5).toInt64()));
    }
    if (parameters.size() == 7) {
      int64_t v = extractFunctionParameterValue(parameters, 6).toInt64();
      if (v > 999) {
        v = 999;
      }
      ms = milliseconds(v);
    }

    if ((h < hours{0}) || (min < minutes{0}) || (s < seconds{0}) ||
        (ms < milliseconds{0})) {
      registerWarning(expressionContext, AFN,
                      TRI_ERROR_QUERY_INVALID_DATE_VALUE);
      return AqlValue(AqlValueHintNull());
    }

    time = date::sys_days(ymd).time_since_epoch();
    time += h;
    time += min;
    time += s;
    time += ms;
    tp = tp_sys_clock_ms(time);
  }

  if (asTimestamp) {
    return AqlValue(AqlValueHintInt(time.count()));
  }
  return ::timeAqlValue(expressionContext, AFN, tp);
}

AqlValue callApplyBackend(ExpressionContext* expressionContext,
                          AstNode const& node, char const* AFN,
                          AqlValue const& invokeFN,
                          VPackFunctionParametersView invokeParams) {
  auto& trx = expressionContext->trx();

  std::string ucInvokeFN;
  transaction::StringLeaser buffer(&trx);
  velocypack::StringSink adapter(buffer.get());

  ::appendAsString(trx.vpackOptions(), adapter, invokeFN);

  icu::UnicodeString unicodeStr(buffer->data(),
                                static_cast<int32_t>(buffer->length()));
  unicodeStr.toUpper(nullptr);
  unicodeStr.toUTF8String(ucInvokeFN);

  aql::Function const* func = nullptr;
  if (ucInvokeFN.find("::") == std::string::npos) {
    // built-in C++ function
    auto& server = trx.vocbase().server();
    func = server.getFeature<AqlFunctionFeature>().byName(ucInvokeFN);
    if (func->hasCxxImplementation()) {
      std::pair<size_t, size_t> numExpectedArguments = func->numArguments();

      if (invokeParams.size() < numExpectedArguments.first ||
          invokeParams.size() > numExpectedArguments.second) {
        THROW_ARANGO_EXCEPTION_PARAMS(
            TRI_ERROR_QUERY_FUNCTION_ARGUMENT_NUMBER_MISMATCH,
            ucInvokeFN.c_str(), static_cast<int>(numExpectedArguments.first),
            static_cast<int>(numExpectedArguments.second));
      }

      return func->implementation(expressionContext, node, invokeParams);
    }
  }

  // JavaScript function (this includes user-defined functions)
  {
    ISOLATE;
    TRI_V8_CURRENT_GLOBALS_AND_SCOPE;
    auto context = TRI_IGETC;

    auto old = v8g->_expressionContext;
    v8g->_expressionContext = expressionContext;
    auto sg = scopeGuard([&]() noexcept { v8g->_expressionContext = old; });

    VPackOptions const& options = trx.vpackOptions();
    std::string jsName;
    int const n = static_cast<int>(invokeParams.size());
    int const callArgs = (func == nullptr ? 3 : n);
    auto args = std::make_unique<v8::Handle<v8::Value>[]>(callArgs);

    if (func == nullptr) {
      // a call to a user-defined function
      jsName = "FCALL_USER";

      // function name
      args[0] = TRI_V8_STD_STRING(isolate, ucInvokeFN);
      // call parameters
      v8::Handle<v8::Array> params =
          v8::Array::New(isolate, static_cast<int>(n));

      for (int i = 0; i < n; ++i) {
        params
            ->Set(context, static_cast<uint32_t>(i),
                  invokeParams[i].toV8(isolate, &options))
            .FromMaybe(true);
      }
      args[1] = params;
      args[2] = TRI_V8_ASCII_STRING(isolate, AFN);
    } else {
      // a call to a built-in V8 function
      TRI_ASSERT(func->hasV8Implementation());

      jsName = "AQL_" + func->name;
      for (int i = 0; i < n; ++i) {
        args[i] = invokeParams[i].toV8(isolate, &options);
      }
    }

    bool dummy;
    return Expression::invokeV8Function(*expressionContext, jsName, ucInvokeFN,
                                        AFN, false, callArgs, args.get(),
                                        dummy);
  }
}

AqlValue geoContainsIntersect(ExpressionContext* expressionContext,
                              AstNode const&,
                              VPackFunctionParametersView parameters,
                              char const* func, bool contains) {
  auto* vopts = &expressionContext->trx().vpackOptions();
  AqlValue const& p1 = extractFunctionParameterValue(parameters, 0);
  AqlValue const& p2 = extractFunctionParameterValue(parameters, 1);

  if (!p1.isObject()) {
    registerWarning(expressionContext, func,
                    Result(TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH,
                           "Expecting GeoJSON object"));
    return AqlValue(AqlValueHintNull());
  }

  AqlValueMaterializer mat1(vopts);
  geo::ShapeContainer outer, inner;
  Result res = geo::geojson::parseRegion(mat1.slice(p1, true), outer, false);
  if (res.fail()) {
    registerWarning(expressionContext, func, res);
    return AqlValue(AqlValueHintNull());
  }
  if (contains && !outer.isAreaType()) {
    registerWarning(
        expressionContext, func,
        Result(
            TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH,
            "Only Polygon and MultiPolygon types are valid as first argument"));
    return AqlValue(AqlValueHintNull());
  }

  AqlValueMaterializer mat2(vopts);
  if (p2.isArray() && p2.length() >= 2) {
    res = inner.parseCoordinates(mat2.slice(p2, true), /*geoJson*/ true);
  } else if (p2.isObject()) {
    res = geo::geojson::parseRegion(mat2.slice(p2, true), inner, false);
  } else {
    res.reset(TRI_ERROR_BAD_PARAMETER,
              "Second arg requires coordinate pair or GeoJSON");
  }
  if (res.fail()) {
    registerWarning(expressionContext, func, res);
    return AqlValue(AqlValueHintNull());
  }

  bool result;
  try {
    result = contains ? outer.contains(&inner) : outer.intersects(&inner);
    return AqlValue(AqlValueHintBool(result));
  } catch (basics::Exception const& ex) {
    res.reset(ex.code(), ex.what());
    registerWarning(expressionContext, func, res);
    return AqlValue(AqlValueHintBool(false));
  }
}

static Result parseGeoPolygon(VPackSlice polygon, VPackBuilder& b) {
  // check if nested or not
  bool unnested = false;
  for (VPackSlice v : VPackArrayIterator(polygon)) {
    if (v.isArray() && v.length() == 2) {
      unnested = true;
    }
  }

  if (unnested) {
    b.openArray();
  }

  if (!polygon.isArray()) {
    return Result(TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH,
                  "Polygon needs to be an array of positions.");
  }

  for (VPackSlice v : VPackArrayIterator(polygon)) {
    if (v.isArray() && v.length() > 2) {
      b.openArray();
      for (VPackSlice const coord : VPackArrayIterator(v)) {
        if (coord.isNumber()) {
          b.add(VPackValue(coord.getNumber<double>()));
        } else if (coord.isArray()) {
          if (coord.length() < 2) {
            return Result(TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH,
                          "a Position needs at least two numeric values");
          } else {
            b.openArray();
            for (VPackSlice const innercord : VPackArrayIterator(coord)) {
              if (innercord.isNumber()) {
                b.add(VPackValue(innercord.getNumber<double>()));  // TODO
              } else if (innercord.isArray() && innercord.length() == 2) {
                if (innercord.at(0).isNumber() && innercord.at(1).isNumber()) {
                  b.openArray();
                  b.add(VPackValue(innercord.at(0).getNumber<double>()));
                  b.add(VPackValue(innercord.at(1).getNumber<double>()));
                  b.close();
                } else {
                  return Result(TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH,
                                "coordinate is not a number");
                }
              } else {
                return Result(TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH,
                              "not an array describing a position");
              }
            }
            b.close();
          }
        } else {
          return Result(TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH,
                        "not an array containing positions");
        }
      }
      b.close();
    } else if (v.isArray() && v.length() == 2) {
      if (polygon.length() > 2) {
        b.openArray();
        for (VPackSlice const innercord : VPackArrayIterator(v)) {
          if (innercord.isNumber()) {
            b.add(VPackValue(innercord.getNumber<double>()));
          } else if (innercord.isArray() && innercord.length() == 2) {
            if (innercord.at(0).isNumber() && innercord.at(1).isNumber()) {
              b.openArray();
              b.add(VPackValue(innercord.at(0).getNumber<double>()));
              b.add(VPackValue(innercord.at(1).getNumber<double>()));
              b.close();
            } else {
              return Result(TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH,
                            "coordinate is not a number");
            }
          } else {
            return Result(TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH,
                          "not a numeric value");
          }
        }
        b.close();
      } else {
        return Result(TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH,
                      "a Polygon needs at least three positions");
      }
    } else {
      return Result(TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH,
                    "not an array containing positions");
    }
  }

  if (unnested) {
    b.close();
  }

  return {TRI_ERROR_NO_ERROR};
}

Result parseShape(ExpressionContext* exprCtx, AqlValue const& value,
                  geo::ShapeContainer& shape) {
  auto* vopts = &exprCtx->trx().vpackOptions();
  AqlValueMaterializer mat(vopts);

  if (value.isArray() && value.length() >= 2) {
    return shape.parseCoordinates(mat.slice(value, true), /*geoJson*/ true);
  } else if (value.isObject()) {
    return geo::geojson::parseRegion(mat.slice(value, true), shape, false);
  } else {
    return {TRI_ERROR_BAD_PARAMETER, "Requires coordinate pair or GeoJSON"};
  }
}

irs::string_ref getFunctionName(const AstNode& node) {
  TRI_ASSERT(aql::NODE_TYPE_FCALL == node.type);
  auto const* impl = static_cast<aql::Function*>(node.getData());
  TRI_ASSERT(impl != nullptr);
  return impl->name;
}

}  // namespace

namespace arangodb {
namespace aql {

/// @brief register warning
void registerWarning(ExpressionContext* expressionContext,
                     char const* functionName, Result const& rr) {
  std::string msg = "in function '";
  msg.append(functionName);
  msg.append("()': ");
  msg.append(rr.errorMessage());
  expressionContext->registerWarning(rr.errorNumber(), msg.c_str());
}

/// @brief register warning
void registerWarning(ExpressionContext* expressionContext,
                     char const* functionName, ErrorCode code) {
  if (code != TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH &&
      code != TRI_ERROR_QUERY_FUNCTION_ARGUMENT_NUMBER_MISMATCH) {
    registerWarning(expressionContext, functionName, Result(code));
    return;
  }

  std::string msg = basics::Exception::FillExceptionString(code, functionName);
  expressionContext->registerWarning(code, msg.c_str());
}

void registerError(ExpressionContext* expressionContext,
                   char const* functionName, ErrorCode code) {
  std::string msg;

  if (code == TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH) {
    msg = aql::QueryWarnings::buildFormattedString(code, functionName);
  } else {
    msg.append("in function '");
    msg.append(functionName);
    msg.append("()': ");
    msg.append(TRI_errno_string(code));
  }

  expressionContext->registerError(code, msg.c_str());
}

/// @brief register usage of an invalid function argument
void registerInvalidArgumentWarning(ExpressionContext* expressionContext,
                                    char const* functionName) {
  registerWarning(expressionContext, functionName,
                  TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH);
}

}  // namespace aql
}  // namespace arangodb

/// @brief append the VelocyPack value to a string buffer
void Functions::Stringify(VPackOptions const* vopts,
                          velocypack::StringSink& buffer,
                          VPackSlice const& slice) {
  if (slice.isNull()) {
    // null is the empty string
    return;
  }

  if (slice.isString()) {
    // dumping adds additional ''. we want to avoid that.
    VPackValueLength length;
    char const* p = slice.getStringUnchecked(length);
    buffer.append(p, length);
    return;
  }

  VPackOptions adjustedOptions = *vopts;
  adjustedOptions.escapeUnicode = false;
  adjustedOptions.escapeForwardSlashes = false;
  VPackDumper dumper(&buffer, &adjustedOptions);
  dumper.dump(slice);
}

/// @brief function IS_NULL
AqlValue Functions::IsNull(ExpressionContext*, AstNode const&,
                           VPackFunctionParametersView parameters) {
  AqlValue const& a = extractFunctionParameterValue(parameters, 0);
  return AqlValue(AqlValueHintBool(a.isNull(true)));
}

/// @brief function IS_BOOL
AqlValue Functions::IsBool(ExpressionContext*, AstNode const&,
                           VPackFunctionParametersView parameters) {
  AqlValue const& a = extractFunctionParameterValue(parameters, 0);
  return AqlValue(AqlValueHintBool(a.isBoolean()));
}

/// @brief function IS_NUMBER
AqlValue Functions::IsNumber(ExpressionContext*, AstNode const&,
                             VPackFunctionParametersView parameters) {
  AqlValue const& a = extractFunctionParameterValue(parameters, 0);
  return AqlValue(AqlValueHintBool(a.isNumber()));
}

/// @brief function IS_STRING
AqlValue Functions::IsString(ExpressionContext*, AstNode const&,
                             VPackFunctionParametersView parameters) {
  AqlValue const& a = extractFunctionParameterValue(parameters, 0);
  return AqlValue(AqlValueHintBool(a.isString()));
}

/// @brief function IS_ARRAY
AqlValue Functions::IsArray(ExpressionContext*, AstNode const&,
                            VPackFunctionParametersView parameters) {
  AqlValue const& a = extractFunctionParameterValue(parameters, 0);
  return AqlValue(AqlValueHintBool(a.isArray()));
}

/// @brief function IS_OBJECT
AqlValue Functions::IsObject(ExpressionContext*, AstNode const&,
                             VPackFunctionParametersView parameters) {
  AqlValue const& a = extractFunctionParameterValue(parameters, 0);
  return AqlValue(AqlValueHintBool(a.isObject()));
}

/// @brief function TYPENAME
AqlValue Functions::Typename(ExpressionContext*, AstNode const&,
                             VPackFunctionParametersView parameters) {
  AqlValue const& value = extractFunctionParameterValue(parameters, 0);
  char const* type = value.getTypeString();

  return AqlValue(type, std::strlen(type));
}

/// @brief function TO_NUMBER
AqlValue Functions::ToNumber(ExpressionContext*, AstNode const&,
                             VPackFunctionParametersView parameters) {
  AqlValue const& a = extractFunctionParameterValue(parameters, 0);
  bool failed;
  double value = a.toDouble(failed);

  if (failed) {
    return AqlValue(AqlValueHintZero());
  }

  return AqlValue(AqlValueHintDouble(value));
}

/// @brief function TO_STRING
AqlValue Functions::ToString(ExpressionContext* expr, AstNode const&,
                             VPackFunctionParametersView parameters) {
  auto& trx = expr->trx();
  AqlValue const& value = extractFunctionParameterValue(parameters, 0);

  transaction::StringLeaser buffer(&trx);
  velocypack::StringSink adapter(buffer.get());

  ::appendAsString(trx.vpackOptions(), adapter, value);
  return AqlValue(buffer->data(), buffer->length());
}

/// @brief function TO_BASE64
AqlValue Functions::ToBase64(ExpressionContext* expr, AstNode const&,
                             VPackFunctionParametersView parameters) {
  auto& trx = expr->trx();
  AqlValue const& value = extractFunctionParameterValue(parameters, 0);

  transaction::StringLeaser buffer(&trx);
  velocypack::StringSink adapter(buffer.get());

  ::appendAsString(trx.vpackOptions(), adapter, value);

  std::string encoded =
      basics::StringUtils::encodeBase64(buffer->data(), buffer->length());

  return AqlValue(encoded);
}

/// @brief function TO_HEX
AqlValue Functions::ToHex(ExpressionContext* expr, AstNode const&,
                          VPackFunctionParametersView parameters) {
  auto& trx = expr->trx();
  AqlValue const& value = extractFunctionParameterValue(parameters, 0);

  transaction::StringLeaser buffer(&trx);
  velocypack::StringSink adapter(buffer.get());

  ::appendAsString(trx.vpackOptions(), adapter, value);

  std::string encoded =
      basics::StringUtils::encodeHex(buffer->data(), buffer->length());

  return AqlValue(encoded);
}

/// @brief function ENCODE_URI_COMPONENT
AqlValue Functions::EncodeURIComponent(ExpressionContext* expr, AstNode const&,
                                       VPackFunctionParametersView parameters) {
  auto& trx = expr->trx();
  AqlValue const& value = extractFunctionParameterValue(parameters, 0);

  transaction::StringLeaser buffer(&trx);
  velocypack::StringSink adapter(buffer.get());

  ::appendAsString(trx.vpackOptions(), adapter, value);

  std::string encoded =
      basics::StringUtils::encodeURIComponent(buffer->data(), buffer->length());

  return AqlValue(encoded);
}

/// @brief function UUID
AqlValue Functions::Uuid(ExpressionContext*, AstNode const&,
                         VPackFunctionParametersView parameters) {
  boost::uuids::uuid uuid;
  {
    // must protect mutex generation from races
    MUTEX_LOCKER(mutexLocker, ::uuidMutex);
    uuid = boost::uuids::random_generator()();
  }

  return AqlValue(boost::uuids::to_string(uuid));
}

/// @brief function SOUNDEX
AqlValue Functions::Soundex(ExpressionContext* expr, AstNode const&,
                            VPackFunctionParametersView parameters) {
  auto& trx = expr->trx();
  AqlValue const& value = extractFunctionParameterValue(parameters, 0);

  transaction::StringLeaser buffer(&trx);
  velocypack::StringSink adapter(buffer.get());

  ::appendAsString(trx.vpackOptions(), adapter, value);

  std::string encoded = basics::StringUtils::soundex(
      basics::StringUtils::trim(basics::StringUtils::tolower(
          std::string(buffer->data(), buffer->length()))));

  return AqlValue(encoded);
}

/// @brief function LEVENSHTEIN_DISTANCE
AqlValue Functions::LevenshteinDistance(
    ExpressionContext* expr, AstNode const&,
    VPackFunctionParametersView parameters) {
  auto& trx = expr->trx();
  AqlValue const& value1 = extractFunctionParameterValue(parameters, 0);
  AqlValue const& value2 = extractFunctionParameterValue(parameters, 1);

  // we use one buffer to stringify both arguments
  transaction::StringLeaser buffer(&trx);
  velocypack::StringSink adapter(buffer.get());

  // stringify argument 1
  ::appendAsString(trx.vpackOptions(), adapter, value1);

  // note split position
  size_t const split = buffer->length();

  // stringify argument 2
  ::appendAsString(trx.vpackOptions(), adapter, value2);

  int encoded = basics::StringUtils::levenshteinDistance(
      buffer->data(), split, buffer->data() + split, buffer->length() - split);

  return AqlValue(AqlValueHintInt(encoded));
}

namespace {
template<bool search_semantics>
AqlValue NgramSimilarityHelper(char const* AFN, ExpressionContext* ctx,
                               VPackFunctionParametersView args) {
  TRI_ASSERT(ctx);
  if (args.size() < 3) {
    registerWarning(ctx, AFN,
                    Result{TRI_ERROR_QUERY_FUNCTION_ARGUMENT_NUMBER_MISMATCH,
                           "Minimum 3 arguments are expected."});
    return AqlValue(AqlValueHintNull());
  }

  auto const& attribute = extractFunctionParameterValue(args, 0);
  if (ADB_UNLIKELY(!attribute.isString())) {
    aql::registerInvalidArgumentWarning(ctx, AFN);
    return aql::AqlValue{aql::AqlValueHintNull{}};
  }
  auto const attributeValue =
      arangodb::iresearch::getStringRef(attribute.slice());

  auto const& target = extractFunctionParameterValue(args, 1);
  if (ADB_UNLIKELY(!target.isString())) {
    aql::registerInvalidArgumentWarning(ctx, AFN);
    return aql::AqlValue{aql::AqlValueHintNull{}};
  }
  auto const targetValue = arangodb::iresearch::getStringRef(target.slice());

  auto const& ngramSize = extractFunctionParameterValue(args, 2);
  if (ADB_UNLIKELY(!ngramSize.isNumber())) {
    aql::registerInvalidArgumentWarning(ctx, AFN);
    return aql::AqlValue{aql::AqlValueHintNull{}};
  }
  auto const ngramSizeValue = ngramSize.toInt64();

  if (ADB_UNLIKELY(ngramSizeValue < 1)) {
    aql::registerWarning(ctx, AFN,
                         Result{TRI_ERROR_BAD_PARAMETER,
                                "Invalid ngram size. Should be 1 or greater"});
    return aql::AqlValue{aql::AqlValueHintNull{}};
  }

  auto utf32Attribute = basics::StringUtils::characterCodes(
      attributeValue.c_str(), attributeValue.size());
  auto utf32Target = basics::StringUtils::characterCodes(targetValue.c_str(),
                                                         targetValue.size());

  auto const similarity = irs::ngram_similarity<uint32_t, search_semantics>(
      utf32Target.data(), utf32Target.size(), utf32Attribute.data(),
      utf32Attribute.size(), ngramSizeValue);
  return AqlValue(AqlValueHintDouble(similarity));
}
}  // namespace

/// Executes NGRAM_SIMILARITY based on binary ngram similarity
AqlValue Functions::NgramSimilarity(ExpressionContext* ctx, AstNode const&,
                                    VPackFunctionParametersView args) {
  static char const* AFN = "NGRAM_SIMILARITY";
  return NgramSimilarityHelper<true>(AFN, ctx, args);
}

/// Executes NGRAM_POSITIONAL_SIMILARITY based on positional ngram similarity
AqlValue Functions::NgramPositionalSimilarity(
    ExpressionContext* ctx, AstNode const&, VPackFunctionParametersView args) {
  static char const* AFN = "NGRAM_POSITIONAL_SIMILARITY";
  return NgramSimilarityHelper<false>(AFN, ctx, args);
}

/// Executes NGRAM_MATCH based on binary ngram similarity
AqlValue Functions::NgramMatch(ExpressionContext* ctx, AstNode const&,
                               VPackFunctionParametersView args) {
  TRI_ASSERT(ctx);
  static char const* AFN = "NGRAM_MATCH";

  auto const argc = args.size();

  if (argc <
      3) {  // for const evaluation we need analyzer to be set explicitly (we
            // can`t access filter context) but we can`t set analyzer as
            // mandatory in function AQL signature - this will break SEARCH
    registerWarning(ctx, AFN,
                    Result{TRI_ERROR_QUERY_FUNCTION_ARGUMENT_NUMBER_MISMATCH,
                           "Minimum 3 arguments are expected."});
    return AqlValue(AqlValueHintNull());
  }

  auto const& attribute = extractFunctionParameterValue(args, 0);
  if (ADB_UNLIKELY(!attribute.isString())) {
    aql::registerInvalidArgumentWarning(ctx, AFN);
    return aql::AqlValue{aql::AqlValueHintNull{}};
  }
  auto const attributeValue = iresearch::getStringRef(attribute.slice());

  auto const& target = extractFunctionParameterValue(args, 1);
  if (ADB_UNLIKELY(!target.isString())) {
    aql::registerInvalidArgumentWarning(ctx, AFN);
    return aql::AqlValue{aql::AqlValueHintNull{}};
  }
  auto const targetValue = iresearch::getStringRef(target.slice());

  auto threshold = iresearch::FilterConstants::DefaultNgramMatchThreshold;
  size_t analyzerPosition = 2;
  if (argc > 3) {  // 4 args given. 3rd is threshold
    auto const& thresholdArg = extractFunctionParameterValue(args, 2);
    analyzerPosition = 3;
    if (ADB_UNLIKELY(!thresholdArg.isNumber())) {
      aql::registerInvalidArgumentWarning(ctx, AFN);
      return aql::AqlValue{aql::AqlValueHintNull{}};
    }
    threshold = thresholdArg.toDouble();
    if (threshold <= 0 || threshold > 1) {
      aql::registerWarning(
          ctx, AFN,
          Result{TRI_ERROR_BAD_PARAMETER, "Threshold must be between 0 and 1"});
    }
  }

  auto const& analyzerArg =
      extractFunctionParameterValue(args, analyzerPosition);
  if (ADB_UNLIKELY(!analyzerArg.isString())) {
    aql::registerInvalidArgumentWarning(ctx, AFN);
    return aql::AqlValue{aql::AqlValueHintNull{}};
  }
  TRI_ASSERT(ctx != nullptr);
  auto const analyzerId = iresearch::getStringRef(analyzerArg.slice());
  auto& server = ctx->vocbase().server();
  if (!server.hasFeature<iresearch::IResearchAnalyzerFeature>()) {
    aql::registerWarning(ctx, AFN, TRI_ERROR_INTERNAL);
    return aql::AqlValue{aql::AqlValueHintNull{}};
  }
  auto& analyzerFeature =
      server.getFeature<iresearch::IResearchAnalyzerFeature>();
  auto& trx = ctx->trx();
  auto analyzer = analyzerFeature.get(analyzerId, ctx->vocbase(),
                                      trx.state()->analyzersRevision());
  if (!analyzer) {
    aql::registerWarning(
        ctx, AFN,
        Result{TRI_ERROR_BAD_PARAMETER, "Unable to load requested analyzer"});
    return aql::AqlValue{aql::AqlValueHintNull{}};
  }

  auto analyzerImpl = analyzer->get();
  TRI_ASSERT(analyzerImpl);
  irs::term_attribute const* token =
      irs::get<irs::term_attribute>(*analyzerImpl);
  TRI_ASSERT(token);

  std::vector<irs::bstring> attrNgrams;
  analyzerImpl->reset(attributeValue);
  while (analyzerImpl->next()) {
    attrNgrams.emplace_back(token->value.c_str(), token->value.size());
  }

  std::vector<irs::bstring> targetNgrams;
  analyzerImpl->reset(targetValue);
  while (analyzerImpl->next()) {
    targetNgrams.emplace_back(token->value.c_str(), token->value.size());
  }

  // consider only non empty ngrams sets. As no ngrams emitted - means no data
  // in index = no match
  if (!targetNgrams.empty() && !attrNgrams.empty()) {
    size_t thresholdMatches =
        (size_t)std::ceil((float_t)targetNgrams.size() * threshold);
    size_t d = 0;  // will store upper-left cell value for current cache row
    std::vector<size_t> cache(attrNgrams.size() + 1, 0);
    for (auto const& targetNgram : targetNgrams) {
      size_t s_ngram_idx = 1;
      for (; s_ngram_idx <= attrNgrams.size(); ++s_ngram_idx) {
        size_t curMatches =
            d + (size_t)(attrNgrams[s_ngram_idx - 1] == targetNgram);
        if (curMatches >= thresholdMatches) {
          return aql::AqlValue{aql::AqlValueHintBool{true}};
        }
        auto tmp = cache[s_ngram_idx];
        cache[s_ngram_idx] = std::max(
            std::max(cache[s_ngram_idx - 1], cache[s_ngram_idx]), curMatches);
        d = tmp;
      }
    }
  }
  return aql::AqlValue{aql::AqlValueHintBool{false}};
}

/// Executes LEVENSHTEIN_MATCH
AqlValue Functions::LevenshteinMatch(ExpressionContext* ctx,
                                     AstNode const& node,
                                     VPackFunctionParametersView args) {
  static char const* AFN = "LEVENSHTEIN_MATCH";

  auto const& maxDistance = extractFunctionParameterValue(args, 2);

  if (ADB_UNLIKELY(!maxDistance.isNumber())) {
    aql::registerInvalidArgumentWarning(ctx, AFN);
    return aql::AqlValue{aql::AqlValueHintNull{}};
  }

  bool withTranspositionsValue = true;
  int64_t maxDistanceValue = maxDistance.toInt64();

  if (args.size() > 3) {
    auto const& withTranspositions = extractFunctionParameterValue(args, 3);

    if (ADB_UNLIKELY(!withTranspositions.isBoolean())) {
      registerInvalidArgumentWarning(ctx, AFN);
      return AqlValue{AqlValueHintNull{}};
    }

    withTranspositionsValue = withTranspositions.toBoolean();
  }

  if (maxDistanceValue < 0 ||
      (!withTranspositionsValue &&
       maxDistanceValue > iresearch::kMaxLevenshteinDistance)) {
    registerInvalidArgumentWarning(ctx, AFN);
    return AqlValue{AqlValueHintNull{}};
  }

  if (withTranspositionsValue &&
      maxDistanceValue > iresearch::kMaxDamerauLevenshteinDistance) {
    // fallback to LEVENSHTEIN_DISTANCE
    auto const dist = Functions::LevenshteinDistance(ctx, node, args);
    TRI_ASSERT(dist.isNumber());

    return AqlValue{AqlValueHintBool{dist.toInt64() <= maxDistanceValue}};
  }

  size_t const unsignedMaxDistanceValue = static_cast<size_t>(maxDistanceValue);

  auto& description = iresearch::getParametricDescription(
      static_cast<irs::byte_type>(unsignedMaxDistanceValue),
      withTranspositionsValue);

  if (ADB_UNLIKELY(!description)) {
    registerInvalidArgumentWarning(ctx, AFN);
    return AqlValue{AqlValueHintNull{}};
  }

  auto const& lhs = extractFunctionParameterValue(args, 0);
  auto const lhsValue = lhs.isString() ? iresearch::getBytesRef(lhs.slice())
                                       : irs::bytes_ref::EMPTY;
  auto const& rhs = extractFunctionParameterValue(args, 1);
  auto const rhsValue = rhs.isString() ? iresearch::getBytesRef(rhs.slice())
                                       : irs::bytes_ref::EMPTY;

  size_t const dist = irs::edit_distance(description, lhsValue, rhsValue);

  return AqlValue(AqlValueHintBool(dist <= unsignedMaxDistanceValue));
}

/// @brief function IN_RANGE
AqlValue Functions::InRange(ExpressionContext* ctx, AstNode const&,
                            VPackFunctionParametersView args) {
  static char const* AFN = "IN_RANGE";

  auto const argc = args.size();

  if (argc != 5) {
    registerWarning(ctx, AFN,
                    Result{TRI_ERROR_QUERY_FUNCTION_ARGUMENT_NUMBER_MISMATCH,
                           "5 arguments are expected."});
    return AqlValue(AqlValueHintNull());
  }

  auto* vopts = &ctx->trx().vpackOptions();
  auto const& attributeVal = extractFunctionParameterValue(args, 0);
  auto const& lowerVal = extractFunctionParameterValue(args, 1);
  auto const& upperVal = extractFunctionParameterValue(args, 2);
  auto const& includeLowerVal = extractFunctionParameterValue(args, 3);
  auto const& includeUpperVal = extractFunctionParameterValue(args, 4);

  if (ADB_UNLIKELY(!includeLowerVal.isBoolean())) {
    aql::registerInvalidArgumentWarning(ctx, AFN);
    return aql::AqlValue{aql::AqlValueHintNull{}};
  }

  if (ADB_UNLIKELY(!includeUpperVal.isBoolean())) {
    aql::registerInvalidArgumentWarning(ctx, AFN);
    return aql::AqlValue{aql::AqlValueHintNull{}};
  }

  auto const includeLower = includeLowerVal.toBoolean();
  auto const includeUpper = includeUpperVal.toBoolean();

  // first check lower bound
  {
    auto const compareLowerResult =
        AqlValue::Compare(vopts, lowerVal, attributeVal, true);
    if ((!includeLower && compareLowerResult >= 0) ||
        (includeLower && compareLowerResult > 0)) {
      return AqlValue(AqlValueHintBool(false));
    }
  }

  // lower bound is fine, check upper
  auto const compareUpperResult =
      AqlValue::Compare(vopts, attributeVal, upperVal, true);
  return AqlValue(AqlValueHintBool((includeUpper && compareUpperResult <= 0) ||
                                   (!includeUpper && compareUpperResult < 0)));
}

/// @brief function TO_BOOL
AqlValue Functions::ToBool(ExpressionContext*, AstNode const&,
                           VPackFunctionParametersView parameters) {
  AqlValue const& a = extractFunctionParameterValue(parameters, 0);
  return AqlValue(AqlValueHintBool(a.toBoolean()));
}

/// @brief function TO_ARRAY
AqlValue Functions::ToArray(ExpressionContext* ctx, AstNode const&,
                            VPackFunctionParametersView parameters) {
  AqlValue const& value = extractFunctionParameterValue(parameters, 0);

  if (value.isArray()) {
    // return copy of the original array
    return value.clone();
  }

  if (value.isNull(true)) {
    return AqlValue(AqlValueHintEmptyArray());
  }

  auto* trx = &ctx->trx();
  transaction::BuilderLeaser builder(trx);
  builder->openArray();
  if (value.isBoolean() || value.isNumber() || value.isString()) {
    // return array with single member
    builder->add(value.slice());
  } else if (value.isObject()) {
    AqlValueMaterializer materializer(&trx->vpackOptions());
    VPackSlice slice = materializer.slice(value, false);
    // return an array with the attribute values
    for (auto it : VPackObjectIterator(slice, true)) {
      if (it.value.isCustom()) {
        builder->add(VPackValue(trx->extractIdString(slice)));
      } else {
        builder->add(it.value);
      }
    }
  }
  builder->close();
  return AqlValue(builder->slice(), builder->size());
}

/// @brief function LENGTH
AqlValue Functions::Length(ExpressionContext*, AstNode const&,
                           VPackFunctionParametersView parameters) {
  AqlValue const& value = extractFunctionParameterValue(parameters, 0);
  if (value.isArray()) {
    // shortcut!
    return AqlValue(AqlValueHintUInt(value.length()));
  }

  size_t length = 0;
  if (value.isNull(true)) {
    length = 0;
  } else if (value.isBoolean()) {
    if (value.toBoolean()) {
      length = 1;
    } else {
      length = 0;
    }
  } else if (value.isNumber()) {
    double tmp = value.toDouble();
    if (std::isnan(tmp) || !std::isfinite(tmp)) {
      length = 0;
    } else {
      char buffer[24];
      length = static_cast<size_t>(fpconv_dtoa(tmp, buffer));
    }
  } else if (value.isString()) {
    VPackValueLength l;
    char const* p = value.slice().getStringUnchecked(l);
    length = TRI_CharLengthUtf8String(p, l);
  } else if (value.isObject()) {
    length = static_cast<size_t>(value.length());
  }

  return AqlValue(AqlValueHintUInt(length));
}

/// @brief function FIND_FIRST
/// FIND_FIRST(text, search, start, end) → position
AqlValue Functions::FindFirst(ExpressionContext* expressionContext,
                              AstNode const&,
                              VPackFunctionParametersView parameters) {
  static char const* AFN = "FIND_FIRST";

  auto* trx = &expressionContext->trx();
  auto const& vopts = trx->vpackOptions();
  AqlValue const& value = extractFunctionParameterValue(parameters, 0);
  AqlValue const& searchValue = extractFunctionParameterValue(parameters, 1);

  transaction::StringLeaser buf1(trx);
  velocypack::StringSink adapter(buf1.get());
  ::appendAsString(vopts, adapter, value);
  icu::UnicodeString uBuf(buf1->data(), static_cast<int32_t>(buf1->length()));

  transaction::StringLeaser buf2(trx);
  velocypack::StringSink adapter2(buf2.get());
  ::appendAsString(vopts, adapter2, searchValue);
  icu::UnicodeString uSearchBuf(buf2->data(),
                                static_cast<int32_t>(buf2->length()));
  auto searchLen = uSearchBuf.length();

  int64_t startOffset = 0;
  int64_t maxEnd = -1;

  if (parameters.size() >= 3) {
    AqlValue const& optionalStartOffset =
        extractFunctionParameterValue(parameters, 2);
    startOffset = optionalStartOffset.toInt64();
    if (startOffset < 0) {
      return AqlValue(AqlValueHintInt(-1));
    }
  }

  maxEnd = uBuf.length();
  if (parameters.size() == 4) {
    AqlValue const& optionalEndMax =
        extractFunctionParameterValue(parameters, 3);
    if (!optionalEndMax.isNull(true)) {
      maxEnd = optionalEndMax.toInt64();
      if ((maxEnd < startOffset) || (maxEnd < 0)) {
        return AqlValue(AqlValueHintInt(-1));
      }
    }
  }

  if (searchLen == 0) {
    return AqlValue(AqlValueHintInt(startOffset));
  }
  if (uBuf.length() == 0) {
    return AqlValue(AqlValueHintInt(-1));
  }

  auto& server = trx->vocbase().server();
  auto locale = server.getFeature<LanguageFeature>().getLocale();
  UErrorCode status = U_ZERO_ERROR;
  icu::StringSearch search(uSearchBuf, uBuf, locale, nullptr, status);

  for (int pos = search.first(status); U_SUCCESS(status) && pos != USEARCH_DONE;
       pos = search.next(status)) {
    if (U_FAILURE(status)) {
      ::registerICUWarning(expressionContext, AFN, status);
      return AqlValue(AqlValueHintNull());
    }
    if ((pos >= startOffset) && ((pos + searchLen - 1) <= maxEnd)) {
      return AqlValue(AqlValueHintInt(pos));
    }
  }
  return AqlValue(AqlValueHintInt(-1));
}

/// @brief function FIND_LAST
/// FIND_FIRST(text, search, start, end) → position
AqlValue Functions::FindLast(ExpressionContext* expressionContext,
                             AstNode const&,
                             VPackFunctionParametersView parameters) {
  static char const* AFN = "FIND_LAST";

  auto* trx = &expressionContext->trx();
  auto const& vopts = trx->vpackOptions();
  AqlValue const& value = extractFunctionParameterValue(parameters, 0);
  AqlValue const& searchValue = extractFunctionParameterValue(parameters, 1);

  transaction::StringLeaser buf1(trx);
  velocypack::StringSink adapter(buf1.get());
  ::appendAsString(vopts, adapter, value);
  icu::UnicodeString uBuf(buf1->data(), static_cast<int32_t>(buf1->length()));

  transaction::StringLeaser buf2(trx);
  velocypack::StringSink adapter2(buf2.get());
  ::appendAsString(vopts, adapter2, searchValue);
  icu::UnicodeString uSearchBuf(buf2->data(),
                                static_cast<int32_t>(buf2->length()));
  auto searchLen = uSearchBuf.length();

  int64_t startOffset = 0;
  int64_t maxEnd = -1;

  if (parameters.size() >= 3) {
    AqlValue const& optionalStartOffset =
        extractFunctionParameterValue(parameters, 2);
    startOffset = optionalStartOffset.toInt64();
    if (startOffset < 0) {
      return AqlValue(AqlValueHintInt(-1));
    }
  }

  maxEnd = uBuf.length();
  int emptySearchCludge = 0;
  if (parameters.size() == 4) {
    AqlValue const& optionalEndMax =
        extractFunctionParameterValue(parameters, 3);
    if (!optionalEndMax.isNull(true)) {
      maxEnd = optionalEndMax.toInt64();
      if ((maxEnd < startOffset) || (maxEnd < 0)) {
        return AqlValue(AqlValueHintInt(-1));
      }
      emptySearchCludge = 1;
    }
  }

  if (searchLen == 0) {
    return AqlValue(AqlValueHintInt(maxEnd + emptySearchCludge));
  }
  if (uBuf.length() == 0) {
    return AqlValue(AqlValueHintInt(-1));
  }

  auto& server = trx->vocbase().server();
  auto locale = server.getFeature<LanguageFeature>().getLocale();
  UErrorCode status = U_ZERO_ERROR;
  icu::StringSearch search(uSearchBuf, uBuf, locale, nullptr, status);

  int foundPos = -1;
  for (int pos = search.first(status); U_SUCCESS(status) && pos != USEARCH_DONE;
       pos = search.next(status)) {
    if (U_FAILURE(status)) {
      ::registerICUWarning(expressionContext, AFN, status);
      return AqlValue(AqlValueHintNull());
    }
    if ((pos >= startOffset) && ((pos + searchLen - 1) <= maxEnd)) {
      foundPos = pos;
    }
  }
  return AqlValue(AqlValueHintInt(foundPos));
}

/// @brief function REVERSE
AqlValue Functions::Reverse(ExpressionContext* expressionContext,
                            AstNode const&,
                            VPackFunctionParametersView parameters) {
  static char const* AFN = "REVERSE";

  transaction::Methods* trx = &expressionContext->trx();
  auto const& vopts = trx->vpackOptions();
  AqlValue const& value = extractFunctionParameterValue(parameters, 0);

  if (value.isArray()) {
    transaction::BuilderLeaser builder(trx);
    AqlValueMaterializer materializer(&vopts);
    VPackSlice slice = materializer.slice(value, false);
    std::vector<VPackSlice> array;
    array.reserve(slice.length());
    for (VPackSlice it : VPackArrayIterator(slice)) {
      array.push_back(it);
    }
    std::reverse(std::begin(array), std::end(array));

    builder->openArray();
    for (auto const& it : array) {
      builder->add(it);
    }
    builder->close();
    return AqlValue(builder->slice(), builder->size());
  } else if (value.isString()) {
    std::string utf8;
    transaction::StringLeaser buf1(trx);
    velocypack::StringSink adapter(buf1.get());
    ::appendAsString(vopts, adapter, value);
    icu::UnicodeString uBuf(buf1->data(), static_cast<int32_t>(buf1->length()));
    // reserve the result buffer, but need to set empty afterwards:
    icu::UnicodeString result;
    result.getBuffer(uBuf.length());
    result = "";
    icu::StringCharacterIterator iter(uBuf, uBuf.length());
    UChar c = iter.previous();
    while (c != icu::CharacterIterator::DONE) {
      result.append(c);
      c = iter.previous();
    }
    result.toUTF8String(utf8);

    return AqlValue(utf8);
  } else {
    // neither array nor string...
    registerWarning(expressionContext, AFN, TRI_ERROR_QUERY_ARRAY_EXPECTED);
    return AqlValue(AqlValueHintNull());
  }
}

/// @brief function FIRST
AqlValue Functions::First(ExpressionContext* expressionContext, AstNode const&,
                          VPackFunctionParametersView parameters) {
  // cppcheck-suppress variableScope
  static char const* AFN = "FIRST";

  AqlValue const& value = extractFunctionParameterValue(parameters, 0);

  if (!value.isArray()) {
    // not an array
    registerWarning(expressionContext, AFN, TRI_ERROR_QUERY_ARRAY_EXPECTED);
    return AqlValue(AqlValueHintNull());
  }

  if (value.length() == 0) {
    return AqlValue(AqlValueHintNull());
  }

  bool mustDestroy;
  return value.at(0, mustDestroy, true);
}

/// @brief function LAST
AqlValue Functions::Last(ExpressionContext* expressionContext, AstNode const&,
                         VPackFunctionParametersView parameters) {
  // cppcheck-suppress variableScope
  static char const* AFN = "LAST";

  AqlValue const& value = extractFunctionParameterValue(parameters, 0);

  if (!value.isArray()) {
    // not an array
    registerWarning(expressionContext, AFN, TRI_ERROR_QUERY_ARRAY_EXPECTED);
    return AqlValue(AqlValueHintNull());
  }

  VPackValueLength const n = value.length();

  if (n == 0) {
    return AqlValue(AqlValueHintNull());
  }

  bool mustDestroy;
  return value.at(n - 1, mustDestroy, true);
}

/// @brief function NTH
AqlValue Functions::Nth(ExpressionContext* expressionContext, AstNode const&,
                        VPackFunctionParametersView parameters) {
  // cppcheck-suppress variableScope
  static char const* AFN = "NTH";

  AqlValue const& value = extractFunctionParameterValue(parameters, 0);

  if (!value.isArray()) {
    // not an array
    registerWarning(expressionContext, AFN, TRI_ERROR_QUERY_ARRAY_EXPECTED);
    return AqlValue(AqlValueHintNull());
  }

  VPackValueLength const n = value.length();

  if (n == 0) {
    return AqlValue(AqlValueHintNull());
  }

  AqlValue const& position = extractFunctionParameterValue(parameters, 1);
  int64_t index = position.toInt64();

  if (index < 0 || index >= static_cast<int64_t>(n)) {
    return AqlValue(AqlValueHintNull());
  }

  bool mustDestroy;
  return value.at(index, mustDestroy, true);
}

/// @brief function CONTAINS
AqlValue Functions::Contains(ExpressionContext* ctx, AstNode const&,
                             VPackFunctionParametersView parameters) {
  auto* trx = &ctx->trx();
  auto const& vopts = trx->vpackOptions();
  AqlValue const& value = extractFunctionParameterValue(parameters, 0);
  AqlValue const& search = extractFunctionParameterValue(parameters, 1);
  AqlValue const& returnIndex = extractFunctionParameterValue(parameters, 2);

  bool const willReturnIndex = returnIndex.toBoolean();

  int result = -1;  // default is "not found"
  {
    transaction::StringLeaser buffer(trx);
    velocypack::StringSink adapter(buffer.get());

    ::appendAsString(vopts, adapter, value);
    size_t const valueLength = buffer->length();

    size_t const searchOffset = buffer->length();
    ::appendAsString(vopts, adapter, search);
    size_t const searchLength = buffer->length() - valueLength;

    if (searchLength > 0) {
      char const* found = static_cast<char const*>(
          memmem(buffer->data(), valueLength, buffer->data() + searchOffset,
                 searchLength));

      if (found != nullptr) {
        if (willReturnIndex) {
          // find offset into string
          int bytePosition = static_cast<int>(found - buffer->data());
          char const* p = buffer->data();
          int pos = 0;
          while (pos < bytePosition) {
            unsigned char c = static_cast<unsigned char>(*p);
            if (c < 128) {
              ++pos;
            } else if (c < 224) {
              pos += 2;
            } else if (c < 240) {
              pos += 3;
            } else if (c < 248) {
              pos += 4;
            }
          }
          result = pos;
        } else {
          // fake result position, but it does not matter as it will
          // only be compared to -1 later
          result = 0;
        }
      }
    }
  }

  if (willReturnIndex) {
    // return numeric value
    return AqlValue(AqlValueHintInt(result));
  }

  // return boolean
  return AqlValue(AqlValueHintBool(result != -1));
}

/// @brief function CONCAT
AqlValue Functions::Concat(ExpressionContext* ctx, AstNode const&,
                           VPackFunctionParametersView parameters) {
  transaction::Methods* trx = &ctx->trx();
  auto const& vopts = trx->vpackOptions();
  transaction::StringLeaser buffer(trx);
  velocypack::StringSink adapter(buffer.get());

  size_t const n = parameters.size();

  if (n == 1) {
    AqlValue const& member = extractFunctionParameterValue(parameters, 0);
    if (member.isArray()) {
      AqlValueMaterializer materializer(&vopts);
      VPackSlice slice = materializer.slice(member, false);

      for (VPackSlice it : VPackArrayIterator(slice)) {
        if (it.isNull()) {
          continue;
        }
        // convert member to a string and append
        ::appendAsString(vopts, adapter, AqlValue(it.begin()));
      }
      return AqlValue(buffer->data(), buffer->length());
    }
  }

  for (size_t i = 0; i < n; ++i) {
    AqlValue const& member = extractFunctionParameterValue(parameters, i);

    if (member.isNull(true)) {
      continue;
    }

    // convert member to a string and append
    ::appendAsString(vopts, adapter, member);
  }

  return AqlValue(buffer->data(), buffer->length());
}

/// @brief function CONCAT_SEPARATOR
AqlValue Functions::ConcatSeparator(ExpressionContext* ctx, AstNode const&,
                                    VPackFunctionParametersView parameters) {
  transaction::Methods* trx = &ctx->trx();
  auto const& vopts = trx->vpackOptions();
  transaction::StringLeaser buffer(trx);
  velocypack::StringSink adapter(buffer.get());

  bool found = false;
  size_t const n = parameters.size();

  AqlValue const& separator = extractFunctionParameterValue(parameters, 0);
  ::appendAsString(vopts, adapter, separator);
  std::string const plainStr(buffer->data(), buffer->length());

  buffer->clear();

  if (n == 2) {
    AqlValue const& member = extractFunctionParameterValue(parameters, 1);

    if (member.isArray()) {
      // reserve *some* space
      buffer->reserve((plainStr.size() + 10) * member.length());

      AqlValueMaterializer materializer(&vopts);
      VPackSlice slice = materializer.slice(member, false);

      for (VPackSlice it : VPackArrayIterator(slice)) {
        if (it.isNull()) {
          continue;
        }
        if (found) {
          buffer->append(plainStr);
        }
        // convert member to a string and append
        ::appendAsString(vopts, adapter, AqlValue(it.begin()));
        found = true;
      }
      return AqlValue(buffer->data(), buffer->length());
    }
  }

  // reserve *some* space
  buffer->reserve((plainStr.size() + 10) * n);
  for (size_t i = 1; i < n; ++i) {
    AqlValue const& member = extractFunctionParameterValue(parameters, i);

    if (member.isNull(true)) {
      continue;
    }
    if (found) {
      buffer->append(plainStr);
    }

    // convert member to a string and append
    ::appendAsString(vopts, adapter, member);
    found = true;
  }

  return AqlValue(buffer->data(), buffer->length());
}

/// @brief function CHAR_LENGTH
AqlValue Functions::CharLength(ExpressionContext* ctx, AstNode const&,
                               VPackFunctionParametersView parameters) {
  transaction::Methods* trx = &ctx->trx();
  auto* vopts = &trx->vpackOptions();
  AqlValue const& value = extractFunctionParameterValue(parameters, 0);
  size_t length = 0;

  if (value.isArray() || value.isObject()) {
    AqlValueMaterializer materializer(vopts);
    VPackSlice slice = materializer.slice(value, false);

    transaction::StringLeaser buffer(trx);
    velocypack::StringSink adapter(buffer.get());

    VPackDumper dumper(&adapter, vopts);
    dumper.dump(slice);

    length = buffer->length();

  } else if (value.isNull(true)) {
    length = 0;

  } else if (value.isBoolean()) {
    if (value.toBoolean()) {
      length = 4;
    } else {
      length = 5;
    }

  } else if (value.isNumber()) {
    double tmp = value.toDouble();
    if (std::isnan(tmp) || !std::isfinite(tmp)) {
      length = 0;
    } else {
      char buffer[24];
      length = static_cast<size_t>(fpconv_dtoa(tmp, buffer));
    }

  } else if (value.isString()) {
    VPackValueLength l;
    char const* p = value.slice().getStringUnchecked(l);
    length = TRI_CharLengthUtf8String(p, l);
  }

  return AqlValue(AqlValueHintUInt(length));
}

/// @brief function LOWER
AqlValue Functions::Lower(ExpressionContext* ctx, AstNode const&,
                          VPackFunctionParametersView parameters) {
  std::string utf8;
  transaction::Methods* trx = &ctx->trx();
  auto const& vopts = trx->vpackOptions();
  AqlValue const& value = extractFunctionParameterValue(parameters, 0);

  transaction::StringLeaser buffer(trx);
  velocypack::StringSink adapter(buffer.get());

  ::appendAsString(vopts, adapter, value);

  icu::UnicodeString unicodeStr(buffer->data(),
                                static_cast<int32_t>(buffer->length()));
  unicodeStr.toLower(nullptr);
  unicodeStr.toUTF8String(utf8);

  return AqlValue(utf8);
}

/// @brief function UPPER
AqlValue Functions::Upper(ExpressionContext* ctx, AstNode const&,
                          VPackFunctionParametersView parameters) {
  std::string utf8;
  transaction::Methods* trx = &ctx->trx();
  auto const& vopts = trx->vpackOptions();
  AqlValue const& value = extractFunctionParameterValue(parameters, 0);

  transaction::StringLeaser buffer(trx);
  velocypack::StringSink adapter(buffer.get());

  ::appendAsString(vopts, adapter, value);

  icu::UnicodeString unicodeStr(buffer->data(),
                                static_cast<int32_t>(buffer->length()));
  unicodeStr.toUpper(nullptr);
  unicodeStr.toUTF8String(utf8);

  return AqlValue(utf8);
}

/// @brief function SUBSTRING
AqlValue Functions::Substring(ExpressionContext* ctx, AstNode const&,
                              VPackFunctionParametersView parameters) {
  transaction::Methods* trx = &ctx->trx();
  auto const& vopts = trx->vpackOptions();
  AqlValue const& value = extractFunctionParameterValue(parameters, 0);

  int32_t length = INT32_MAX;

  transaction::StringLeaser buffer(trx);
  velocypack::StringSink adapter(buffer.get());

  ::appendAsString(vopts, adapter, value);
  icu::UnicodeString unicodeStr(buffer->data(),
                                static_cast<int32_t>(buffer->length()));

  int32_t offset = static_cast<int32_t>(
      extractFunctionParameterValue(parameters, 1).toInt64());

  if (parameters.size() == 3) {
    length = static_cast<int32_t>(
        extractFunctionParameterValue(parameters, 2).toInt64());
  }

  if (offset < 0) {
    offset = unicodeStr.moveIndex32(
        unicodeStr.moveIndex32(unicodeStr.length(), 0), offset);
  } else {
    offset = unicodeStr.moveIndex32(0, offset);
  }

  std::string utf8;
  unicodeStr
      .tempSubString(offset, unicodeStr.moveIndex32(offset, length) - offset)
      .toUTF8String(utf8);

  return AqlValue(utf8);
}
////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////

AqlValue Functions::Substitute(ExpressionContext* expressionContext,
                               AstNode const&,
                               VPackFunctionParametersView parameters) {
  static char const* AFN = "SUBSTITUTE";

  transaction::Methods* trx = &expressionContext->trx();
  auto const& vopts = trx->vpackOptions();
  AqlValue const& search = extractFunctionParameterValue(parameters, 1);
  int64_t limit = -1;
  AqlValueMaterializer materializer(&vopts);
  std::vector<icu::UnicodeString> matchPatterns;
  std::vector<icu::UnicodeString> replacePatterns;
  bool replaceWasPlainString = false;

  if (search.isObject()) {
    if (parameters.size() > 3) {
      registerWarning(expressionContext, AFN,
                      TRI_ERROR_QUERY_FUNCTION_ARGUMENT_NUMBER_MISMATCH);
      return AqlValue(AqlValueHintNull());
    }
    if (parameters.size() == 3) {
      limit = extractFunctionParameterValue(parameters, 2).toInt64();
    }
    VPackSlice slice = materializer.slice(search, false);
    matchPatterns.reserve(slice.length());
    replacePatterns.reserve(slice.length());
    for (auto it : VPackObjectIterator(slice)) {
      velocypack::ValueLength length;
      char const* str = it.key.getString(length);
      matchPatterns.push_back(
          icu::UnicodeString(str, static_cast<int32_t>(length)));
      if (it.value.isNull()) {
        // null replacement value => replace with an empty string
        replacePatterns.push_back(icu::UnicodeString("", int32_t(0)));
      } else if (it.value.isString()) {
        // string case
        str = it.value.getStringUnchecked(length);
        replacePatterns.push_back(
            icu::UnicodeString(str, static_cast<int32_t>(length)));
      } else {
        // non strings
        registerInvalidArgumentWarning(expressionContext, AFN);
        return AqlValue(AqlValueHintNull());
      }
    }
  } else {
    if (parameters.size() < 2) {
      registerWarning(expressionContext, AFN,
                      TRI_ERROR_QUERY_FUNCTION_ARGUMENT_NUMBER_MISMATCH);
      return AqlValue(AqlValueHintNull());
    }
    if (parameters.size() == 4) {
      limit = extractFunctionParameterValue(parameters, 3).toInt64();
    }

    VPackSlice slice = materializer.slice(search, false);
    if (search.isArray()) {
      for (VPackSlice it : VPackArrayIterator(slice)) {
        if (it.isString()) {
          velocypack::ValueLength length;
          char const* str = it.getStringUnchecked(length);
          matchPatterns.push_back(
              icu::UnicodeString(str, static_cast<int32_t>(length)));
        } else {
          registerInvalidArgumentWarning(expressionContext, AFN);
          return AqlValue(AqlValueHintNull());
        }
      }
    } else {
      if (!search.isString()) {
        registerInvalidArgumentWarning(expressionContext, AFN);
        return AqlValue(AqlValueHintNull());
      }
      velocypack::ValueLength length;

      char const* str = slice.getStringUnchecked(length);
      matchPatterns.push_back(
          icu::UnicodeString(str, static_cast<int32_t>(length)));
    }
    if (parameters.size() > 2) {
      AqlValue const& replace = extractFunctionParameterValue(parameters, 2);
      AqlValueMaterializer materializer2(&vopts);
      VPackSlice rslice = materializer2.slice(replace, false);
      if (replace.isArray()) {
        for (VPackSlice it : VPackArrayIterator(rslice)) {
          if (it.isNull()) {
            // null replacement value => replace with an empty string
            replacePatterns.push_back(icu::UnicodeString("", int32_t(0)));
          } else if (it.isString()) {
            velocypack::ValueLength length;
            char const* str = it.getStringUnchecked(length);
            replacePatterns.push_back(
                icu::UnicodeString(str, static_cast<int32_t>(length)));
          } else {
            registerInvalidArgumentWarning(expressionContext, AFN);
            return AqlValue(AqlValueHintNull());
          }
        }
      } else if (replace.isString()) {
        // If we have a string as replacement,
        // it counts in for all found values.
        replaceWasPlainString = true;
        velocypack::ValueLength length;
        char const* str = rslice.getString(length);
        replacePatterns.push_back(
            icu::UnicodeString(str, static_cast<int32_t>(length)));
      } else {
        registerInvalidArgumentWarning(expressionContext, AFN);
        return AqlValue(AqlValueHintNull());
      }
    }
  }

  AqlValue const& value = extractFunctionParameterValue(parameters, 0);
  if ((limit == 0) || (matchPatterns.size() == 0)) {
    // if the limit is 0, or we don't have any match pattern, return the source
    // string.
    return AqlValue(value);
  }

  transaction::StringLeaser buffer(trx);
  velocypack::StringSink adapter(buffer.get());

  ::appendAsString(vopts, adapter, value);
  icu::UnicodeString unicodeStr(buffer->data(),
                                static_cast<int32_t>(buffer->length()));

  auto& server = trx->vocbase().server();
  auto locale = server.getFeature<LanguageFeature>().getLocale();
  // we can't copy the search instances, thus use pointers:
  std::vector<std::unique_ptr<icu::StringSearch>> searchVec;
  searchVec.reserve(matchPatterns.size());
  UErrorCode status = U_ZERO_ERROR;
  for (auto const& searchStr : matchPatterns) {
    // create a vector of string searches
    searchVec.push_back(std::make_unique<icu::StringSearch>(
        searchStr, unicodeStr, locale, nullptr, status));
    if (U_FAILURE(status)) {
      ::registerICUWarning(expressionContext, AFN, status);
      return AqlValue(AqlValueHintNull());
    }
  }

  std::vector<std::pair<int32_t, int32_t>> srchResultPtrs;
  std::string utf8;
  srchResultPtrs.reserve(matchPatterns.size());
  for (auto& search : searchVec) {
    // We now find the first hit for each search string.
    auto pos = search->first(status);
    if (U_FAILURE(status)) {
      ::registerICUWarning(expressionContext, AFN, status);
      return AqlValue(AqlValueHintNull());
    }

    int32_t len = 0;
    if (pos != USEARCH_DONE) {
      len = search->getMatchedLength();
    }
    srchResultPtrs.push_back(std::make_pair(pos, len));
  }

  icu::UnicodeString result;
  int32_t lastStart = 0;
  int64_t count = 0;
  while (true) {
    int which = -1;
    int32_t pos = USEARCH_DONE;
    int32_t mLen = 0;
    int i = 0;
    for (auto resultPair : srchResultPtrs) {
      // We locate the nearest matching search result.
      int32_t thisPos;
      thisPos = resultPair.first;
      if ((pos == USEARCH_DONE) || (pos > thisPos)) {
        if (thisPos != USEARCH_DONE) {
          pos = thisPos;
          which = i;
          mLen = resultPair.second;
        }
      }
      i++;
    }
    if (which == -1) {
      break;
    }
    // from last match to this match, copy the original string.
    result.append(unicodeStr, lastStart, pos - lastStart);
    if (replacePatterns.size() != 0) {
      if (replacePatterns.size() > (size_t)which) {
        result.append(replacePatterns[which]);
      } else if (replaceWasPlainString) {
        result.append(replacePatterns[0]);
      }
    }

    // lastStart is the place up to we searched the source string
    lastStart = pos + mLen;

    // we try to search the next occurance of this string
    auto& search = searchVec[which];
    pos = search->next(status);
    if (U_FAILURE(status)) {
      ::registerICUWarning(expressionContext, AFN, status);
      return AqlValue(AqlValueHintNull());
    }
    if (pos != USEARCH_DONE) {
      mLen = search->getMatchedLength();
    } else {
      mLen = -1;
    }
    srchResultPtrs[which] = std::make_pair(pos, mLen);

    which = 0;
    for (auto searchPair : srchResultPtrs) {
      // now we invalidate all search results that overlap with
      // our last search result and see whether we can find the
      // overlapped pattern again.
      // However, that mustn't overlap with the current lastStart
      // position either.
      int32_t thisPos;
      thisPos = searchPair.first;
      if ((thisPos != USEARCH_DONE) && (thisPos < lastStart)) {
        auto& search = searchVec[which];
        pos = thisPos;
        while ((pos < lastStart) && (pos != USEARCH_DONE)) {
          pos = search->next(status);
          if (U_FAILURE(status)) {
            ::registerICUWarning(expressionContext, AFN, status);
            return AqlValue(AqlValueHintNull());
          }
          if (pos != USEARCH_DONE) {
            mLen = search->getMatchedLength();
          }
          srchResultPtrs[which] = std::make_pair(pos, mLen);
        }
      }
      which++;
    }

    count++;
    if ((limit != -1) && (count >= limit)) {
      // Do we have a limit count?
      break;
    }
    // check whether none of our search objects has any more results
    bool allFound = true;
    for (auto resultPair : srchResultPtrs) {
      if (resultPair.first != USEARCH_DONE) {
        allFound = false;
        break;
      }
    }
    if (allFound) {
      break;
    }
  }
  // Append from the last found:
  result.append(unicodeStr, lastStart, unicodeStr.length() - lastStart);

  result.toUTF8String(utf8);
  return AqlValue(utf8);
}

/// @brief function LEFT str, length
AqlValue Functions::Left(ExpressionContext* ctx, AstNode const&,
                         VPackFunctionParametersView parameters) {
  transaction::Methods* trx = &ctx->trx();
  auto const& vopts = trx->vpackOptions();
  AqlValue value = extractFunctionParameterValue(parameters, 0);
  uint32_t length = static_cast<int32_t>(
      extractFunctionParameterValue(parameters, 1).toInt64());

  std::string utf8;
  transaction::StringLeaser buffer(trx);
  velocypack::StringSink adapter(buffer.get());

  ::appendAsString(vopts, adapter, value);

  icu::UnicodeString unicodeStr(buffer->data(),
                                static_cast<int32_t>(buffer->length()));
  icu::UnicodeString left =
      unicodeStr.tempSubString(0, unicodeStr.moveIndex32(0, length));

  left.toUTF8String(utf8);
  return AqlValue(utf8);
}

/// @brief function RIGHT
AqlValue Functions::Right(ExpressionContext* ctx, AstNode const&,
                          VPackFunctionParametersView parameters) {
  transaction::Methods* trx = &ctx->trx();
  auto const& vopts = trx->vpackOptions();
  AqlValue value = extractFunctionParameterValue(parameters, 0);
  uint32_t length = static_cast<int32_t>(
      extractFunctionParameterValue(parameters, 1).toInt64());

  std::string utf8;
  transaction::StringLeaser buffer(trx);
  velocypack::StringSink adapter(buffer.get());

  ::appendAsString(vopts, adapter, value);

  icu::UnicodeString unicodeStr(buffer->data(),
                                static_cast<int32_t>(buffer->length()));
  icu::UnicodeString right = unicodeStr.tempSubString(unicodeStr.moveIndex32(
      unicodeStr.length(), -static_cast<int32_t>(length)));

  right.toUTF8String(utf8);
  return AqlValue(utf8);
}

namespace {
void ltrimInternal(int32_t& startOffset, int32_t& endOffset,
                   icu::UnicodeString& unicodeStr, uint32_t numWhitespaces,
                   UChar32* spaceChars) {
  for (; startOffset < endOffset;
       startOffset = unicodeStr.moveIndex32(startOffset, 1)) {
    bool found = false;

    for (uint32_t pos = 0; pos < numWhitespaces; pos++) {
      if (unicodeStr.char32At(startOffset) == spaceChars[pos]) {
        found = true;
        break;
      }
    }

    if (!found) {
      break;
    }
  }  // for
}

void rtrimInternal(int32_t& startOffset, int32_t& endOffset,
                   icu::UnicodeString& unicodeStr, uint32_t numWhitespaces,
                   UChar32* spaceChars) {
  if (unicodeStr.length() == 0) {
    return;
  }
  for (int32_t codePos = unicodeStr.moveIndex32(endOffset, -1);
       startOffset <= codePos; codePos = unicodeStr.moveIndex32(codePos, -1)) {
    bool found = false;

    for (uint32_t pos = 0; pos < numWhitespaces; pos++) {
      if (unicodeStr.char32At(codePos) == spaceChars[pos]) {
        found = true;
        --endOffset;
        break;
      }
    }

    if (!found || codePos == 0) {
      break;
    }
  }  // for
}
}  // namespace

/// @brief function TRIM
AqlValue Functions::Trim(ExpressionContext* expressionContext, AstNode const&,
                         VPackFunctionParametersView parameters) {
  // cppcheck-suppress variableScope
  static char const* AFN = "TRIM";

  transaction::Methods* trx = &expressionContext->trx();
  auto const& vopts = trx->vpackOptions();
  AqlValue const& value = extractFunctionParameterValue(parameters, 0);
  transaction::StringLeaser buffer(trx);
  velocypack::StringSink adapter(buffer.get());
  ::appendAsString(vopts, adapter, value);
  icu::UnicodeString unicodeStr(buffer->data(),
                                static_cast<int32_t>(buffer->length()));

  int64_t howToTrim = 0;
  icu::UnicodeString whitespace("\r\n\t ");

  if (parameters.size() == 2) {
    AqlValue const& optional = extractFunctionParameterValue(parameters, 1);

    if (optional.isNumber()) {
      howToTrim = optional.toInt64();

      if (howToTrim < 0 || 2 < howToTrim) {
        howToTrim = 0;
      }
    } else if (optional.isString()) {
      buffer->clear();
      ::appendAsString(vopts, adapter, optional);
      whitespace = icu::UnicodeString(buffer->data(),
                                      static_cast<int32_t>(buffer->length()));
    }
  }

  uint32_t numWhitespaces = whitespace.countChar32();
  UErrorCode errorCode = U_ZERO_ERROR;
  auto spaceChars = std::make_unique<UChar32[]>(numWhitespaces);

  whitespace.toUTF32(spaceChars.get(), numWhitespaces, errorCode);
  if (U_FAILURE(errorCode)) {
    ::registerICUWarning(expressionContext, AFN, errorCode);
    return AqlValue(AqlValueHintNull());
  }

  int32_t startOffset = 0, endOffset = unicodeStr.length();

  if (howToTrim <= 1) {
    ltrimInternal(startOffset, endOffset, unicodeStr, numWhitespaces,
                  spaceChars.get());
  }

  if (howToTrim == 2 || howToTrim == 0) {
    rtrimInternal(startOffset, endOffset, unicodeStr, numWhitespaces,
                  spaceChars.get());
  }

  icu::UnicodeString result =
      unicodeStr.tempSubString(startOffset, endOffset - startOffset);
  std::string utf8;
  result.toUTF8String(utf8);
  return AqlValue(utf8);
}

/// @brief function LTRIM
AqlValue Functions::LTrim(ExpressionContext* expressionContext, AstNode const&,
                          VPackFunctionParametersView parameters) {
  // cppcheck-suppress variableScope
  static char const* AFN = "LTRIM";

  transaction::Methods* trx = &expressionContext->trx();
  auto const& vopts = trx->vpackOptions();
  AqlValue const& value = extractFunctionParameterValue(parameters, 0);
  transaction::StringLeaser buffer(trx);
  velocypack::StringSink adapter(buffer.get());
  ::appendAsString(vopts, adapter, value);
  icu::UnicodeString unicodeStr(buffer->data(),
                                static_cast<int32_t>(buffer->length()));
  icu::UnicodeString whitespace("\r\n\t ");

  if (parameters.size() == 2) {
    AqlValue const& pWhitespace = extractFunctionParameterValue(parameters, 1);
    buffer->clear();
    ::appendAsString(vopts, adapter, pWhitespace);
    whitespace = icu::UnicodeString(buffer->data(),
                                    static_cast<int32_t>(buffer->length()));
  }

  uint32_t numWhitespaces = whitespace.countChar32();
  UErrorCode errorCode = U_ZERO_ERROR;
  auto spaceChars = std::make_unique<UChar32[]>(numWhitespaces);

  whitespace.toUTF32(spaceChars.get(), numWhitespaces, errorCode);
  if (U_FAILURE(errorCode)) {
    ::registerICUWarning(expressionContext, AFN, errorCode);
    return AqlValue(AqlValueHintNull());
  }

  int32_t startOffset = 0, endOffset = unicodeStr.length();

  ltrimInternal(startOffset, endOffset, unicodeStr, numWhitespaces,
                spaceChars.get());

  icu::UnicodeString result =
      unicodeStr.tempSubString(startOffset, endOffset - startOffset);
  std::string utf8;
  result.toUTF8String(utf8);
  return AqlValue(utf8);
}

/// @brief function RTRIM
AqlValue Functions::RTrim(ExpressionContext* expressionContext, AstNode const&,
                          VPackFunctionParametersView parameters) {
  // cppcheck-suppress variableScope
  static char const* AFN = "RTRIM";

  transaction::Methods* trx = &expressionContext->trx();
  auto const& vopts = trx->vpackOptions();
  AqlValue const& value = extractFunctionParameterValue(parameters, 0);
  transaction::StringLeaser buffer(trx);
  velocypack::StringSink adapter(buffer.get());
  ::appendAsString(vopts, adapter, value);
  icu::UnicodeString unicodeStr(buffer->data(),
                                static_cast<int32_t>(buffer->length()));
  icu::UnicodeString whitespace("\r\n\t ");

  if (parameters.size() == 2) {
    AqlValue const& pWhitespace = extractFunctionParameterValue(parameters, 1);
    buffer->clear();
    ::appendAsString(vopts, adapter, pWhitespace);
    whitespace = icu::UnicodeString(buffer->data(),
                                    static_cast<int32_t>(buffer->length()));
  }

  uint32_t numWhitespaces = whitespace.countChar32();
  UErrorCode errorCode = U_ZERO_ERROR;
  auto spaceChars = std::make_unique<UChar32[]>(numWhitespaces);

  whitespace.toUTF32(spaceChars.get(), numWhitespaces, errorCode);
  if (U_FAILURE(errorCode)) {
    ::registerICUWarning(expressionContext, AFN, errorCode);
    return AqlValue(AqlValueHintNull());
  }

  int32_t startOffset = 0, endOffset = unicodeStr.length();

  rtrimInternal(startOffset, endOffset, unicodeStr, numWhitespaces,
                spaceChars.get());

  icu::UnicodeString result =
      unicodeStr.tempSubString(startOffset, endOffset - startOffset);
  std::string utf8;
  result.toUTF8String(utf8);
  return AqlValue(utf8);
}

/// @brief function LIKE
AqlValue Functions::Like(ExpressionContext* expressionContext, AstNode const&,
                         VPackFunctionParametersView parameters) {
  static char const* AFN = "LIKE";

  transaction::Methods* trx = &expressionContext->trx();
  auto const& vopts = trx->vpackOptions();
  bool const caseInsensitive = ::getBooleanParameter(parameters, 2, false);
  transaction::StringLeaser buffer(trx);
  velocypack::StringSink adapter(buffer.get());

  // build pattern from parameter #1
  AqlValue const& regex = extractFunctionParameterValue(parameters, 1);
  ::appendAsString(vopts, adapter, regex);

  // the matcher is owned by the context!
  icu::RegexMatcher* matcher = expressionContext->buildLikeMatcher(
      buffer->data(), buffer->length(), caseInsensitive);

  if (matcher == nullptr) {
    // compiling regular expression failed
    registerWarning(expressionContext, AFN, TRI_ERROR_QUERY_INVALID_REGEX);
    return AqlValue(AqlValueHintNull());
  }

  // extract value
  buffer->clear();
  AqlValue const& value = extractFunctionParameterValue(parameters, 0);
  ::appendAsString(vopts, adapter, value);

  bool error = false;
  bool const result = basics::Utf8Helper::DefaultUtf8Helper.matches(
      matcher, buffer->data(), buffer->length(), false, error);

  if (error) {
    // compiling regular expression failed
    registerWarning(expressionContext, AFN, TRI_ERROR_QUERY_INVALID_REGEX);
    return AqlValue(AqlValueHintNull());
  }

  return AqlValue(AqlValueHintBool(result));
}

/// @brief function SPLIT
AqlValue Functions::Split(ExpressionContext* expressionContext, AstNode const&,
                          VPackFunctionParametersView parameters) {
  static char const* AFN = "SPLIT";

  transaction::Methods* trx = &expressionContext->trx();
  auto* vopts = &trx->vpackOptions();
  // cheapest parameter checks first:
  int64_t limitNumber = -1;
  if (parameters.size() == 3) {
    AqlValue const& aqlLimit = extractFunctionParameterValue(parameters, 2);
    if (aqlLimit.isNumber()) {
      limitNumber = aqlLimit.toInt64();
    } else {
      registerInvalidArgumentWarning(expressionContext, AFN);
      return AqlValue(AqlValueHintNull());
    }

    // these are edge cases which are documented to have these return values:
    if (limitNumber < 0) {
      return AqlValue(AqlValueHintNull());
    }
    if (limitNumber == 0) {
      return AqlValue(AqlValueHintEmptyArray());
    }
  }

  transaction::StringLeaser regexBuffer(trx);
  AqlValue aqlSeparatorExpression;
  if (parameters.size() >= 2) {
    aqlSeparatorExpression = extractFunctionParameterValue(parameters, 1);
    if (aqlSeparatorExpression.isObject()) {
      registerInvalidArgumentWarning(expressionContext, AFN);
      return AqlValue(AqlValueHintNull());
    }
  }

  AqlValue const& aqlValueToSplit =
      extractFunctionParameterValue(parameters, 0);

  if (parameters.size() == 1) {
    // pre-documented edge-case: if we only have the first parameter, return it.
    transaction::BuilderLeaser builder(trx);
    builder->openArray();
    builder->add(aqlValueToSplit.slice());
    builder->close();
    return AqlValue(builder->slice(), builder->size());
  }

  // Get ready for ICU
  transaction::StringLeaser buffer(trx);
  velocypack::StringSink adapter(buffer.get());
  Stringify(vopts, adapter, aqlValueToSplit.slice());
  icu::UnicodeString valueToSplit(buffer->data(),
                                  static_cast<int32_t>(buffer->length()));
  bool isEmptyExpression = false;

  // the matcher is owned by the context!
  icu::RegexMatcher* matcher = expressionContext->buildSplitMatcher(
      aqlSeparatorExpression, &trx->vpackOptions(), isEmptyExpression);

  if (matcher == nullptr) {
    // compiling regular expression failed
    registerWarning(expressionContext, AFN, TRI_ERROR_QUERY_INVALID_REGEX);
    return AqlValue(AqlValueHintNull());
  }

  transaction::BuilderLeaser result(trx);
  result->openArray();
  if (!isEmptyExpression && (buffer->length() == 0)) {
    // Edge case: splitting an empty string by non-empty expression produces an
    // empty string again.
    result->add(VPackValue(""));
    result->close();
    return AqlValue(result->slice(), result->size());
  }

  std::string utf8;
  static const uint16_t nrResults = 16;
  icu::UnicodeString uResults[nrResults];
  int64_t totalCount = 0;
  while (true) {
    UErrorCode errorCode = U_ZERO_ERROR;
    auto uCount = matcher->split(valueToSplit, uResults, nrResults, errorCode);
    uint16_t copyThisTime = uCount;

    if (U_FAILURE(errorCode)) {
      registerWarning(expressionContext, AFN, TRI_ERROR_QUERY_INVALID_REGEX);
      return AqlValue(AqlValueHintNull());
    }

    if ((copyThisTime > 0) && (copyThisTime > nrResults)) {
      // last hit is the remaining string to be fed into split in a subsequent
      // invocation
      copyThisTime--;
    }

    if ((copyThisTime > 0) &&
        ((copyThisTime == nrResults) || isEmptyExpression)) {
      // ICU will give us a traling empty string we don't care for if we split
      // with empty strings.
      copyThisTime--;
    }

    int64_t i = 0;
    while ((i < copyThisTime) &&
           ((limitNumber < 0) || (totalCount < limitNumber))) {
      if ((i == 0) && isEmptyExpression) {
        // ICU will give us an empty string that we don't care for
        // as first value of one match-chunk
        i++;
        continue;
      }
      uResults[i].toUTF8String(utf8);
      result->add(VPackValue(utf8));
      utf8.clear();
      i++;
      totalCount++;
    }

    if (((uCount != nrResults)) ||  // fetch any / found less then N
        ((limitNumber >= 0) && (totalCount >= limitNumber))) {  // fetch N
      break;
    }
    // ok, we have more to parse in the last result slot, reiterate with it:
    if (uCount == nrResults) {
      valueToSplit = uResults[nrResults - 1];
    } else {
      // should not go beyound the last match!
      TRI_ASSERT(false);
      break;
    }
  }

  result->close();
  return AqlValue(result->slice(), result->size());
}

/// @brief function REGEX_MATCHES
AqlValue Functions::RegexMatches(ExpressionContext* expressionContext,
                                 AstNode const&,
                                 VPackFunctionParametersView parameters) {
  static char const* AFN = "REGEX_MATCHES";

  transaction::Methods* trx = &expressionContext->trx();
  auto const& vopts = trx->vpackOptions();
  AqlValue const& aqlValueToMatch =
      extractFunctionParameterValue(parameters, 0);

  if (parameters.size() == 1) {
    transaction::BuilderLeaser builder(trx);
    builder->openArray();
    builder->add(aqlValueToMatch.slice());
    builder->close();
    return AqlValue(builder->slice(), builder->size());
  }

  bool const caseInsensitive = ::getBooleanParameter(parameters, 2, false);

  // build pattern from parameter #1
  transaction::StringLeaser buffer(trx);
  velocypack::StringSink adapter(buffer.get());

  AqlValue const& regex = extractFunctionParameterValue(parameters, 1);
  ::appendAsString(vopts, adapter, regex);
  bool isEmptyExpression = (buffer->length() == 0);

  // the matcher is owned by the context!
  icu::RegexMatcher* matcher = expressionContext->buildRegexMatcher(
      buffer->data(), buffer->length(), caseInsensitive);

  if (matcher == nullptr) {
    registerWarning(expressionContext, AFN, TRI_ERROR_QUERY_INVALID_REGEX);
    return AqlValue(AqlValueHintNull());
  }

  buffer->clear();
  AqlValue const& value = extractFunctionParameterValue(parameters, 0);
  ::appendAsString(vopts, adapter, value);
  icu::UnicodeString valueToMatch(buffer->data(),
                                  static_cast<uint32_t>(buffer->length()));

  transaction::BuilderLeaser result(trx);
  result->openArray();

  if (!isEmptyExpression && (buffer->length() == 0)) {
    // Edge case: splitting an empty string by non-empty expression produces an
    // empty string again.
    result->add(VPackValue(""));
    result->close();
    return AqlValue(result->slice(), result->size());
  }

  UErrorCode status = U_ZERO_ERROR;

  matcher->reset(valueToMatch);
  bool find = matcher->find();
  if (!find) {
    return AqlValue(AqlValueHintNull());
  }

  for (int i = 0; i <= matcher->groupCount(); i++) {
    icu::UnicodeString match = matcher->group(i, status);
    if (U_FAILURE(status)) {
      ::registerICUWarning(expressionContext, AFN, status);
      return AqlValue(AqlValueHintNull());
    } else {
      std::string s;
      match.toUTF8String(s);
      result->add(VPackValue(s));
    }
  }

  result->close();
  return AqlValue(result->slice(), result->size());
}

/// @brief function REGEX_SPLIT
AqlValue Functions::RegexSplit(ExpressionContext* expressionContext,
                               AstNode const&,
                               VPackFunctionParametersView parameters) {
  static char const* AFN = "REGEX_SPLIT";

  transaction::Methods* trx = &expressionContext->trx();
  auto const& vopts = trx->vpackOptions();
  int64_t limitNumber = -1;
  if (parameters.size() == 4) {
    AqlValue const& aqlLimit = extractFunctionParameterValue(parameters, 3);
    if (aqlLimit.isNumber()) {
      limitNumber = aqlLimit.toInt64();
    } else {
      registerInvalidArgumentWarning(expressionContext, AFN);
      return AqlValue(AqlValueHintNull());
    }

    if (limitNumber < 0) {
      return AqlValue(AqlValueHintNull());
    }
    if (limitNumber == 0) {
      return AqlValue(AqlValueHintEmptyArray());
    }
  }

  AqlValue const& aqlValueToSplit =
      extractFunctionParameterValue(parameters, 0);

  if (parameters.size() == 1) {
    // pre-documented edge-case: if we only have the first parameter, return it.
    transaction::BuilderLeaser builder(trx);
    builder->openArray();
    builder->add(aqlValueToSplit.slice());
    builder->close();
    return AqlValue(builder->slice(), builder->size());
  }

  bool const caseInsensitive = ::getBooleanParameter(parameters, 2, false);

  // build pattern from parameter #1
  transaction::StringLeaser buffer(trx);
  velocypack::StringSink adapter(buffer.get());

  AqlValue const& regex = extractFunctionParameterValue(parameters, 1);
  ::appendAsString(vopts, adapter, regex);
  bool isEmptyExpression = (buffer->length() == 0);

  // the matcher is owned by the context!
  icu::RegexMatcher* matcher = expressionContext->buildRegexMatcher(
      buffer->data(), buffer->length(), caseInsensitive);

  if (matcher == nullptr) {
    registerWarning(expressionContext, AFN, TRI_ERROR_QUERY_INVALID_REGEX);
    return AqlValue(AqlValueHintNull());
  }

  buffer->clear();
  AqlValue const& value = extractFunctionParameterValue(parameters, 0);
  ::appendAsString(vopts, adapter, value);
  icu::UnicodeString valueToSplit(buffer->data(),
                                  static_cast<int32_t>(buffer->length()));

  transaction::BuilderLeaser result(trx);
  result->openArray();
  if (!isEmptyExpression && (buffer->length() == 0)) {
    // Edge case: splitting an empty string by non-empty expression produces an
    // empty string again.
    result->add(VPackValue(""));
    result->close();
    return AqlValue(result->slice(), result->size());
  }

  std::string utf8;
  static const uint16_t nrResults = 16;
  icu::UnicodeString uResults[nrResults];
  int64_t totalCount = 0;
  while (true) {
    UErrorCode errorCode = U_ZERO_ERROR;
    auto uCount = matcher->split(valueToSplit, uResults, nrResults, errorCode);
    uint16_t copyThisTime = uCount;

    if (U_FAILURE(errorCode)) {
      registerWarning(expressionContext, AFN, TRI_ERROR_QUERY_INVALID_REGEX);
      return AqlValue(AqlValueHintNull());
    }

    if ((copyThisTime > 0) && (copyThisTime > nrResults)) {
      // last hit is the remaining string to be fed into split in a subsequent
      // invocation
      copyThisTime--;
    }

    if ((copyThisTime > 0) &&
        ((copyThisTime == nrResults) || isEmptyExpression)) {
      // ICU will give us a traling empty string we don't care for if we split
      // with empty strings.
      copyThisTime--;
    }

    int64_t i = 0;
    while (i < copyThisTime && (limitNumber < 0 || totalCount < limitNumber)) {
      if ((i == 0) && isEmptyExpression) {
        // ICU will give us an empty string that we don't care for
        // as first value of one match-chunk
        i++;
        continue;
      }
      uResults[i].toUTF8String(utf8);
      result->add(VPackValue(utf8));
      utf8.clear();
      i++;
      totalCount++;
    }

    if (uCount != nrResults ||  // fetch any / found less then N
        (limitNumber >= 0 && totalCount >= limitNumber)) {  // fetch N
      break;
    }
    // ok, we have more to parse in the last result slot, reiterate with it:
    if (uCount == nrResults) {
      valueToSplit = uResults[nrResults - 1];
    } else {
      // should not go beyound the last match!
      TRI_ASSERT(false);
      break;
    }
  }

  result->close();
  return AqlValue(result->slice(), result->size());
}

/// @brief function REGEX_TEST
AqlValue Functions::RegexTest(ExpressionContext* expressionContext,
                              AstNode const&,
                              VPackFunctionParametersView parameters) {
  static char const* AFN = "REGEX_TEST";

  transaction::Methods* trx = &expressionContext->trx();
  auto const& vopts = trx->vpackOptions();
  bool const caseInsensitive = ::getBooleanParameter(parameters, 2, false);
  transaction::StringLeaser buffer(trx);
  velocypack::StringSink adapter(buffer.get());

  // build pattern from parameter #1
  AqlValue const& regex = extractFunctionParameterValue(parameters, 1);
  ::appendAsString(vopts, adapter, regex);

  // the matcher is owned by the context!
  icu::RegexMatcher* matcher = expressionContext->buildRegexMatcher(
      buffer->data(), buffer->length(), caseInsensitive);

  if (matcher == nullptr) {
    // compiling regular expression failed
    registerWarning(expressionContext, AFN, TRI_ERROR_QUERY_INVALID_REGEX);
    return AqlValue(AqlValueHintNull());
  }

  // extract value
  buffer->clear();
  AqlValue const& value = extractFunctionParameterValue(parameters, 0);
  ::appendAsString(vopts, adapter, value);

  bool error = false;
  bool const result = basics::Utf8Helper::DefaultUtf8Helper.matches(
      matcher, buffer->data(), buffer->length(), true, error);

  if (error) {
    // compiling regular expression failed
    registerWarning(expressionContext, AFN, TRI_ERROR_QUERY_INVALID_REGEX);
    return AqlValue(AqlValueHintNull());
  }

  return AqlValue(AqlValueHintBool(result));
}

/// @brief function REGEX_REPLACE
AqlValue Functions::RegexReplace(ExpressionContext* expressionContext,
                                 AstNode const&,
                                 VPackFunctionParametersView parameters) {
  static char const* AFN = "REGEX_REPLACE";

  transaction::Methods* trx = &expressionContext->trx();
  auto const& vopts = trx->vpackOptions();
  bool const caseInsensitive = ::getBooleanParameter(parameters, 3, false);
  transaction::StringLeaser buffer(trx);
  velocypack::StringSink adapter(buffer.get());

  // build pattern from parameter #1
  AqlValue const& regex = extractFunctionParameterValue(parameters, 1);
  ::appendAsString(vopts, adapter, regex);

  // the matcher is owned by the context!
  icu::RegexMatcher* matcher = expressionContext->buildRegexMatcher(
      buffer->data(), buffer->length(), caseInsensitive);

  if (matcher == nullptr) {
    // compiling regular expression failed
    registerWarning(expressionContext, AFN, TRI_ERROR_QUERY_INVALID_REGEX);
    return AqlValue(AqlValueHintNull());
  }

  // extract value
  buffer->clear();
  AqlValue const& value = extractFunctionParameterValue(parameters, 0);
  ::appendAsString(vopts, adapter, value);

  size_t const split = buffer->length();
  AqlValue const& replace = extractFunctionParameterValue(parameters, 2);
  ::appendAsString(vopts, adapter, replace);

  bool error = false;
  std::string result = basics::Utf8Helper::DefaultUtf8Helper.replace(
      matcher, buffer->data(), split, buffer->data() + split,
      buffer->length() - split, false, error);

  if (error) {
    // compiling regular expression failed
    registerWarning(expressionContext, AFN, TRI_ERROR_QUERY_INVALID_REGEX);
    return AqlValue(AqlValueHintNull());
  }

  return AqlValue(result);
}

/// @brief function DATE_NOW
AqlValue Functions::DateNow(ExpressionContext*, AstNode const&,
                            VPackFunctionParametersView) {
  auto millis = std::chrono::duration_cast<duration<int64_t, std::milli>>(
      system_clock::now().time_since_epoch());
  uint64_t dur = millis.count();
  return AqlValue(AqlValueHintUInt(dur));
}

/// @brief function DATE_ISO8601
AqlValue Functions::DateIso8601(ExpressionContext* expressionContext,
                                AstNode const&,
                                VPackFunctionParametersView parameters) {
  static char const* AFN = "DATE_ISO8601";
  return ::dateFromParameters(expressionContext, parameters, AFN, false);
}

/// @brief function DATE_TIMESTAMP
AqlValue Functions::DateTimestamp(ExpressionContext* expressionContext,
                                  AstNode const&,
                                  VPackFunctionParametersView parameters) {
  static char const* AFN = "DATE_TIMESTAMP";
  return ::dateFromParameters(expressionContext, parameters, AFN, true);
}

/// @brief function IS_DATESTRING
AqlValue Functions::IsDatestring(ExpressionContext*, AstNode const&,
                                 VPackFunctionParametersView parameters) {
  AqlValue const& value = extractFunctionParameterValue(parameters, 0);

  bool isValid = false;

  if (value.isString()) {
    tp_sys_clock_ms tp;  // unused
    isValid = basics::parseDateTime(value.slice().stringView(), tp);
  }

  return AqlValue(AqlValueHintBool(isValid));
}

/// @brief function DATE_DAYOFWEEK
AqlValue Functions::DateDayOfWeek(ExpressionContext* expressionContext,
                                  AstNode const&,
                                  VPackFunctionParametersView parameters) {
  static char const* AFN = "DATE_DAYOFWEEK";
  tp_sys_clock_ms tp;
  if (!::parameterToTimePoint(expressionContext, parameters, tp, AFN, 0)) {
    return AqlValue(AqlValueHintNull());
  }
  date::weekday wd{floor<date::days>(tp)};

  return AqlValue(AqlValueHintUInt(wd.c_encoding()));
}

/// @brief function DATE_YEAR
AqlValue Functions::DateYear(ExpressionContext* expressionContext,
                             AstNode const&,
                             VPackFunctionParametersView parameters) {
  static char const* AFN = "DATE_YEAR";
  tp_sys_clock_ms tp;

  if (!::parameterToTimePoint(expressionContext, parameters, tp, AFN, 0)) {
    return AqlValue(AqlValueHintNull());
  }
  auto ymd = date::year_month_day(floor<date::days>(tp));
  // Not the library has operator (int) implemented...
  int64_t year = static_cast<int64_t>((int)ymd.year());
  return AqlValue(AqlValueHintInt(year));
}

/// @brief function DATE_MONTH
AqlValue Functions::DateMonth(ExpressionContext* expressionContext,
                              AstNode const&,
                              VPackFunctionParametersView parameters) {
  static char const* AFN = "DATE_MONTH";
  tp_sys_clock_ms tp;

  if (!::parameterToTimePoint(expressionContext, parameters, tp, AFN, 0)) {
    return AqlValue(AqlValueHintNull());
  }
  auto ymd = date::year_month_day(floor<date::days>(tp));
  // The library has operator (unsigned) implemented
  uint64_t month = static_cast<uint64_t>((unsigned)ymd.month());
  return AqlValue(AqlValueHintUInt(month));
}

/// @brief function DATE_DAY
AqlValue Functions::DateDay(ExpressionContext* expressionContext,
                            AstNode const&,
                            VPackFunctionParametersView parameters) {
  static char const* AFN = "DATE_DAY";
  tp_sys_clock_ms tp;

  if (!::parameterToTimePoint(expressionContext, parameters, tp, AFN, 0)) {
    return AqlValue(AqlValueHintNull());
  }

  auto ymd = date::year_month_day(floor<date::days>(tp));
  // The library has operator (unsigned) implemented
  uint64_t day = static_cast<uint64_t>((unsigned)ymd.day());
  return AqlValue(AqlValueHintUInt(day));
}

/// @brief function DATE_HOUR
AqlValue Functions::DateHour(ExpressionContext* expressionContext,
                             AstNode const&,
                             VPackFunctionParametersView parameters) {
  static char const* AFN = "DATE_HOUR";
  tp_sys_clock_ms tp;

  if (!::parameterToTimePoint(expressionContext, parameters, tp, AFN, 0)) {
    return AqlValue(AqlValueHintNull());
  }

  auto day_time = date::make_time(tp - floor<date::days>(tp));
  uint64_t hours = day_time.hours().count();
  return AqlValue(AqlValueHintUInt(hours));
}

/// @brief function DATE_MINUTE
AqlValue Functions::DateMinute(ExpressionContext* expressionContext,
                               AstNode const&,
                               VPackFunctionParametersView parameters) {
  static char const* AFN = "DATE_MINUTE";
  tp_sys_clock_ms tp;

  if (!::parameterToTimePoint(expressionContext, parameters, tp, AFN, 0)) {
    return AqlValue(AqlValueHintNull());
  }

  auto day_time = date::make_time(tp - floor<date::days>(tp));
  uint64_t minutes = day_time.minutes().count();
  return AqlValue(AqlValueHintUInt(minutes));
}

/// @brief function DATE_SECOND
AqlValue Functions::DateSecond(ExpressionContext* expressionContext,
                               AstNode const&,
                               VPackFunctionParametersView parameters) {
  static char const* AFN = "DATE_SECOND";
  tp_sys_clock_ms tp;

  if (!::parameterToTimePoint(expressionContext, parameters, tp, AFN, 0)) {
    return AqlValue(AqlValueHintNull());
  }

  auto day_time = date::make_time(tp - floor<date::days>(tp));
  uint64_t seconds = day_time.seconds().count();
  return AqlValue(AqlValueHintUInt(seconds));
}

/// @brief function DATE_MILLISECOND
AqlValue Functions::DateMillisecond(ExpressionContext* expressionContext,
                                    AstNode const&,
                                    VPackFunctionParametersView parameters) {
  static char const* AFN = "DATE_MILLISECOND";
  tp_sys_clock_ms tp;

  if (!::parameterToTimePoint(expressionContext, parameters, tp, AFN, 0)) {
    return AqlValue(AqlValueHintNull());
  }
  auto day_time = date::make_time(tp - floor<date::days>(tp));
  uint64_t millis = day_time.subseconds().count();
  return AqlValue(AqlValueHintUInt(millis));
}

/// @brief function DATE_DAYOFYEAR
AqlValue Functions::DateDayOfYear(ExpressionContext* expressionContext,
                                  AstNode const&,
                                  VPackFunctionParametersView parameters) {
  static char const* AFN = "DATE_DAYOFYEAR";
  tp_sys_clock_ms tp;

  if (!::parameterToTimePoint(expressionContext, parameters, tp, AFN, 0)) {
    return AqlValue(AqlValueHintNull());
  }

  auto ymd = date::year_month_day(floor<date::days>(tp));
  auto yyyy = date::year{ymd.year()};
  // we construct the date with the first day in the year:
  auto firstDayInYear = yyyy / date::jan / date::day{0};
  uint64_t daysSinceFirst =
      duration_cast<date::days>(tp - date::sys_days(firstDayInYear)).count();

  return AqlValue(AqlValueHintUInt(daysSinceFirst));
}

/// @brief function DATE_ISOWEEK
AqlValue Functions::DateIsoWeek(ExpressionContext* expressionContext,
                                AstNode const&,
                                VPackFunctionParametersView parameters) {
  static char const* AFN = "DATE_ISOWEEK";
  tp_sys_clock_ms tp;

  if (!::parameterToTimePoint(expressionContext, parameters, tp, AFN, 0)) {
    return AqlValue(AqlValueHintNull());
  }

  iso_week::year_weeknum_weekday yww{floor<date::days>(tp)};
  // The (unsigned) operator is overloaded...
  uint64_t isoWeek = static_cast<uint64_t>((unsigned)(yww.weeknum()));
  return AqlValue(AqlValueHintUInt(isoWeek));
}

/// @brief function DATE_LEAPYEAR
AqlValue Functions::DateLeapYear(ExpressionContext* expressionContext,
                                 AstNode const&,
                                 VPackFunctionParametersView parameters) {
  static char const* AFN = "DATE_LEAPYEAR";
  tp_sys_clock_ms tp;

  if (!::parameterToTimePoint(expressionContext, parameters, tp, AFN, 0)) {
    return AqlValue(AqlValueHintNull());
  }

  date::year_month_day ymd{floor<date::days>(tp)};

  return AqlValue(AqlValueHintBool(ymd.year().is_leap()));
}

/// @brief function DATE_QUARTER
AqlValue Functions::DateQuarter(ExpressionContext* expressionContext,
                                AstNode const&,
                                VPackFunctionParametersView parameters) {
  static char const* AFN = "DATE_QUARTER";
  tp_sys_clock_ms tp;

  if (!::parameterToTimePoint(expressionContext, parameters, tp, AFN, 0)) {
    return AqlValue(AqlValueHintNull());
  }

  date::year_month_day ymd{floor<date::days>(tp)};
  date::month m = ymd.month();

  // Library has unsigned operator implemented.
  uint64_t part = static_cast<uint64_t>(ceil(unsigned(m) / 3.0f));
  // We only have 4 quarters ;)
  TRI_ASSERT(part <= 4);
  return AqlValue(AqlValueHintUInt(part));
}

/// @brief function DATE_DAYS_IN_MONTH
AqlValue Functions::DateDaysInMonth(ExpressionContext* expressionContext,
                                    AstNode const&,
                                    VPackFunctionParametersView parameters) {
  static char const* AFN = "DATE_DAYS_IN_MONTH";
  tp_sys_clock_ms tp;

  if (!::parameterToTimePoint(expressionContext, parameters, tp, AFN, 0)) {
    return AqlValue(AqlValueHintNull());
  }

  auto ymd = date::year_month_day{floor<date::days>(tp)};
  auto lastMonthDay = ymd.year() / ymd.month() / date::last;

  // The Library has operator unsigned implemented
  return AqlValue(
      AqlValueHintUInt(static_cast<uint64_t>(unsigned(lastMonthDay.day()))));
}

/// @brief function DATE_TRUNC
AqlValue Functions::DateTrunc(ExpressionContext* expressionContext,
                              AstNode const&,
                              VPackFunctionParametersView parameters) {
  static char const* AFN = "DATE_TRUNC";

  tp_sys_clock_ms tp;

  if (!::parameterToTimePoint(expressionContext, parameters, tp, AFN, 0)) {
    return AqlValue(AqlValueHintNull());
  }

  AqlValue const& durationType = extractFunctionParameterValue(parameters, 1);

  if (!durationType.isString()) {  // unit type must be string
    registerInvalidArgumentWarning(expressionContext, AFN);
    return AqlValue(AqlValueHintNull());
  }

  std::string duration = durationType.slice().copyString();
  basics::StringUtils::tolowerInPlace(duration);

  date::year_month_day ymd{floor<date::days>(tp)};
  auto day_time = date::make_time(tp - date::sys_days(ymd));
  milliseconds ms{0};
  if (duration == "y" || duration == "year" || duration == "years") {
    ymd = date::year{ymd.year()} / date::jan / date::day{1};
  } else if (duration == "m" || duration == "month" || duration == "months") {
    ymd = date::year{ymd.year()} / ymd.month() / date::day{1};
  } else if (duration == "d" || duration == "day" || duration == "days") {
    // this would be: ymd = year{ymd.year()}/ymd.month()/ymd.day();
    // However, we already split ymd to the precision of days,
    // and ms to cary the timestamp part, so nothing needs to be done here.
  } else if (duration == "h" || duration == "hour" || duration == "hours") {
    ms = day_time.hours();
  } else if (duration == "i" || duration == "minute" || duration == "minutes") {
    ms = day_time.hours() + day_time.minutes();
  } else if (duration == "s" || duration == "second" || duration == "seconds") {
    ms = day_time.to_duration() - day_time.subseconds();
  } else if (duration == "f" || duration == "millisecond" ||
             duration == "milliseconds") {
    ms = day_time.to_duration();
  } else {
    registerWarning(expressionContext, AFN, TRI_ERROR_QUERY_INVALID_DATE_VALUE);
    return AqlValue(AqlValueHintNull());
  }
  tp = tp_sys_clock_ms{date::sys_days(ymd) + ms};

  return ::timeAqlValue(expressionContext, AFN, tp);
}

/// @brief function DATE_UTCTOLOCAL
AqlValue Functions::DateUtcToLocal(ExpressionContext* expressionContext,
                                   AstNode const&,
                                   VPackFunctionParametersView parameters) {
  static char const* AFN = "DATE_UTCTOLOCAL";

  tp_sys_clock_ms tp_utc;

  if (!::parameterToTimePoint(expressionContext, parameters, tp_utc, AFN, 0)) {
    return AqlValue(AqlValueHintNull());
  }

  AqlValue const& timeZoneParam = extractFunctionParameterValue(parameters, 1);

  if (!timeZoneParam.isString()) {  // timezone type must be string
    registerInvalidArgumentWarning(expressionContext, AFN);
    return AqlValue(AqlValueHintNull());
  }

  auto showDetail = false;

  if (parameters.size() == 3) {
    AqlValue const& detailParam = extractFunctionParameterValue(parameters, 2);
    if (!detailParam.isBoolean()) {  // detail type must be bool
      registerInvalidArgumentWarning(expressionContext, AFN);
      return AqlValue(AqlValueHintNull());
    }

    showDetail = detailParam.slice().getBoolean();
  }

  std::string const tz = timeZoneParam.slice().copyString();
  auto const utc = floor<milliseconds>(tp_utc);
  auto const zoned = date::make_zoned(tz, utc);
  auto const info = zoned.get_info();
  auto const tp_local =
      tp_sys_clock_ms{zoned.get_local_time().time_since_epoch()};
  AqlValue aqlLocal =
      ::timeAqlValue(expressionContext, AFN, tp_local,
                     info.offset.count() == 0 && info.save.count() == 0);

  AqlValueGuard aqlLocalGuard(aqlLocal, true);

  if (showDetail) {
    auto const tp_begin = tp_sys_clock_ms{info.begin.time_since_epoch()};
    AqlValue aqlBegin =
        ::timeAqlValue(expressionContext, AFN, tp_begin, true, false);
    AqlValueGuard aqlBeginGuard(aqlBegin, true);

    auto const tp_end = tp_sys_clock_ms{info.end.time_since_epoch()};
    AqlValue aqlEnd =
        ::timeAqlValue(expressionContext, AFN, tp_end, true, false);
    AqlValueGuard aqlEndGuard(aqlEnd, true);

    transaction::Methods* trx = &expressionContext->trx();
    transaction::BuilderLeaser builder(trx);
    builder->openObject();
    builder->add("local", aqlLocal.slice());
    builder->add("tzdb", VPackValue(date::get_tzdb().version));
    builder->add("zoneInfo", VPackValue(VPackValueType::Object));
    builder->add("name", VPackValue(info.abbrev));
    builder->add("begin", aqlBegin.slice());
    builder->add("end", aqlEnd.slice());
    builder->add("dst", VPackValue(info.save.count() != 0));
    builder->add("offset", VPackValue(info.offset.count()));
    builder->close();
    builder->close();

    return AqlValue(builder->slice(), builder->size());
  }

  aqlLocalGuard.steal();
  return aqlLocal;
}

/// @brief function DATE_LOCALTOUTC
AqlValue Functions::DateLocalToUtc(ExpressionContext* expressionContext,
                                   AstNode const&,
                                   VPackFunctionParametersView parameters) {
  static char const* AFN = "DATE_LOCALTOUTC";

  tp_sys_clock_ms tp_local;

  if (!::parameterToTimePoint(expressionContext, parameters, tp_local, AFN,
                              0)) {
    return AqlValue(AqlValueHintNull());
  }

  AqlValue const& timeZoneParam = extractFunctionParameterValue(parameters, 1);

  if (!timeZoneParam.isString()) {  // timezone type must be string
    registerInvalidArgumentWarning(expressionContext, AFN);
    return AqlValue(AqlValueHintNull());
  }

  auto showDetail = false;

  if (parameters.size() == 3) {
    AqlValue const& detailParam = extractFunctionParameterValue(parameters, 2);
    if (!detailParam.isBoolean()) {  // detail type must be bool
      registerInvalidArgumentWarning(expressionContext, AFN);
      return AqlValue(AqlValueHintNull());
    }

    showDetail = detailParam.slice().getBoolean();
  }

  std::string const tz = timeZoneParam.slice().copyString();
  auto const local = date::local_time<milliseconds>{
      floor<milliseconds>(tp_local).time_since_epoch()};
  auto const zoned = date::make_zoned(tz, local);
  auto const info = zoned.get_info();
  auto const tp_utc = tp_sys_clock_ms{zoned.get_sys_time().time_since_epoch()};
  AqlValue aqlUtc = ::timeAqlValue(expressionContext, AFN, tp_utc);

  AqlValueGuard aqlUtcGuard(aqlUtc, true);

  if (showDetail) {
    auto const tp_begin = tp_sys_clock_ms{info.begin.time_since_epoch()};
    AqlValue aqlBegin =
        ::timeAqlValue(expressionContext, AFN, tp_begin, true, false);
    AqlValueGuard aqlBeginGuard(aqlBegin, true);

    auto const tp_end = tp_sys_clock_ms{info.end.time_since_epoch()};
    AqlValue aqlEnd =
        ::timeAqlValue(expressionContext, AFN, tp_end, true, false);
    AqlValueGuard aqlEndGuard(aqlEnd, true);

    transaction::Methods* trx = &expressionContext->trx();
    transaction::BuilderLeaser builder(trx);
    builder->openObject();
    builder->add("utc", aqlUtc.slice());
    builder->add("tzdb", VPackValue(date::get_tzdb().version));
    builder->add("zoneInfo", VPackValue(VPackValueType::Object));
    builder->add("name", VPackValue(info.abbrev));
    builder->add("begin", aqlBegin.slice());
    builder->add("end", aqlEnd.slice());
    builder->add("dst", VPackValue(info.save.count() != 0));
    builder->add("offset", VPackValue(info.offset.count()));
    builder->close();
    builder->close();

    return AqlValue(builder->slice(), builder->size());
  }

  aqlUtcGuard.steal();
  return aqlUtc;
}

/// @brief function DATE_TIMEZONE
AqlValue Functions::DateTimeZone(ExpressionContext* expressionContext,
                                 AstNode const&,
                                 VPackFunctionParametersView parameters) {
  auto const* zone = date::current_zone();

  if (zone != nullptr) {
    return AqlValue(zone->name());
  }

  return AqlValue(AqlValueHintNull());
}

/// @brief function DATE_TIMEZONES
AqlValue Functions::DateTimeZones(ExpressionContext* expressionContext,
                                  AstNode const&,
                                  VPackFunctionParametersView parameters) {
  auto& list = date::get_tzdb_list();
  auto& db = list.front();

  transaction::Methods* trx = &expressionContext->trx();
  transaction::BuilderLeaser result(trx);
  result->openArray();

  for (auto& zone : db.zones) {
    result->add(VPackValue(zone.name()));
  }

  for (auto& link : db.links) {
    result->add(VPackValue(link.name()));
  }

  result->close();
  return AqlValue(result->slice(), result->size());
}

/// @brief function DATE_ADD
AqlValue Functions::DateAdd(ExpressionContext* expressionContext,
                            AstNode const&,
                            VPackFunctionParametersView parameters) {
  static char const* AFN = "DATE_ADD";
  tp_sys_clock_ms tp;

  if (!::parameterToTimePoint(expressionContext, parameters, tp, AFN, 0)) {
    return AqlValue(AqlValueHintNull());
  }

  // size == 3 unit / unit type
  // size == 2 iso duration

  if (parameters.size() == 3) {
    AqlValue const& durationUnit = extractFunctionParameterValue(parameters, 1);
    if (!durationUnit.isNumber()) {  // unit must be number
      registerInvalidArgumentWarning(expressionContext, AFN);
      return AqlValue(AqlValueHintNull());
    }

    AqlValue const& durationType = extractFunctionParameterValue(parameters, 2);
    if (!durationType.isString()) {  // unit type must be string
      registerInvalidArgumentWarning(expressionContext, AFN);
      return AqlValue(AqlValueHintNull());
    }

    // Numbers and Strings can both be sliced
    return ::addOrSubtractUnitFromTimestamp(expressionContext, tp,
                                            durationUnit.slice(),
                                            durationType.slice(), AFN, false);
  } else {  // iso duration
    AqlValue const& isoDuration = extractFunctionParameterValue(parameters, 1);
    if (!isoDuration.isString()) {
      registerInvalidArgumentWarning(expressionContext, AFN);
      return AqlValue(AqlValueHintNull());
    }

    return ::addOrSubtractIsoDurationFromTimestamp(
        expressionContext, tp, isoDuration.slice().stringView(), AFN, false);
  }
}

/// @brief function DATE_SUBTRACT
AqlValue Functions::DateSubtract(ExpressionContext* expressionContext,
                                 AstNode const&,
                                 VPackFunctionParametersView parameters) {
  static char const* AFN = "DATE_SUBTRACT";
  tp_sys_clock_ms tp;

  if (!::parameterToTimePoint(expressionContext, parameters, tp, AFN, 0)) {
    return AqlValue(AqlValueHintNull());
  }

  // size == 3 unit / unit type
  // size == 2 iso duration

  if (parameters.size() == 3) {
    AqlValue const& durationUnit = extractFunctionParameterValue(parameters, 1);
    if (!durationUnit.isNumber()) {  // unit must be number
      registerInvalidArgumentWarning(expressionContext, AFN);
      return AqlValue(AqlValueHintNull());
    }

    AqlValue const& durationType = extractFunctionParameterValue(parameters, 2);
    if (!durationType.isString()) {  // unit type must be string
      registerInvalidArgumentWarning(expressionContext, AFN);
      return AqlValue(AqlValueHintNull());
    }

    // Numbers and Strings can both be sliced
    return ::addOrSubtractUnitFromTimestamp(expressionContext, tp,
                                            durationUnit.slice(),
                                            durationType.slice(), AFN, true);
  } else {  // iso duration
    AqlValue const& isoDuration = extractFunctionParameterValue(parameters, 1);
    if (!isoDuration.isString()) {
      registerInvalidArgumentWarning(expressionContext, AFN);
      return AqlValue(AqlValueHintNull());
    }

    return ::addOrSubtractIsoDurationFromTimestamp(
        expressionContext, tp, isoDuration.slice().stringView(), AFN, true);
  }
}

/// @brief function DATE_DIFF
AqlValue Functions::DateDiff(ExpressionContext* expressionContext,
                             AstNode const&,
                             VPackFunctionParametersView parameters) {
  static char const* AFN = "DATE_DIFF";
  // Extract first date
  tp_sys_clock_ms tp1;
  if (!::parameterToTimePoint(expressionContext, parameters, tp1, AFN, 0)) {
    return AqlValue(AqlValueHintNull());
  }

  // Extract second date
  tp_sys_clock_ms tp2;
  if (!::parameterToTimePoint(expressionContext, parameters, tp2, AFN, 1)) {
    return AqlValue(AqlValueHintNull());
  }

  double diff = 0.0;
  bool asFloat = false;
  auto diffDuration = tp2 - tp1;

  AqlValue const& unitValue = extractFunctionParameterValue(parameters, 2);
  if (!unitValue.isString()) {
    registerInvalidArgumentWarning(expressionContext, AFN);
    return AqlValue(AqlValueHintNull());
  }

  DateSelectionModifier flag = ::parseDateModifierFlag(unitValue.slice());

  if (parameters.size() == 4) {
    AqlValue const& asFloatValue = extractFunctionParameterValue(parameters, 3);
    if (!asFloatValue.isBoolean()) {
      registerInvalidArgumentWarning(expressionContext, AFN);
      return AqlValue(AqlValueHintNull());
    }
    asFloat = asFloatValue.toBoolean();
  }

  switch (flag) {
    case YEAR:
      diff = duration_cast<
                 duration<double, std::ratio_multiply<std::ratio<146097, 400>,
                                                      date::days::period>>>(
                 diffDuration)
                 .count();
      break;
    case MONTH:
      diff = duration_cast<duration<
          double, std::ratio_divide<date::years::period, std::ratio<12>>>>(
                 diffDuration)
                 .count();
      break;
    case WEEK:
      diff = duration_cast<duration<
          double, std::ratio_multiply<std::ratio<7>, date::days::period>>>(
                 diffDuration)
                 .count();
      break;
    case DAY:
      diff = duration_cast<duration<
          double,
          std::ratio_multiply<std::ratio<24>, std::chrono::hours::period>>>(
                 diffDuration)
                 .count();
      break;
    case HOUR:
      diff = duration_cast<duration<double, std::ratio<3600>>>(diffDuration)
                 .count();
      break;
    case MINUTE:
      diff =
          duration_cast<duration<double, std::ratio<60>>>(diffDuration).count();
      break;
    case SECOND:
      diff = duration_cast<duration<double>>(diffDuration).count();
      break;
    case MILLI:
      diff = duration_cast<duration<double, std::milli>>(diffDuration).count();
      break;
    case INVALID:
      registerWarning(expressionContext, AFN,
                      TRI_ERROR_QUERY_INVALID_DATE_VALUE);
      return AqlValue(AqlValueHintNull());
  }

  if (asFloat) {
    return AqlValue(AqlValueHintDouble(diff));
  }
  return AqlValue(AqlValueHintInt(static_cast<int64_t>(std::round(diff))));
}

/// @brief function DATE_COMPARE
AqlValue Functions::DateCompare(ExpressionContext* expressionContext,
                                AstNode const&,
                                VPackFunctionParametersView parameters) {
  static char const* AFN = "DATE_COMPARE";
  tp_sys_clock_ms tp1;
  if (!::parameterToTimePoint(expressionContext, parameters, tp1, AFN, 0)) {
    return AqlValue(AqlValueHintNull());
  }

  tp_sys_clock_ms tp2;
  if (!::parameterToTimePoint(expressionContext, parameters, tp2, AFN, 1)) {
    return AqlValue(AqlValueHintNull());
  }

  AqlValue const& rangeStartValue =
      extractFunctionParameterValue(parameters, 2);

  DateSelectionModifier rangeStart =
      ::parseDateModifierFlag(rangeStartValue.slice());

  if (rangeStart == INVALID) {
    registerWarning(expressionContext, AFN,
                    TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH);
    return AqlValue(AqlValueHintNull());
  }

  DateSelectionModifier rangeEnd = rangeStart;
  if (parameters.size() == 4) {
    AqlValue const& rangeEndValue =
        extractFunctionParameterValue(parameters, 3);
    rangeEnd = ::parseDateModifierFlag(rangeEndValue.slice());

    if (rangeEnd == INVALID) {
      registerWarning(expressionContext, AFN,
                      TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH);
      return AqlValue(AqlValueHintNull());
    }
  }
  auto ymd1 = date::year_month_day{floor<date::days>(tp1)};
  auto ymd2 = date::year_month_day{floor<date::days>(tp2)};
  auto time1 = date::make_time(tp1 - floor<date::days>(tp1));
  auto time2 = date::make_time(tp2 - floor<date::days>(tp2));

  // This switch has the following feature:
  // It is ordered by the Highest value of
  // the Modifier (YEAR) and flows down to
  // lower values.
  // In each case if the value is significant
  // (above or equal the endRange) we compare it.
  // If this part is not equal we return false.
  // Otherwise we fall down to the next part.
  // As soon as we are below the endRange
  // we bail out.
  // So all Fall throughs here are intentional
  switch (rangeStart) {
    case YEAR:
      // Always check for the year
      if (ymd1.year() != ymd2.year()) {
        return AqlValue(AqlValueHintBool(false));
      }
      [[fallthrough]];
    case MONTH:
      if (rangeEnd > MONTH) {
        break;
      }
      if (ymd1.month() != ymd2.month()) {
        return AqlValue(AqlValueHintBool(false));
      }
      [[fallthrough]];
    case DAY:
      if (rangeEnd > DAY) {
        break;
      }
      if (ymd1.day() != ymd2.day()) {
        return AqlValue(AqlValueHintBool(false));
      }
      [[fallthrough]];
    case HOUR:
      if (rangeEnd > HOUR) {
        break;
      }
      if (time1.hours() != time2.hours()) {
        return AqlValue(AqlValueHintBool(false));
      }
      [[fallthrough]];
    case MINUTE:
      if (rangeEnd > MINUTE) {
        break;
      }
      if (time1.minutes() != time2.minutes()) {
        return AqlValue(AqlValueHintBool(false));
      }
      [[fallthrough]];
    case SECOND:
      if (rangeEnd > SECOND) {
        break;
      }
      if (time1.seconds() != time2.seconds()) {
        return AqlValue(AqlValueHintBool(false));
      }
      [[fallthrough]];
    case MILLI:
      if (rangeEnd > MILLI) {
        break;
      }
      if (time1.subseconds() != time2.subseconds()) {
        return AqlValue(AqlValueHintBool(false));
      }
      break;
    case INVALID:
    case WEEK:
      // Was handled before
      TRI_ASSERT(false);
  }

  // If we get here all significant places are equal
  // Name these two dates as equal
  return AqlValue(AqlValueHintBool(true));
}

/// @brief function DATE_ROUND
AqlValue Functions::DateRound(ExpressionContext* expressionContext,
                              AstNode const&,
                              VPackFunctionParametersView parameters) {
  static char const* AFN = "DATE_ROUND";
  tp_sys_clock_ms tp;

  if (!::parameterToTimePoint(expressionContext, parameters, tp, AFN, 0)) {
    return AqlValue(AqlValueHintNull());
  }

  AqlValue const& durationUnit = extractFunctionParameterValue(parameters, 1);
  if (!durationUnit.isNumber()) {  // unit must be number
    registerInvalidArgumentWarning(expressionContext, AFN);
    return AqlValue(AqlValueHintNull());
  }

  AqlValue const& durationType = extractFunctionParameterValue(parameters, 2);
  if (!durationType.isString()) {  // unit type must be string
    registerInvalidArgumentWarning(expressionContext, AFN);
    return AqlValue(AqlValueHintNull());
  }

  int64_t const m = durationUnit.toInt64();
  if (m <= 0) {
    registerInvalidArgumentWarning(expressionContext, AFN);
    return AqlValue(AqlValueHintNull());
  }

  std::string_view s = durationType.slice().stringView();

  int64_t factor = 1;
  if (s == "milliseconds" || s == "millisecond" || s == "f") {
    factor = 1;
  } else if (s == "seconds" || s == "second" || s == "s") {
    factor = 1000;
  } else if (s == "minutes" || s == "minute" || s == "i") {
    factor = 60 * 1000;
  } else if (s == "hours" || s == "hour" || s == "h") {
    factor = 60 * 60 * 1000;
  } else if (s == "days" || s == "day" || s == "d") {
    factor = 24 * 60 * 60 * 1000;
  } else {
    registerInvalidArgumentWarning(expressionContext, AFN);
    return AqlValue(AqlValueHintNull());
  }

  int64_t const multiplier = factor * m;

  duration<int64_t, std::milli> time = tp.time_since_epoch();
  int64_t t = time.count();
  // integer division!
  t /= multiplier;
  tp = tp_sys_clock_ms(milliseconds(t * multiplier));
  return ::timeAqlValue(expressionContext, AFN, tp);
}

/// @brief function PASSTHRU
AqlValue Functions::Passthru(ExpressionContext*, AstNode const&,
                             VPackFunctionParametersView parameters) {
  if (parameters.empty()) {
    return AqlValue(AqlValueHintNull());
  }

  return extractFunctionParameterValue(parameters, 0).clone();
}

/// @brief function UNSET
AqlValue Functions::Unset(ExpressionContext* expressionContext, AstNode const&,
                          VPackFunctionParametersView parameters) {
  static char const* AFN = "UNSET";

  AqlValue const& value = extractFunctionParameterValue(parameters, 0);
  transaction::Methods* trx = &expressionContext->trx();
  auto* vopts = &trx->vpackOptions();

  if (!value.isObject()) {
    registerInvalidArgumentWarning(expressionContext, AFN);
    return AqlValue(AqlValueHintNull());
  }

  containers::FlatHashSet<std::string> names;
  ::extractKeys(names, expressionContext, vopts, parameters, 1, AFN);

  AqlValueMaterializer materializer(vopts);
  VPackSlice slice = materializer.slice(value, false);
  transaction::BuilderLeaser builder(trx);
  ::unsetOrKeep(trx, slice, names, true, false, *builder.get());
  return AqlValue(builder->slice(), builder->size());
}

/// @brief function UNSET_RECURSIVE
AqlValue Functions::UnsetRecursive(ExpressionContext* expressionContext,
                                   AstNode const&,
                                   VPackFunctionParametersView parameters) {
  static char const* AFN = "UNSET_RECURSIVE";

  AqlValue const& value = extractFunctionParameterValue(parameters, 0);

  if (!value.isObject()) {
    registerInvalidArgumentWarning(expressionContext, AFN);
    return AqlValue(AqlValueHintNull());
  }

  transaction::Methods* trx = &expressionContext->trx();
  auto* vopts = &trx->vpackOptions();

  containers::FlatHashSet<std::string> names;
  ::extractKeys(names, expressionContext, vopts, parameters, 1, AFN);

  AqlValueMaterializer materializer(vopts);
  VPackSlice slice = materializer.slice(value, false);
  transaction::BuilderLeaser builder(trx);
  ::unsetOrKeep(trx, slice, names, true, true, *builder.get());
  return AqlValue(builder->slice(), builder->size());
}

/// @brief function KEEP
AqlValue Functions::Keep(ExpressionContext* expressionContext, AstNode const&,
                         VPackFunctionParametersView parameters) {
  static char const* AFN = "KEEP";

  AqlValue const& value = extractFunctionParameterValue(parameters, 0);

  if (!value.isObject()) {
    registerInvalidArgumentWarning(expressionContext, AFN);
    return AqlValue(AqlValueHintNull());
  }

  transaction::Methods* trx = &expressionContext->trx();
  auto* vopts = &trx->vpackOptions();

  containers::FlatHashSet<std::string> names;
  ::extractKeys(names, expressionContext, vopts, parameters, 1, AFN);

  AqlValueMaterializer materializer(vopts);
  VPackSlice slice = materializer.slice(value, false);
  transaction::BuilderLeaser builder(trx);
  ::unsetOrKeep(trx, slice, names, false, false, *builder.get());
  return AqlValue(builder->slice(), builder->size());
}

/// @brief function KEEP_RECURSIVE
AqlValue Functions::KeepRecursive(ExpressionContext* expressionContext,
                                  AstNode const&,
                                  VPackFunctionParametersView parameters) {
  static char const* AFN = "KEEP_RECURSIVE";

  AqlValue const& value = extractFunctionParameterValue(parameters, 0);

  if (!value.isObject()) {
    registerInvalidArgumentWarning(expressionContext, AFN);
    return AqlValue(AqlValueHintNull());
  }

  transaction::Methods* trx = &expressionContext->trx();
  auto* vopts = &trx->vpackOptions();

  containers::FlatHashSet<std::string> names;
  ::extractKeys(names, expressionContext, vopts, parameters, 1, AFN);

  AqlValueMaterializer materializer(vopts);
  VPackSlice slice = materializer.slice(value, false);
  transaction::BuilderLeaser builder(trx);
  ::unsetOrKeep(trx, slice, names, false, true, *builder.get());
  return AqlValue(builder->slice(), builder->size());
}

/// @brief function TRANSLATE
AqlValue Functions::Translate(ExpressionContext* expressionContext,
                              AstNode const&,
                              VPackFunctionParametersView parameters) {
  // cppcheck-suppress variableScope
  static char const* AFN = "TRANSLATE";

  transaction::Methods* trx = &expressionContext->trx();
  auto* vopts = &trx->vpackOptions();
  AqlValue const& key = extractFunctionParameterValue(parameters, 0);
  AqlValue const& lookupDocument = extractFunctionParameterValue(parameters, 1);

  if (!lookupDocument.isObject()) {
    registerInvalidArgumentWarning(expressionContext, AFN);
    return AqlValue(AqlValueHintNull());
  }

  AqlValueMaterializer materializer(vopts);
  VPackSlice slice = materializer.slice(lookupDocument, true);
  TRI_ASSERT(slice.isObject());

  VPackSlice result;
  if (key.isString()) {
    result = slice.get(key.slice().copyString());
  } else {
    transaction::StringLeaser buffer(trx);
    velocypack::StringSink adapter(buffer.get());
    Functions::Stringify(vopts, adapter, key.slice());
    result = slice.get(*buffer);
  }

  if (!result.isNone()) {
    return AqlValue(result);
  }

  // attribute not found, now return the default value
  // we must create copy of it however
  AqlValue const& defaultValue = extractFunctionParameterValue(parameters, 2);
  if (defaultValue.isNone()) {
    return key.clone();
  }
  return defaultValue.clone();
}

/// @brief function MERGE
AqlValue Functions::Merge(ExpressionContext* expressionContext, AstNode const&,
                          VPackFunctionParametersView parameters) {
  return ::mergeParameters(expressionContext, parameters, "MERGE", false);
}

/// @brief function MERGE_RECURSIVE
AqlValue Functions::MergeRecursive(ExpressionContext* expressionContext,
                                   AstNode const&,
                                   VPackFunctionParametersView parameters) {
  return ::mergeParameters(expressionContext, parameters, "MERGE_RECURSIVE",
                           true);
}

/// @brief function HAS
AqlValue Functions::Has(ExpressionContext* expressionContext, AstNode const&,
                        VPackFunctionParametersView parameters) {
  size_t const n = parameters.size();
  if (n < 2) {
    // no parameters
    return AqlValue(AqlValueHintBool(false));
  }

  AqlValue const& value = extractFunctionParameterValue(parameters, 0);

  if (!value.isObject()) {
    // not an object
    return AqlValue(AqlValueHintBool(false));
  }

  transaction::Methods* trx = &expressionContext->trx();
  AqlValue const& name = extractFunctionParameterValue(parameters, 1);
  if (!name.isString()) {
    auto const& vopts = trx->vpackOptions();
    transaction::StringLeaser buffer(trx);
    velocypack::StringSink adapter(buffer.get());
    ::appendAsString(vopts, adapter, name);
    return AqlValue(AqlValueHintBool(value.hasKey(*buffer)));
  }
  return AqlValue(AqlValueHintBool(value.hasKey(name.slice().stringView())));
}

/// @brief function ATTRIBUTES
AqlValue Functions::Attributes(ExpressionContext* expressionContext,
                               AstNode const&,
                               VPackFunctionParametersView parameters) {
  size_t const n = parameters.size();

  if (n < 1) {
    // no parameters
    return AqlValue(AqlValueHintNull());
  }

  AqlValue const& value = extractFunctionParameterValue(parameters, 0);
  if (!value.isObject()) {
    // not an object
    registerWarning(expressionContext, "ATTRIBUTES",
                    TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH);
    return AqlValue(AqlValueHintNull());
  }

  bool const removeInternal = ::getBooleanParameter(parameters, 1, false);
  bool const doSort = ::getBooleanParameter(parameters, 2, false);

  TRI_ASSERT(value.isObject());
  if (value.length() == 0) {
    return AqlValue(AqlValueHintEmptyArray());
  }

  transaction::Methods* trx = &expressionContext->trx();
  auto* vopts = &trx->vpackOptions();

  AqlValueMaterializer materializer(vopts);
  VPackSlice slice = materializer.slice(value, false);

  if (doSort) {
    std::set<std::string_view,
             basics::VelocyPackHelper::AttributeSorterUTF8StringView>
        keys;

    VPackCollection::keys(slice, keys);
    transaction::BuilderLeaser builder(trx);
    builder->openArray();
    for (auto const& it : keys) {
      TRI_ASSERT(!it.empty());
      if (removeInternal && !it.empty() && it.front() == '_') {
        continue;
      }
      builder->add(VPackValue(it));
    }
    builder->close();

    return AqlValue(builder->slice(), builder->size());
  }

  std::unordered_set<std::string_view> keys;
  VPackCollection::keys(slice, keys);

  transaction::BuilderLeaser builder(trx);
  builder->openArray();
  for (auto const& it : keys) {
    if (removeInternal && !it.empty() && it.front() == '_') {
      continue;
    }
    builder->add(VPackValue(it));
  }
  builder->close();
  return AqlValue(builder->slice(), builder->size());
}

/// @brief function VALUES
AqlValue Functions::Values(ExpressionContext* expressionContext, AstNode const&,
                           VPackFunctionParametersView parameters) {
  size_t const n = parameters.size();

  if (n < 1) {
    // no parameters
    return AqlValue(AqlValueHintNull());
  }

  AqlValue const& value = extractFunctionParameterValue(parameters, 0);
  if (!value.isObject()) {
    // not an object
    registerWarning(expressionContext, "VALUES",
                    TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH);
    return AqlValue(AqlValueHintNull());
  }

  bool const removeInternal = ::getBooleanParameter(parameters, 1, false);

  TRI_ASSERT(value.isObject());
  if (value.length() == 0) {
    return AqlValue(AqlValueHintEmptyArray());
  }

  transaction::Methods* trx = &expressionContext->trx();
  auto* vopts = &trx->vpackOptions();

  AqlValueMaterializer materializer(vopts);
  VPackSlice slice = materializer.slice(value, false);
  transaction::BuilderLeaser builder(trx);
  builder->openArray();
  for (auto const& entry : VPackObjectIterator(slice, true)) {
    if (!entry.key.isString()) {
      // somehow invalid
      continue;
    }
    if (removeInternal) {
      VPackValueLength l;
      char const* p = entry.key.getStringUnchecked(l);
      if (l > 0 && *p == '_') {
        // skip attribute
        continue;
      }
    }
    if (entry.value.isCustom()) {
      builder->add(VPackValue(trx->extractIdString(slice)));
    } else {
      builder->add(entry.value);
    }
  }
  builder->close();

  return AqlValue(builder->slice(), builder->size());
}

/// @brief function MIN
AqlValue Functions::Min(ExpressionContext* expressionContext, AstNode const&,
                        VPackFunctionParametersView parameters) {
  AqlValue const& value = extractFunctionParameterValue(parameters, 0);

  if (!value.isArray()) {
    // not an array
    registerWarning(expressionContext, "MIN", TRI_ERROR_QUERY_ARRAY_EXPECTED);
    return AqlValue(AqlValueHintNull());
  }

  transaction::Methods* trx = &expressionContext->trx();
  auto* vopts = &trx->vpackOptions();

  AqlValueMaterializer materializer(vopts);
  VPackSlice slice = materializer.slice(value, false);

  VPackSlice minValue;
  auto options = trx->transactionContextPtr()->getVPackOptions();
  for (VPackSlice it : VPackArrayIterator(slice)) {
    if (it.isNull()) {
      continue;
    }
    if (minValue.isNone() ||
        basics::VelocyPackHelper::compare(it, minValue, true, options) < 0) {
      minValue = it;
    }
  }
  if (minValue.isNone()) {
    return AqlValue(AqlValueHintNull());
  }
  return AqlValue(minValue);
}

/// @brief function MAX
AqlValue Functions::Max(ExpressionContext* expressionContext, AstNode const&,
                        VPackFunctionParametersView parameters) {
  AqlValue const& value = extractFunctionParameterValue(parameters, 0);

  if (!value.isArray()) {
    // not an array
    registerWarning(expressionContext, "MAX", TRI_ERROR_QUERY_ARRAY_EXPECTED);
    return AqlValue(AqlValueHintNull());
  }

  transaction::Methods* trx = &expressionContext->trx();
  auto* vopts = &trx->vpackOptions();

  AqlValueMaterializer materializer(vopts);
  VPackSlice slice = materializer.slice(value, false);
  VPackSlice maxValue;
  auto options = trx->transactionContextPtr()->getVPackOptions();
  for (VPackSlice it : VPackArrayIterator(slice)) {
    if (maxValue.isNone() ||
        basics::VelocyPackHelper::compare(it, maxValue, true, options) > 0) {
      maxValue = it;
    }
  }
  if (maxValue.isNone()) {
    return AqlValue(AqlValueHintNull());
  }
  return AqlValue(maxValue);
}

/// @brief function SUM
AqlValue Functions::Sum(ExpressionContext* expressionContext, AstNode const&,
                        VPackFunctionParametersView parameters) {
  AqlValue const& value = extractFunctionParameterValue(parameters, 0);

  if (!value.isArray()) {
    // not an array
    registerWarning(expressionContext, "SUM", TRI_ERROR_QUERY_ARRAY_EXPECTED);
    return AqlValue(AqlValueHintNull());
  }

  transaction::Methods* trx = &expressionContext->trx();
  auto* vopts = &trx->vpackOptions();

  AqlValueMaterializer materializer(vopts);
  VPackSlice slice = materializer.slice(value, false);
  double sum = 0.0;
  for (VPackSlice it : VPackArrayIterator(slice)) {
    if (it.isNull()) {
      continue;
    }
    if (!it.isNumber()) {
      return AqlValue(AqlValueHintNull());
    }
    double const number = it.getNumericValue<double>();

    if (!std::isnan(number) && number != HUGE_VAL && number != -HUGE_VAL) {
      sum += number;
    }
  }

  return ::numberValue(sum, false);
}

/// @brief function AVERAGE
AqlValue Functions::Average(ExpressionContext* expressionContext,
                            AstNode const&,
                            VPackFunctionParametersView parameters) {
  static char const* AFN = "AVERAGE";
  AqlValue const& value = extractFunctionParameterValue(parameters, 0);

  if (!value.isArray()) {
    // not an array
    registerWarning(expressionContext, AFN, TRI_ERROR_QUERY_ARRAY_EXPECTED);
    return AqlValue(AqlValueHintNull());
  }

  transaction::Methods* trx = &expressionContext->trx();
  auto* vopts = &trx->vpackOptions();

  AqlValueMaterializer materializer(vopts);
  VPackSlice slice = materializer.slice(value, false);

  double sum = 0.0;
  size_t count = 0;
  for (VPackSlice v : VPackArrayIterator(slice)) {
    if (v.isNull()) {
      continue;
    }
    if (!v.isNumber()) {
      registerWarning(expressionContext, AFN, TRI_ERROR_QUERY_ARRAY_EXPECTED);
      return AqlValue(AqlValueHintNull());
    }

    // got a numeric value
    double const number = v.getNumericValue<double>();

    if (!std::isnan(number) && number != HUGE_VAL && number != -HUGE_VAL) {
      sum += number;
      ++count;
    }
  }

  if (count > 0 && !std::isnan(sum) && sum != HUGE_VAL && sum != -HUGE_VAL) {
    return ::numberValue(sum / static_cast<size_t>(count), false);
  }

  return AqlValue(AqlValueHintNull());
}

/// @brief function PRODUCT
AqlValue Functions::Product(ExpressionContext* expressionContext,
                            AstNode const&,
                            VPackFunctionParametersView parameters) {
  AqlValue const& value = extractFunctionParameterValue(parameters, 0);

  if (!value.isArray()) {
    // not an array
    registerWarning(expressionContext, "PRODUCT",
                    TRI_ERROR_QUERY_ARRAY_EXPECTED);
    return AqlValue(AqlValueHintNull());
  }

  transaction::Methods* trx = &expressionContext->trx();
  auto* vopts = &trx->vpackOptions();

  AqlValueMaterializer materializer(vopts);
  VPackSlice slice = materializer.slice(value, false);
  double product = 1.0;
  for (VPackSlice it : VPackArrayIterator(slice)) {
    if (it.isNull()) {
      continue;
    }
    if (!it.isNumber()) {
      return AqlValue(AqlValueHintNull());
    }
    double const number = it.getNumericValue<double>();

    if (!std::isnan(number) && number != HUGE_VAL && number != -HUGE_VAL) {
      product *= number;
    }
  }

  return ::numberValue(product, false);
}

/// @brief function SLEEP
AqlValue Functions::Sleep(ExpressionContext* expressionContext, AstNode const&,
                          VPackFunctionParametersView parameters) {
  AqlValue const& value = extractFunctionParameterValue(parameters, 0);

  if (!value.isNumber() || value.toDouble() < 0) {
    registerWarning(expressionContext, "SLEEP",
                    TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH);
    return AqlValue(AqlValueHintNull());
  }

  auto& server = expressionContext->vocbase().server();

  double const sleepValue = value.toDouble();
  auto now = std::chrono::steady_clock::now();
  auto const endTime = now + std::chrono::milliseconds(
                                 static_cast<int64_t>(sleepValue * 1000.0));

  while (now < endTime) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    if (expressionContext->killed()) {
      THROW_ARANGO_EXCEPTION(TRI_ERROR_QUERY_KILLED);
    } else if (server.isStopping()) {
      THROW_ARANGO_EXCEPTION(TRI_ERROR_SHUTTING_DOWN);
    }
    now = std::chrono::steady_clock::now();
  }
  return AqlValue(AqlValueHintNull());
}

/// @brief function COLLECTIONS
AqlValue Functions::Collections(ExpressionContext* exprCtx, AstNode const&,
                                VPackFunctionParametersView parameters) {
  transaction::BuilderLeaser builder(&exprCtx->trx());
  builder->openArray();

  auto& vocbase = exprCtx->vocbase();
  auto colls = methods::Collections::sorted(vocbase);

  size_t const n = colls.size();

  auto const& exec = ExecContext::current();
  for (size_t i = 0; i < n; ++i) {
    auto& coll = colls[i];

    if (!exec.canUseCollection(vocbase.name(), coll->name(), auth::Level::RO)) {
      continue;
    }

    builder->openObject();
    builder->add("_id", VPackValue(std::to_string(coll->id().id())));
    builder->add("name", VPackValue(coll->name()));
    builder->close();
  }

  builder->close();

  return AqlValue(builder->slice(), builder->size());
}

/// @brief function RANDOM_TOKEN
AqlValue Functions::RandomToken(ExpressionContext*, AstNode const&,
                                VPackFunctionParametersView parameters) {
  AqlValue const& value = extractFunctionParameterValue(parameters, 0);

  int64_t const length = value.toInt64();
  if (length < 0 || length > 65536) {
    THROW_ARANGO_EXCEPTION_PARAMS(
        TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH, "RANDOM_TOKEN");
  }

  UniformCharacter generator(
      "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789");
  return AqlValue(generator.random(static_cast<size_t>(length)));
}

/// @brief function IPV4_FROM_NUMBER
AqlValue Functions::IpV4FromNumber(ExpressionContext* expressionContext,
                                   AstNode const&,
                                   VPackFunctionParametersView parameters) {
  static char const* AFN = "IPV4_FROM_NUMBER";

  AqlValue const& value = extractFunctionParameterValue(parameters, 0);

  if (!value.isNumber()) {
    registerWarning(expressionContext, AFN,
                    TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH);
    return AqlValue(AqlValueHintNull());
  }

  int64_t input = value.toInt64();
  if (input < 0 || static_cast<uint64_t>(input) > UINT32_MAX) {
    registerInvalidArgumentWarning(expressionContext, AFN);
    return AqlValue(AqlValueHintNull());
  }

  uint64_t number = static_cast<uint64_t>(input);

  // in theory, we only need a 15 bytes buffer here, as the maximum result
  // string is "255.255.255.255"
  char result[32];

  char* p = &result[0];
  // first part
  uint64_t digit = (number & 0xff000000ULL) >> 24ULL;
  p += basics::StringUtils::itoa(digit, p);
  *p++ = '.';
  // second part
  digit = (number & 0x00ff0000ULL) >> 16ULL;
  p += basics::StringUtils::itoa(digit, p);
  *p++ = '.';
  // third part
  digit = (number & 0x0000ff00ULL) >> 8ULL;
  p += basics::StringUtils::itoa(digit, p);
  *p++ = '.';
  // fourth part
  digit = (number & 0x0000ffULL);
  p += basics::StringUtils::itoa(digit, p);

  return AqlValue(&result[0], p - &result[0]);
}

/// @brief function IPV4_TO_NUMBER
AqlValue Functions::IpV4ToNumber(ExpressionContext* expressionContext,
                                 AstNode const&,
                                 VPackFunctionParametersView parameters) {
  static char const* AFN = "IPV4_TO_NUMBER";

  transaction::Methods* trx = &expressionContext->trx();
  auto* vopts = &trx->vpackOptions();
  AqlValue const& value = extractFunctionParameterValue(parameters, 0);

  if (!value.isString()) {
    registerWarning(expressionContext, AFN,
                    TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH);
    return AqlValue(AqlValueHintNull());
  }

  AqlValueMaterializer materializer(vopts);
  VPackSlice slice = materializer.slice(value, false);

  // parse the input string
  TRI_ASSERT(slice.isString());
  VPackValueLength l;
  char const* p = slice.getString(l);

  if (l >= 7 && l <= 15) {
    // min value is 0.0.0.0 (length = 7)
    // max value is 255.255.255.255 (length = 15)
    char buffer[16];
    memcpy(&buffer[0], p, l);
    // null-terminate the buffer
    buffer[l] = '\0';

    struct in_addr addr;
    memset(&addr, 0, sizeof(struct in_addr));
#if _WIN32
    int result = InetPton(AF_INET, &buffer[0], &addr);
#else
    int result = inet_pton(AF_INET, &buffer[0], &addr);
#endif

#ifdef __APPLE__
    // inet_pton on MacOS accepts leading zeros...
    // inet_pton on Linux and Windows doesn't
    // this is the least intrusive solution, but it is not efficient.
    if (result == 1 &&
        std::regex_match(&buffer[0], buffer + l, ::ipV4LeadingZerosRegex,
                         std::regex_constants::match_any)) {
      result = 0;
    }
#endif

    if (result == 1) {
      return AqlValue(AqlValueHintUInt(
          basics::hostToBig(*reinterpret_cast<uint32_t*>(&addr))));
    }
  }

  registerInvalidArgumentWarning(expressionContext, AFN);
  return AqlValue(AqlValueHintNull());
}

/// @brief function IS_IPV4
AqlValue Functions::IsIpV4(ExpressionContext* expressionContext, AstNode const&,
                           VPackFunctionParametersView parameters) {
  transaction::Methods* trx = &expressionContext->trx();
  auto* vopts = &trx->vpackOptions();
  AqlValue const& value = extractFunctionParameterValue(parameters, 0);

  if (!value.isString()) {
    return AqlValue(AqlValueHintBool(false));
  }

  AqlValueMaterializer materializer(vopts);
  VPackSlice slice = materializer.slice(value, false);

  // parse the input string
  TRI_ASSERT(slice.isString());
  VPackValueLength l;
  char const* p = slice.getString(l);

  if (l >= 7 && l <= 15) {
    // min value is 0.0.0.0 (length = 7)
    // max value is 255.255.255.255 (length = 15)
    char buffer[16];
    memcpy(&buffer[0], p, l);
    // null-terminate the buffer
    buffer[l] = '\0';

    struct in_addr addr;
    memset(&addr, 0, sizeof(struct in_addr));
#if _WIN32
    int result = InetPton(AF_INET, &buffer[0], &addr);
#else
    int result = inet_pton(AF_INET, &buffer[0], &addr);
#endif

    if (result == 1) {
#ifdef __APPLE__
      // inet_pton on MacOS accepts leading zeros...
      // inet_pton on Linux and Windows doesn't
      // this is the least intrusive solution, but it is not efficient.
      if (std::regex_match(&buffer[0], buffer + l, ::ipV4LeadingZerosRegex,
                           std::regex_constants::match_any)) {
        return AqlValue(AqlValueHintBool(false));
      }
#endif
      return AqlValue(AqlValueHintBool(true));
    }
  }

  return AqlValue(AqlValueHintBool(false));
}

/// @brief function MD5
AqlValue Functions::Md5(ExpressionContext* exprCtx, AstNode const&,
                        VPackFunctionParametersView parameters) {
  transaction::Methods* trx = &exprCtx->trx();
  auto const& vopts = trx->vpackOptions();
  AqlValue const& value = extractFunctionParameterValue(parameters, 0);
  transaction::StringLeaser buffer(trx);
  velocypack::StringSink adapter(buffer.get());

  ::appendAsString(vopts, adapter, value);

  // create md5
  char hash[17];
  char* p = &hash[0];
  size_t length;

  rest::SslInterface::sslMD5(buffer->data(), buffer->length(), p, length);

  // as hex
  char hex[33];
  p = &hex[0];

  rest::SslInterface::sslHEX(hash, 16, p, length);

  return AqlValue(&hex[0], 32);
}

/// @brief function SHA1
AqlValue Functions::Sha1(ExpressionContext* exprCtx, AstNode const&,
                         VPackFunctionParametersView parameters) {
  transaction::Methods* trx = &exprCtx->trx();
  auto const& vopts = trx->vpackOptions();
  AqlValue const& value = extractFunctionParameterValue(parameters, 0);
  transaction::StringLeaser buffer(trx);
  velocypack::StringSink adapter(buffer.get());

  ::appendAsString(vopts, adapter, value);

  // create sha1
  char hash[21];
  char* p = &hash[0];
  size_t length;

  rest::SslInterface::sslSHA1(buffer->data(), buffer->length(), p, length);

  // as hex
  char hex[41];
  p = &hex[0];

  rest::SslInterface::sslHEX(hash, 20, p, length);

  return AqlValue(&hex[0], 40);
}

/// @brief function SHA512
AqlValue Functions::Sha512(ExpressionContext* exprCtx, AstNode const&,
                           VPackFunctionParametersView parameters) {
  transaction::Methods* trx = &exprCtx->trx();
  auto const& vopts = trx->vpackOptions();
  AqlValue const& value = extractFunctionParameterValue(parameters, 0);
  transaction::StringLeaser buffer(trx);
  velocypack::StringSink adapter(buffer.get());

  ::appendAsString(vopts, adapter, value);

  // create sha512
  char hash[65];
  char* p = &hash[0];
  size_t length;

  rest::SslInterface::sslSHA512(buffer->data(), buffer->length(), p, length);

  // as hex
  char hex[129];
  p = &hex[0];

  rest::SslInterface::sslHEX(hash, 64, p, length);

  return AqlValue(&hex[0], 128);
}

/// @brief function Crc32
AqlValue Functions::Crc32(ExpressionContext* exprCtx, AstNode const&,
                          VPackFunctionParametersView parameters) {
  transaction::Methods* trx = &exprCtx->trx();
  auto const& vopts = trx->vpackOptions();
  AqlValue const& value = extractFunctionParameterValue(parameters, 0);
  transaction::StringLeaser buffer(trx);
  velocypack::StringSink adapter(buffer.get());

  ::appendAsString(vopts, adapter, value);

  uint32_t crc = TRI_Crc32HashPointer(buffer->data(), buffer->length());
  char out[9];
  size_t length = TRI_StringUInt32HexInPlace(crc, &out[0]);
  return AqlValue(&out[0], length);
}

/// @brief function Fnv64
AqlValue Functions::Fnv64(ExpressionContext* exprCtx, AstNode const&,
                          VPackFunctionParametersView parameters) {
  transaction::Methods* trx = &exprCtx->trx();
  auto const& vopts = trx->vpackOptions();
  AqlValue const& value = extractFunctionParameterValue(parameters, 0);
  transaction::StringLeaser buffer(trx);
  velocypack::StringSink adapter(buffer.get());

  ::appendAsString(vopts, adapter, value);

  uint64_t hashval = TRI_FnvHashPointer(buffer->data(), buffer->length());
  char out[17];
  size_t length = TRI_StringUInt64HexInPlace(hashval, &out[0]);
  return AqlValue(&out[0], length);
}

/// @brief function HASH
AqlValue Functions::Hash(ExpressionContext*, AstNode const&,
                         VPackFunctionParametersView parameters) {
  AqlValue const& value = extractFunctionParameterValue(parameters, 0);

  // throw away the top bytes so the hash value can safely be used
  // without precision loss when storing in JavaScript etc.
  uint64_t hash = value.hash() & 0x0007ffffffffffffULL;

  return AqlValue(AqlValueHintUInt(hash));
}

/// @brief function IS_KEY
AqlValue Functions::IsKey(ExpressionContext*, AstNode const&,
                          VPackFunctionParametersView parameters) {
  AqlValue const& value = extractFunctionParameterValue(parameters, 0);
  if (!value.isString()) {
    // not a string, so no valid key
    return AqlValue(AqlValueHintBool(false));
  }

  VPackValueLength l;
  char const* p = value.slice().getStringUnchecked(l);
  return AqlValue(AqlValueHintBool(KeyGeneratorHelper::validateKey(p, l)));
}

/// @brief function COUNT_DISTINCT
AqlValue Functions::CountDistinct(ExpressionContext* expressionContext,
                                  AstNode const&,
                                  VPackFunctionParametersView parameters) {
  // cppcheck-suppress variableScope
  static char const* AFN = "COUNT_DISTINCT";

  transaction::Methods* trx = &expressionContext->trx();
  auto* vopts = &trx->vpackOptions();
  AqlValue const& value = extractFunctionParameterValue(parameters, 0);

  if (!value.isArray()) {
    // not an array
    registerWarning(expressionContext, AFN, TRI_ERROR_QUERY_ARRAY_EXPECTED);
    return AqlValue(AqlValueHintNull());
  }

  AqlValueMaterializer materializer(vopts);
  VPackSlice slice = materializer.slice(value, false);

  auto options = trx->transactionContextPtr()->getVPackOptions();
  containers::FlatHashSet<VPackSlice, basics::VelocyPackHelper::VPackHash,
                          basics::VelocyPackHelper::VPackEqual>
      values(512, basics::VelocyPackHelper::VPackHash(),
             basics::VelocyPackHelper::VPackEqual(options));

  for (VPackSlice s : VPackArrayIterator(slice)) {
    if (!s.isNone()) {
      values.emplace(s.resolveExternal());
    }
  }

  return AqlValue(AqlValueHintUInt(values.size()));
}

/// @brief function UNIQUE
AqlValue Functions::Unique(ExpressionContext* expressionContext, AstNode const&,
                           VPackFunctionParametersView parameters) {
  // cppcheck-suppress variableScope
  static char const* AFN = "UNIQUE";

  transaction::Methods* trx = &expressionContext->trx();
  auto* vopts = &trx->vpackOptions();
  AqlValue const& value = extractFunctionParameterValue(parameters, 0);

  if (!value.isArray()) {
    // not an array
    registerWarning(expressionContext, AFN, TRI_ERROR_QUERY_ARRAY_EXPECTED);
    return AqlValue(AqlValueHintNull());
  }

  AqlValueMaterializer materializer(vopts);
  VPackSlice slice = materializer.slice(value, false);

  auto options = trx->transactionContextPtr()->getVPackOptions();
  containers::FlatHashSet<VPackSlice, basics::VelocyPackHelper::VPackHash,
                          basics::VelocyPackHelper::VPackEqual>
      values(512, basics::VelocyPackHelper::VPackHash(),
             basics::VelocyPackHelper::VPackEqual(options));

  transaction::BuilderLeaser builder(trx);
  builder->openArray();

  for (VPackSlice s : VPackArrayIterator(slice)) {
    if (s.isNone()) {
      continue;
    }

    s = s.resolveExternal();

    if (values.emplace(s).second) {
      builder->add(s);
    }
  }

  builder->close();
  return AqlValue(builder->slice(), builder->size());
}

/// @brief function SORTED_UNIQUE
AqlValue Functions::SortedUnique(ExpressionContext* expressionContext,
                                 AstNode const&,
                                 VPackFunctionParametersView parameters) {
  // cppcheck-suppress variableScope
  static char const* AFN = "SORTED_UNIQUE";

  transaction::Methods* trx = &expressionContext->trx();
  auto* vopts = &trx->vpackOptions();
  AqlValue const& value = extractFunctionParameterValue(parameters, 0);

  if (!value.isArray()) {
    // not an array
    registerWarning(expressionContext, AFN, TRI_ERROR_QUERY_ARRAY_EXPECTED);
    return AqlValue(AqlValueHintNull());
  }

  AqlValueMaterializer materializer(vopts);
  VPackSlice slice = materializer.slice(value, false);

  basics::VelocyPackHelper::VPackLess<true> less(
      trx->transactionContext()->getVPackOptions(), &slice, &slice);
  std::set<VPackSlice, basics::VelocyPackHelper::VPackLess<true>> values(less);
  for (VPackSlice it : VPackArrayIterator(slice)) {
    if (!it.isNone()) {
      values.insert(it);
    }
  }

  transaction::BuilderLeaser builder(trx);
  builder->openArray();
  for (auto const& it : values) {
    builder->add(it);
  }
  builder->close();
  return AqlValue(builder->slice(), builder->size());
}

/// @brief function SORTED
AqlValue Functions::Sorted(ExpressionContext* expressionContext, AstNode const&,
                           VPackFunctionParametersView parameters) {
  // cppcheck-suppress variableScope
  static char const* AFN = "SORTED";

  transaction::Methods* trx = &expressionContext->trx();
  auto* vopts = &trx->vpackOptions();
  AqlValue const& value = extractFunctionParameterValue(parameters, 0);

  if (!value.isArray()) {
    // not an array
    registerWarning(expressionContext, AFN, TRI_ERROR_QUERY_ARRAY_EXPECTED);
    return AqlValue(AqlValueHintNull());
  }

  AqlValueMaterializer materializer(vopts);
  VPackSlice slice = materializer.slice(value, false);

  basics::VelocyPackHelper::VPackLess<true> less(
      trx->transactionContext()->getVPackOptions(), &slice, &slice);
  std::map<VPackSlice, size_t, basics::VelocyPackHelper::VPackLess<true>>
      values(less);
  for (VPackSlice it : VPackArrayIterator(slice)) {
    if (!it.isNone()) {
      auto [itr, emplaced] = values.try_emplace(it, 1);
      if (!emplaced) {
        ++itr->second;
      }
    }
  }

  transaction::BuilderLeaser builder(trx);
  builder->openArray();
  for (auto const& it : values) {
    for (size_t i = 0; i < it.second; ++i) {
      builder->add(it.first);
    }
  }
  builder->close();
  return AqlValue(builder->slice(), builder->size());
}

/// @brief function UNION
AqlValue Functions::Union(ExpressionContext* expressionContext, AstNode const&,
                          VPackFunctionParametersView parameters) {
  static char const* AFN = "UNION";

  transaction::Methods* trx = &expressionContext->trx();
  auto* vopts = &trx->vpackOptions();
  transaction::BuilderLeaser builder(trx);
  builder->openArray();
  size_t const n = parameters.size();
  for (size_t i = 0; i < n; ++i) {
    AqlValue const& value = extractFunctionParameterValue(parameters, i);

    if (!value.isArray()) {
      // not an array
      registerInvalidArgumentWarning(expressionContext, AFN);
      return AqlValue(AqlValueHintNull());
    }

    TRI_IF_FAILURE("AqlFunctions::OutOfMemory1") {
      THROW_ARANGO_EXCEPTION(TRI_ERROR_DEBUG);
    }

    AqlValueMaterializer materializer(vopts);
    VPackSlice slice = materializer.slice(value, false);

    // this passes ownership for the JSON contents into result
    for (VPackSlice it : VPackArrayIterator(slice)) {
      builder->add(it);
      TRI_IF_FAILURE("AqlFunctions::OutOfMemory2") {
        THROW_ARANGO_EXCEPTION(TRI_ERROR_DEBUG);
      }
    }
  }
  builder->close();
  TRI_IF_FAILURE("AqlFunctions::OutOfMemory3") {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_DEBUG);
  }

  return AqlValue(builder->slice(), builder->size());
}

/// @brief function UNION_DISTINCT
AqlValue Functions::UnionDistinct(ExpressionContext* expressionContext,
                                  AstNode const&,
                                  VPackFunctionParametersView parameters) {
  static char const* AFN = "UNION_DISTINCT";

  transaction::Methods* trx = &expressionContext->trx();
  auto* vopts = &trx->vpackOptions();

  size_t const n = parameters.size();
  containers::FlatHashSet<VPackSlice, basics::VelocyPackHelper::VPackHash,
                          basics::VelocyPackHelper::VPackEqual>
      values(512, basics::VelocyPackHelper::VPackHash(),
             basics::VelocyPackHelper::VPackEqual(vopts));

  std::vector<AqlValueMaterializer> materializers;
  materializers.reserve(n);
  for (size_t i = 0; i < n; ++i) {
    AqlValue const& value = extractFunctionParameterValue(parameters, i);

    if (!value.isArray()) {
      // not an array
      registerInvalidArgumentWarning(expressionContext, AFN);
      return AqlValue(AqlValueHintNull());
    }

    materializers.emplace_back(vopts);
    VPackSlice slice = materializers.back().slice(value, false);

    for (VPackSlice v : VPackArrayIterator(slice)) {
      v = v.resolveExternal();
      if (values.find(v) == values.end()) {
        TRI_IF_FAILURE("AqlFunctions::OutOfMemory1") {
          THROW_ARANGO_EXCEPTION(TRI_ERROR_DEBUG);
        }

        values.emplace(v);
      }
    }
  }

  TRI_IF_FAILURE("AqlFunctions::OutOfMemory2") {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_DEBUG);
  }

  transaction::BuilderLeaser builder(trx);
  builder->openArray();
  for (auto const& it : values) {
    builder->add(it);
  }
  builder->close();

  TRI_IF_FAILURE("AqlFunctions::OutOfMemory3") {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_DEBUG);
  }

  return AqlValue(builder->slice(), builder->size());
}

/// @brief function INTERSECTION
AqlValue Functions::Intersection(ExpressionContext* expressionContext,
                                 AstNode const&,
                                 VPackFunctionParametersView parameters) {
  static char const* AFN = "INTERSECTION";

  transaction::Methods* trx = &expressionContext->trx();
  auto* vopts = &trx->vpackOptions();
  std::unordered_map<VPackSlice, size_t, basics::VelocyPackHelper::VPackHash,
                     basics::VelocyPackHelper::VPackEqual>
      values(512, basics::VelocyPackHelper::VPackHash(),
             basics::VelocyPackHelper::VPackEqual(vopts));

  size_t const n = parameters.size();
  std::vector<AqlValueMaterializer> materializers;
  materializers.reserve(n);
  for (size_t i = 0; i < n; ++i) {
    AqlValue const& value = extractFunctionParameterValue(parameters, i);

    if (!value.isArray()) {
      // not an array
      registerWarning(expressionContext, AFN, TRI_ERROR_QUERY_ARRAY_EXPECTED);
      return AqlValue(AqlValueHintNull());
    }

    materializers.emplace_back(vopts);
    VPackSlice slice = materializers.back().slice(value, false);

    for (VPackSlice it : VPackArrayIterator(slice)) {
      if (i == 0) {
        // round one

        TRI_IF_FAILURE("AqlFunctions::OutOfMemory1") {
          THROW_ARANGO_EXCEPTION(TRI_ERROR_DEBUG);
        }

        values.try_emplace(it, 1);
      } else {
        // check if we have seen the same element before
        auto found = values.find(it);
        if (found != values.end()) {
          // already seen
          if ((*found).second < i) {
            (*found).second = 0;
          } else {
            (*found).second = i + 1;
          }
        }
      }
    }
  }

  TRI_IF_FAILURE("AqlFunctions::OutOfMemory2") {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_DEBUG);
  }

  transaction::BuilderLeaser builder(trx);
  builder->openArray();
  for (auto const& it : values) {
    if (it.second == n) {
      builder->add(it.first);
    }
  }
  builder->close();

  TRI_IF_FAILURE("AqlFunctions::OutOfMemory3") {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_DEBUG);
  }
  return AqlValue(builder->slice(), builder->size());
}

/// @brief function JACCARD
AqlValue Functions::Jaccard(ExpressionContext* ctx, AstNode const&,
                            VPackFunctionParametersView args) {
  static char const* AFN = "JACCARD";

  typedef std::unordered_map<VPackSlice, size_t,
                             basics::VelocyPackHelper::VPackHash,
                             basics::VelocyPackHelper::VPackEqual>
      ValuesMap;

  transaction::Methods* trx = &ctx->trx();
  auto* vopts = &trx->vpackOptions();
  ValuesMap values(512, basics::VelocyPackHelper::VPackHash(),
                   basics::VelocyPackHelper::VPackEqual(vopts));

  AqlValue const& lhs = extractFunctionParameterValue(args, 0);

  if (ADB_UNLIKELY(!lhs.isArray())) {
    // not an array
    registerWarning(ctx, AFN, TRI_ERROR_QUERY_ARRAY_EXPECTED);
    return AqlValue(AqlValueHintNull());
  }

  AqlValue const& rhs = extractFunctionParameterValue(args, 1);

  if (ADB_UNLIKELY(!rhs.isArray())) {
    // not an array
    registerWarning(ctx, AFN, TRI_ERROR_QUERY_ARRAY_EXPECTED);
    return AqlValue(AqlValueHintNull());
  }

  AqlValueMaterializer lhsMaterializer(vopts);
  AqlValueMaterializer rhsMaterializer(vopts);

  VPackSlice lhsSlice = lhsMaterializer.slice(lhs, false);
  VPackSlice rhsSlice = rhsMaterializer.slice(rhs, false);

  size_t cardinality = 0;  // cardinality of intersection

  for (VPackSlice slice : VPackArrayIterator(lhsSlice)) {
    values.try_emplace(slice, 1);
  }

  for (VPackSlice slice : VPackArrayIterator(rhsSlice)) {
    auto& count = values.try_emplace(slice, 0).first->second;
    cardinality += count;
    count = 0;
  }

  auto const jaccard =
      values.empty() ? 1.0 : double_t(cardinality) / values.size();

  return AqlValue{AqlValueHintDouble{jaccard}};
}

/// @brief function OUTERSECTION
AqlValue Functions::Outersection(ExpressionContext* expressionContext,
                                 AstNode const&,
                                 VPackFunctionParametersView parameters) {
  static char const* AFN = "OUTERSECTION";

  transaction::Methods* trx = &expressionContext->trx();
  auto* vopts = &trx->vpackOptions();
  std::unordered_map<VPackSlice, size_t, basics::VelocyPackHelper::VPackHash,
                     basics::VelocyPackHelper::VPackEqual>
      values(512, basics::VelocyPackHelper::VPackHash(),
             basics::VelocyPackHelper::VPackEqual(vopts));

  size_t const n = parameters.size();
  std::vector<AqlValueMaterializer> materializers;
  materializers.reserve(n);
  for (size_t i = 0; i < n; ++i) {
    AqlValue const& value = extractFunctionParameterValue(parameters, i);

    if (!value.isArray()) {
      // not an array
      registerWarning(expressionContext, AFN, TRI_ERROR_QUERY_ARRAY_EXPECTED);
      return AqlValue(AqlValueHintNull());
    }

    materializers.emplace_back(vopts);
    VPackSlice slice = materializers.back().slice(value, false);

    for (VPackSlice it : VPackArrayIterator(slice)) {
      // check if we have seen the same element before
      auto result = values.insert({it, 1});
      if (!result.second) {
        // already seen
        TRI_ASSERT(result.first->second > 0);
        ++(result.first->second);
      }
    }
  }

  TRI_IF_FAILURE("AqlFunctions::OutOfMemory2") {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_DEBUG);
  }

  transaction::BuilderLeaser builder(trx);
  builder->openArray();
  for (auto const& it : values) {
    if (it.second == 1) {
      builder->add(it.first);
    }
  }
  builder->close();

  TRI_IF_FAILURE("AqlFunctions::OutOfMemory3") {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_DEBUG);
  }
  return AqlValue(builder->slice(), builder->size());
}

/// @brief function DISTANCE
AqlValue Functions::Distance(ExpressionContext* expressionContext,
                             AstNode const&,
                             VPackFunctionParametersView parameters) {
  static char const* AFN = "DISTANCE";

  AqlValue const& lat1 = extractFunctionParameterValue(parameters, 0);
  AqlValue const& lon1 = extractFunctionParameterValue(parameters, 1);
  AqlValue const& lat2 = extractFunctionParameterValue(parameters, 2);
  AqlValue const& lon2 = extractFunctionParameterValue(parameters, 3);

  // non-numeric input...
  if (!lat1.isNumber() || !lon1.isNumber() || !lat2.isNumber() ||
      !lon2.isNumber()) {
    registerWarning(expressionContext, AFN,
                    TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH);
    return AqlValue(AqlValueHintNull());
  }

  bool failed;
  bool error = false;
  double lat1Value = lat1.toDouble(failed);
  error |= failed;
  double lon1Value = lon1.toDouble(failed);
  error |= failed;
  double lat2Value = lat2.toDouble(failed);
  error |= failed;
  double lon2Value = lon2.toDouble(failed);
  error |= failed;

  if (error) {
    registerWarning(expressionContext, AFN,
                    TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH);
    return AqlValue(AqlValueHintNull());
  }

  auto toRadians = [](double degrees) -> double {
    return degrees * (std::acos(-1.0) / 180.0);
  };

  double p1 = toRadians(lat1Value);
  double p2 = toRadians(lat2Value);
  double d1 = toRadians(lat2Value - lat1Value);
  double d2 = toRadians(lon2Value - lon1Value);

  double a =
      std::sin(d1 / 2.0) * std::sin(d1 / 2.0) +
      std::cos(p1) * std::cos(p2) * std::sin(d2 / 2.0) * std::sin(d2 / 2.0);

  double c = 2.0 * std::atan2(std::sqrt(a), std::sqrt(1.0 - a));
  double const EARTHRADIAN = 6371000.0;  // metres

  return ::numberValue(EARTHRADIAN * c, true);
}

/// @brief function GEO_DISTANCE
AqlValue Functions::GeoDistance(ExpressionContext* exprCtx, AstNode const&,
                                VPackFunctionParametersView parameters) {
  constexpr char const AFN[] = "GEO_DISTANCE";
  geo::ShapeContainer shape1, shape2;

  auto res = ::parseShape(exprCtx, extractFunctionParameterValue(parameters, 0),
                          shape1);

  if (res.fail()) {
    registerWarning(exprCtx, AFN, res);
    return AqlValue(AqlValueHintNull());
  }

  res = ::parseShape(exprCtx, extractFunctionParameterValue(parameters, 1),
                     shape2);

  if (res.fail()) {
    registerWarning(exprCtx, AFN, res);
    return AqlValue(AqlValueHintNull());
  }

  if (parameters.size() > 2 && parameters[2].isString()) {
    VPackValueLength len;
    const char* ptr = parameters[2].slice().getStringUnchecked(len);
    geo::Ellipsoid const& e = geo::utils::ellipsoidFromString(ptr, len);
    return ::numberValue(shape1.distanceFromCentroid(shape2.centroid(), e),
                         true);
  }
  return ::numberValue(shape1.distanceFromCentroid(shape2.centroid()), true);
}

/// @brief function GEO_IN_RANGE
AqlValue Functions::GeoInRange(ExpressionContext* ctx, AstNode const& node,
                               VPackFunctionParametersView args) {
  TRI_ASSERT(ctx);
  TRI_ASSERT(aql::NODE_TYPE_FCALL == node.type);

  auto const* impl = static_cast<aql::Function*>(node.getData());
  TRI_ASSERT(impl);

  auto const argc = args.size();

  if (argc < 4 || argc > 7) {
    registerWarning(ctx, impl->name.c_str(),
                    TRI_ERROR_QUERY_FUNCTION_ARGUMENT_NUMBER_MISMATCH);
    return AqlValue(AqlValueHintNull());
  }

  geo::ShapeContainer shape1, shape2;
  auto res = parseShape(ctx, extractFunctionParameterValue(args, 0), shape1);

  if (res.fail()) {
    registerWarning(ctx, impl->name.c_str(), res);
    return AqlValue(AqlValueHintNull());
  }

  res = parseShape(ctx, extractFunctionParameterValue(args, 1), shape2);

  if (res.fail()) {
    registerWarning(ctx, impl->name.c_str(), res);
    return AqlValue(AqlValueHintNull());
  }

  auto const& lowerBound = extractFunctionParameterValue(args, 2);

  if (!lowerBound.isNumber()) {
    registerWarning(
        ctx, impl->name.c_str(),
        {TRI_ERROR_BAD_PARAMETER, "3rd argument requires a number"});
    return AqlValue(AqlValueHintNull());
  }

  auto const& upperBound = extractFunctionParameterValue(args, 3);

  if (!upperBound.isNumber()) {
    registerWarning(
        ctx, impl->name.c_str(),
        {TRI_ERROR_BAD_PARAMETER, "4th argument requires a number"});
    return AqlValue(AqlValueHintNull());
  }

  bool includeLower = true;
  bool includeUpper = true;
  geo::Ellipsoid const* ellipsoid = &geo::SPHERE;

  if (argc > 4) {
    auto const& includeLowerValue = extractFunctionParameterValue(args, 4);

    if (!includeLowerValue.isBoolean()) {
      registerWarning(
          ctx, impl->name.c_str(),
          {TRI_ERROR_BAD_PARAMETER, "5th argument requires a bool"});
      return AqlValue(AqlValueHintNull());
    }

    includeLower = includeLowerValue.toBoolean();

    if (argc > 5) {
      auto const& includeUpperValue = extractFunctionParameterValue(args, 5);

      if (!includeUpperValue.isBoolean()) {
        registerWarning(
            ctx, impl->name.c_str(),
            {TRI_ERROR_BAD_PARAMETER, "6th argument requires a bool"});
        return AqlValue(AqlValueHintNull());
      }

      includeUpper = includeUpperValue.toBoolean();
    }

    if (argc > 6) {
      auto const& value = extractFunctionParameterValue(args, 6);
      if (value.isString()) {
        VPackValueLength len;
        char const* ptr = value.slice().getStringUnchecked(len);
        ellipsoid = &geo::utils::ellipsoidFromString(ptr, len);
      }
    }
  }

  auto const minDistance = lowerBound.toDouble();
  auto const maxDistance = upperBound.toDouble();
  auto const distance =
      (ellipsoid == &geo::SPHERE
           ? shape1.distanceFromCentroid(shape2.centroid())
           : shape1.distanceFromCentroid(shape2.centroid(), *ellipsoid));

  return AqlValue{AqlValueHintBool{
      (includeLower ? distance >= minDistance : distance > minDistance) &&
      (includeUpper ? distance <= maxDistance : distance < maxDistance)}};
}

/// @brief function GEO_CONTAINS
AqlValue Functions::GeoContains(ExpressionContext* expressionContext,
                                AstNode const& node,
                                VPackFunctionParametersView parameters) {
  return ::geoContainsIntersect(expressionContext, node, parameters,
                                "GEO_CONTAINS", true);
}

/// @brief function GEO_INTERSECTS
AqlValue Functions::GeoIntersects(ExpressionContext* expressionContext,
                                  AstNode const& node,
                                  VPackFunctionParametersView parameters) {
  return ::geoContainsIntersect(expressionContext, node, parameters,
                                "GEO_INTERSECTS", false);
}

/// @brief function GEO_EQUALS
AqlValue Functions::GeoEquals(ExpressionContext* expressionContext,
                              AstNode const&,
                              VPackFunctionParametersView parameters) {
  transaction::Methods* trx = &expressionContext->trx();
  auto* vopts = &trx->vpackOptions();
  AqlValue p1 = extractFunctionParameterValue(parameters, 0);
  AqlValue p2 = extractFunctionParameterValue(parameters, 1);

  if (!p1.isObject() || !p2.isObject()) {
    registerWarning(expressionContext, "GEO_EQUALS",
                    Result(TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH,
                           "Expecting GeoJSON object"));
    return AqlValue(AqlValueHintNull());
  }

  AqlValueMaterializer mat1(vopts);
  AqlValueMaterializer mat2(vopts);

  geo::ShapeContainer first, second;
  Result res1 = geo::geojson::parseRegion(mat1.slice(p1, true), first, false);
  Result res2 = geo::geojson::parseRegion(mat2.slice(p2, true), second, false);

  if (res1.fail()) {
    registerWarning(expressionContext, "GEO_EQUALS", res1);
    return AqlValue(AqlValueHintNull());
  }
  if (res2.fail()) {
    registerWarning(expressionContext, "GEO_EQUALS", res2);
    return AqlValue(AqlValueHintNull());
  }

  bool result = first.equals(&second);
  return AqlValue(AqlValueHintBool(result));
}

/// @brief function GEO_AREA
AqlValue Functions::GeoArea(ExpressionContext* expressionContext,
                            AstNode const&,
                            VPackFunctionParametersView parameters) {
  transaction::Methods* trx = &expressionContext->trx();
  auto* vopts = &trx->vpackOptions();
  AqlValue p1 = extractFunctionParameterValue(parameters, 0);
  AqlValue p2 = extractFunctionParameterValue(parameters, 1);

  AqlValueMaterializer mat(vopts);

  geo::ShapeContainer shape;
  Result res = geo::geojson::parseRegion(mat.slice(p1, true), shape, false);

  if (res.fail()) {
    registerWarning(expressionContext, "GEO_AREA", res);
    return AqlValue(AqlValueHintNull());
  }

  auto detEllipsoid = [](AqlValue const& p) {
    if (p.isString()) {
      VPackValueLength len;
      const char* ptr = p.slice().getStringUnchecked(len);
      return geo::utils::ellipsoidFromString(ptr, len);
    }
    return geo::SPHERE;
  };
  return AqlValue(AqlValueHintDouble(shape.area(detEllipsoid(p2))));
}

/// @brief function IS_IN_POLYGON
AqlValue Functions::IsInPolygon(ExpressionContext* expressionContext,
                                AstNode const&,
                                VPackFunctionParametersView parameters) {
  transaction::Methods* trx = &expressionContext->trx();
  auto* vopts = &trx->vpackOptions();
  AqlValue const& coords = extractFunctionParameterValue(parameters, 0);
  AqlValue p2 = extractFunctionParameterValue(parameters, 1);
  AqlValue p3 = extractFunctionParameterValue(parameters, 2);

  if (!coords.isArray()) {
    registerWarning(expressionContext, "IS_IN_POLYGON",
                    TRI_ERROR_QUERY_ARRAY_EXPECTED);
    return AqlValue(AqlValueHintNull());
  }

  double latitude, longitude;
  bool geoJson = false;
  if (p2.isArray()) {
    if (p2.length() < 2) {
      registerInvalidArgumentWarning(expressionContext, "IS_IN_POLYGON");
      return AqlValue(AqlValueHintNull());
    }
    AqlValueMaterializer materializer(vopts);
    VPackSlice arr = materializer.slice(p2, false);
    geoJson = p3.isBoolean() && p3.toBoolean();
    // if geoJson, map [lon, lat] -> lat, lon
    VPackSlice lat = geoJson ? arr[1] : arr[0];
    VPackSlice lon = geoJson ? arr[0] : arr[1];
    if (!lat.isNumber() || !lon.isNumber()) {
      registerInvalidArgumentWarning(expressionContext, "IS_IN_POLYGON");
      return AqlValue(AqlValueHintNull());
    }
    latitude = lat.getNumber<double>();
    longitude = lon.getNumber<double>();
  } else if (p2.isNumber() && p3.isNumber()) {
    bool failed1 = false, failed2 = false;
    latitude = p2.toDouble(failed1);
    longitude = p3.toDouble(failed2);
    if (failed1 || failed2) {
      registerInvalidArgumentWarning(expressionContext, "IS_IN_POLYGON");
      return AqlValue(AqlValueHintNull());
    }
  } else {
    registerInvalidArgumentWarning(expressionContext, "IS_IN_POLYGON");
    return AqlValue(AqlValueHintNull());
  }

  S2Loop loop;
  loop.set_s2debug_override(S2Debug::DISABLE);
  Result res = geo::geojson::parseLoop(coords.slice(), geoJson, loop);
  if (res.fail() || !loop.IsValid()) {
    registerWarning(expressionContext, "IS_IN_POLYGON", res);
    return AqlValue(AqlValueHintNull());
  }

  S2LatLng latLng = S2LatLng::FromDegrees(latitude, longitude);
  return AqlValue(AqlValueHintBool(loop.Contains(latLng.ToPoint())));
}

/// @brief geo constructors

/// @brief function GEO_POINT
AqlValue Functions::GeoPoint(ExpressionContext* expressionContext,
                             AstNode const&,
                             VPackFunctionParametersView parameters) {
  transaction::Methods* trx = &expressionContext->trx();
  size_t const n = parameters.size();

  if (n < 2) {
    // no parameters
    return AqlValue(AqlValueHintNull());
  }

  AqlValue lon1 = extractFunctionParameterValue(parameters, 0);
  AqlValue lat1 = extractFunctionParameterValue(parameters, 1);

  // non-numeric input
  if (!lat1.isNumber() || !lon1.isNumber()) {
    registerWarning(expressionContext, "GEO_POINT",
                    TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH);
    return AqlValue(AqlValueHintNull());
  }

  bool failed;
  bool error = false;
  double lon1Value = lon1.toDouble(failed);
  error |= failed;
  double lat1Value = lat1.toDouble(failed);
  error |= failed;

  if (error) {
    registerWarning(expressionContext, "GEO_POINT",
                    TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH);
    return AqlValue(AqlValueHintNull());
  }

  transaction::BuilderLeaser builder(trx);
  builder->openObject();
  builder->add("type", VPackValue("Point"));
  builder->add("coordinates", VPackValue(VPackValueType::Array));
  builder->add(VPackValue(lon1Value));
  builder->add(VPackValue(lat1Value));
  builder->close();
  builder->close();

  return AqlValue(builder->slice(), builder->size());
}

/// @brief function GEO_MULTIPOINT
AqlValue Functions::GeoMultiPoint(ExpressionContext* expressionContext,
                                  AstNode const&,
                                  VPackFunctionParametersView parameters) {
  transaction::Methods* trx = &expressionContext->trx();
  auto* vopts = &trx->vpackOptions();
  size_t const n = parameters.size();

  if (n < 1) {
    // no parameters
    return AqlValue(AqlValueHintNull());
  }

  AqlValue const& geoArray = extractFunctionParameterValue(parameters, 0);

  if (!geoArray.isArray()) {
    registerWarning(expressionContext, "GEO_MULTIPOINT",
                    TRI_ERROR_QUERY_ARRAY_EXPECTED);
    return AqlValue(AqlValueHintNull());
  }
  if (geoArray.length() < 2) {
    registerWarning(expressionContext, "GEO_MULTIPOINT",
                    Result(TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH,
                           "a MultiPoint needs at least two positions"));
    return AqlValue(AqlValueHintNull());
  }

  transaction::BuilderLeaser builder(trx);

  builder->openObject();
  builder->add("type", VPackValue("MultiPoint"));
  builder->add("coordinates", VPackValue(VPackValueType::Array));

  AqlValueMaterializer materializer(vopts);
  VPackSlice s = materializer.slice(geoArray, false);
  for (VPackSlice v : VPackArrayIterator(s)) {
    if (v.isArray()) {
      builder->openArray();
      for (auto const& coord : VPackArrayIterator(v)) {
        if (coord.isNumber()) {
          builder->add(VPackValue(coord.getNumber<double>()));
        } else {
          registerWarning(
              expressionContext, "GEO_MULTIPOINT",
              Result(TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH,
                     "not a numeric value"));
          return AqlValue(AqlValueHintNull());
        }
      }
      builder->close();
    } else {
      registerWarning(expressionContext, "GEO_MULTIPOINT",
                      Result(TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH,
                             "not an array containing positions"));
      return AqlValue(AqlValueHintNull());
    }
  }

  builder->close();
  builder->close();

  return AqlValue(builder->slice(), builder->size());
}

/// @brief function GEO_POLYGON
AqlValue Functions::GeoPolygon(ExpressionContext* expressionContext,
                               AstNode const&,
                               VPackFunctionParametersView parameters) {
  transaction::Methods* trx = &expressionContext->trx();
  auto* vopts = &trx->vpackOptions();
  size_t const n = parameters.size();

  if (n < 1) {
    // no parameters
    return AqlValue(AqlValueHintNull());
  }

  AqlValue const& geoArray = extractFunctionParameterValue(parameters, 0);

  if (!geoArray.isArray()) {
    registerWarning(expressionContext, "GEO_POLYGON",
                    TRI_ERROR_QUERY_ARRAY_EXPECTED);
    return AqlValue(AqlValueHintNull());
  }

  transaction::BuilderLeaser builder(trx);
  builder->openObject();
  builder->add("type", VPackValue("Polygon"));
  builder->add("coordinates", VPackValue(VPackValueType::Array));

  AqlValueMaterializer materializer(vopts);
  VPackSlice s = materializer.slice(geoArray, false);

  Result res = ::parseGeoPolygon(s, *builder.get());
  if (res.fail()) {
    registerWarning(expressionContext, "GEO_POLYGON", res);
    return AqlValue(AqlValueHintNull());
  }

  builder->close();  // coordinates
  builder->close();  // object

  // Now actually parse the result with S2:
  geo::ShapeContainer container;
  res = geo::geojson::parsePolygon(builder->slice(), container, false);
  if (res.fail()) {
    registerWarning(expressionContext, "GEO_POLYGON", res);
    return AqlValue(AqlValueHintNull());
  }

  return AqlValue(builder->slice(), builder->size());
}

/// @brief function GEO_MULTIPOLYGON
AqlValue Functions::GeoMultiPolygon(ExpressionContext* expressionContext,
                                    AstNode const&,
                                    VPackFunctionParametersView parameters) {
  transaction::Methods* trx = &expressionContext->trx();
  auto* vopts = &trx->vpackOptions();
  size_t const n = parameters.size();

  if (n < 1) {
    // no parameters
    return AqlValue(AqlValueHintNull());
  }

  AqlValue const& geoArray = extractFunctionParameterValue(parameters, 0);

  if (!geoArray.isArray()) {
    registerWarning(expressionContext, "GEO_MULTIPOLYGON",
                    TRI_ERROR_QUERY_ARRAY_EXPECTED);
    return AqlValue(AqlValueHintNull());
  }

  AqlValueMaterializer materializer(vopts);
  VPackSlice s = materializer.slice(geoArray, false);

  /*
  return GEO_MULTIPOLYGON([
    [
      [[40, 40], [20, 45], [45, 30], [40, 40]]
    ],
    [
      [[20, 35], [10, 30], [10, 10], [30, 5], [45, 20], [20, 35]],
      [[30, 20], [20, 15], [20, 25], [30, 20]]
    ]
  ])
  */

  TRI_ASSERT(s.isArray());
  if (s.length() < 2) {
    registerWarning(
        expressionContext, "GEO_MULTIPOLYGON",
        Result(TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH,
               "a MultiPolygon needs at least two Polygons inside."));
    return AqlValue(AqlValueHintNull());
  }

  transaction::BuilderLeaser builder(trx);
  builder->openObject();
  builder->add("type", VPackValue("MultiPolygon"));
  builder->add("coordinates", VPackValue(VPackValueType::Array));

  for (auto const& arrayOfPolygons : VPackArrayIterator(s)) {
    if (!arrayOfPolygons.isArray()) {
      registerWarning(
          expressionContext, "GEO_MULTIPOLYGON",
          Result(TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH,
                 "a MultiPolygon needs at least two Polygons inside."));
      return AqlValue(AqlValueHintNull());
    }
    builder->openArray();  // arrayOfPolygons
    for (VPackSlice v : VPackArrayIterator(arrayOfPolygons)) {
      Result res = ::parseGeoPolygon(v, *builder.get());
      if (res.fail()) {
        registerWarning(expressionContext, "GEO_MULTIPOLYGON", res);
        return AqlValue(AqlValueHintNull());
      }
    }
    builder->close();  // arrayOfPolygons close
  }

  builder->close();
  builder->close();

  // Now actually parse the result with S2:
  geo::ShapeContainer container;
  Result res =
      geo::geojson::parseMultiPolygon(builder->slice(), container, false);
  if (res.fail()) {
    registerWarning(expressionContext, "GEO_MULTIPOLYGON", res);
    return AqlValue(AqlValueHintNull());
  }

  return AqlValue(builder->slice(), builder->size());
}

/// @brief function GEO_LINESTRING
AqlValue Functions::GeoLinestring(ExpressionContext* expressionContext,
                                  AstNode const&,
                                  VPackFunctionParametersView parameters) {
  transaction::Methods* trx = &expressionContext->trx();
  auto* vopts = &trx->vpackOptions();
  size_t const n = parameters.size();

  if (n < 1) {
    // no parameters
    return AqlValue(AqlValueHintNull());
  }

  AqlValue const& geoArray = extractFunctionParameterValue(parameters, 0);

  if (!geoArray.isArray()) {
    registerWarning(expressionContext, "GEO_LINESTRING",
                    TRI_ERROR_QUERY_ARRAY_EXPECTED);
    return AqlValue(AqlValueHintNull());
  }
  if (geoArray.length() < 2) {
    registerWarning(expressionContext, "GEO_LINESTRING",
                    Result(TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH,
                           "a LineString needs at least two positions"));
    return AqlValue(AqlValueHintNull());
  }

  transaction::BuilderLeaser builder(trx);

  builder->add(VPackValue(VPackValueType::Object));
  builder->add("type", VPackValue("LineString"));
  builder->add("coordinates", VPackValue(VPackValueType::Array));

  AqlValueMaterializer materializer(vopts);
  VPackSlice s = materializer.slice(geoArray, false);
  for (VPackSlice v : VPackArrayIterator(s)) {
    if (v.isArray()) {
      builder->openArray();
      for (auto const& coord : VPackArrayIterator(v)) {
        if (coord.isNumber()) {
          builder->add(VPackValue(coord.getNumber<double>()));
        } else {
          registerWarning(
              expressionContext, "GEO_LINESTRING",
              Result(TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH,
                     "not a numeric value"));
          return AqlValue(AqlValueHintNull());
        }
      }
      builder->close();
    } else {
      registerWarning(expressionContext, "GEO_LINESTRING",
                      Result(TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH,
                             "not an array containing positions"));
      return AqlValue(AqlValueHintNull());
    }
  }

  builder->close();
  builder->close();

  return AqlValue(builder->slice(), builder->size());
}

/// @brief function GEO_MULTILINESTRING
AqlValue Functions::GeoMultiLinestring(ExpressionContext* expressionContext,
                                       AstNode const&,
                                       VPackFunctionParametersView parameters) {
  transaction::Methods* trx = &expressionContext->trx();
  auto* vopts = &trx->vpackOptions();
  size_t const n = parameters.size();

  if (n < 1) {
    // no parameters
    return AqlValue(AqlValueHintNull());
  }

  AqlValue const& geoArray = extractFunctionParameterValue(parameters, 0);

  if (!geoArray.isArray()) {
    registerWarning(expressionContext, "GEO_MULTILINESTRING",
                    TRI_ERROR_QUERY_ARRAY_EXPECTED);
    return AqlValue(AqlValueHintNull());
  }
  if (geoArray.length() < 1) {
    registerWarning(
        expressionContext, "GEO_MULTILINESTRING",
        Result(TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH,
               "a MultiLineString needs at least one array of linestrings"));
    return AqlValue(AqlValueHintNull());
  }

  transaction::BuilderLeaser builder(trx);

  builder->add(VPackValue(VPackValueType::Object));
  builder->add("type", VPackValue("MultiLineString"));
  builder->add("coordinates", VPackValue(VPackValueType::Array));

  AqlValueMaterializer materializer(vopts);
  VPackSlice s = materializer.slice(geoArray, false);
  for (VPackSlice v : VPackArrayIterator(s)) {
    if (v.isArray()) {
      if (v.length() > 1) {
        builder->openArray();
        for (VPackSlice const inner : VPackArrayIterator(v)) {
          if (inner.isArray()) {
            builder->openArray();
            for (VPackSlice const coord : VPackArrayIterator(inner)) {
              if (coord.isNumber()) {
                builder->add(VPackValue(coord.getNumber<double>()));
              } else {
                registerWarning(
                    expressionContext, "GEO_MULTILINESTRING",
                    Result(TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH,
                           "not a numeric value"));
                return AqlValue(AqlValueHintNull());
              }
            }
            builder->close();
          } else {
            registerWarning(
                expressionContext, "GEO_MULTILINESTRING",
                Result(TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH,
                       "not an array containing positions"));
            return AqlValue(AqlValueHintNull());
          }
        }
        builder->close();
      } else {
        registerWarning(expressionContext, "GEO_MULTILINESTRING",
                        Result(TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH,
                               "not an array containing linestrings"));
        return AqlValue(AqlValueHintNull());
      }
    } else {
      registerWarning(expressionContext, "GEO_MULTILINESTRING",
                      Result(TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH,
                             "not an array containing positions"));
      return AqlValue(AqlValueHintNull());
    }
  }

  builder->close();
  builder->close();

  return AqlValue(builder->slice(), builder->size());
}

/// @brief function FLATTEN
AqlValue Functions::Flatten(ExpressionContext* expressionContext,
                            AstNode const&,
                            VPackFunctionParametersView parameters) {
  transaction::Methods* trx = &expressionContext->trx();
  auto* vopts = &trx->vpackOptions();
  // cppcheck-suppress variableScope
  static char const* AFN = "FLATTEN";

  AqlValue const& list = extractFunctionParameterValue(parameters, 0);
  if (!list.isArray()) {
    registerWarning(expressionContext, AFN, TRI_ERROR_QUERY_ARRAY_EXPECTED);
    return AqlValue(AqlValueHintNull());
  }

  size_t maxDepth = 1;
  if (parameters.size() == 2) {
    AqlValue const& maxDepthValue =
        extractFunctionParameterValue(parameters, 1);
    bool failed;
    double tmpMaxDepth = maxDepthValue.toDouble(failed);
    if (failed || tmpMaxDepth < 1) {
      maxDepth = 1;
    } else {
      maxDepth = static_cast<size_t>(tmpMaxDepth);
    }
  }

  AqlValueMaterializer materializer(vopts);
  VPackSlice listSlice = materializer.slice(list, false);

  transaction::BuilderLeaser builder(trx);
  builder->openArray();
  ::flattenList(listSlice, maxDepth, 0, *builder.get());
  builder->close();
  return AqlValue(builder->slice(), builder->size());
}

/// @brief function ZIP
AqlValue Functions::Zip(ExpressionContext* expressionContext, AstNode const&,
                        VPackFunctionParametersView parameters) {
  // cppcheck-suppress variableScope
  static char const* AFN = "ZIP";

  AqlValue const& keys = extractFunctionParameterValue(parameters, 0);
  AqlValue const& values = extractFunctionParameterValue(parameters, 1);

  if (!keys.isArray() || !values.isArray() ||
      keys.length() != values.length()) {
    registerWarning(expressionContext, AFN,
                    TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH);
    return AqlValue(AqlValueHintNull());
  }

  transaction::Methods* trx = &expressionContext->trx();
  auto* vopts = &trx->vpackOptions();

  AqlValueMaterializer keyMaterializer(vopts);
  VPackSlice keysSlice = keyMaterializer.slice(keys, false);

  AqlValueMaterializer valueMaterializer(vopts);
  VPackSlice valuesSlice = valueMaterializer.slice(values, false);

  transaction::BuilderLeaser builder(trx);
  builder->openObject();

  // Buffer will temporarily hold the keys
  containers::FlatHashSet<std::string> keysSeen;
  transaction::StringLeaser buffer(trx);
  velocypack::StringSink adapter(buffer.get());

  VPackArrayIterator keysIt(keysSlice);
  VPackArrayIterator valuesIt(valuesSlice);

  TRI_ASSERT(keysIt.size() == valuesIt.size());

  while (keysIt.valid()) {
    TRI_ASSERT(valuesIt.valid());

    // stringify key
    buffer->clear();
    Stringify(vopts, adapter, keysIt.value());

    if (keysSeen.emplace(buffer->data(), buffer->length()).second) {
      // non-duplicate key
      builder->add(buffer->data(), buffer->length(), valuesIt.value());
    }

    keysIt.next();
    valuesIt.next();
  }

  builder->close();

  return AqlValue(builder->slice(), builder->size());
}

/// @brief function JSON_STRINGIFY
AqlValue Functions::JsonStringify(ExpressionContext* exprCtx, AstNode const&,
                                  VPackFunctionParametersView parameters) {
  transaction::Methods* trx = &exprCtx->trx();
  auto* vopts = &trx->vpackOptions();
  AqlValue const& value = extractFunctionParameterValue(parameters, 0);
  AqlValueMaterializer materializer(vopts);
  VPackSlice slice = materializer.slice(value, false);

  transaction::StringLeaser buffer(trx);
  velocypack::StringSink adapter(buffer.get());

  VPackDumper dumper(&adapter, trx->transactionContextPtr()->getVPackOptions());
  dumper.dump(slice);

  return AqlValue(buffer->data(), buffer->length());
}

/// @brief function JSON_PARSE
AqlValue Functions::JsonParse(ExpressionContext* expressionContext,
                              AstNode const&,
                              VPackFunctionParametersView parameters) {
  static char const* AFN = "JSON_PARSE";

  transaction::Methods* trx = &expressionContext->trx();
  auto* vopts = &trx->vpackOptions();
  AqlValue const& value = extractFunctionParameterValue(parameters, 0);
  AqlValueMaterializer materializer(vopts);
  VPackSlice slice = materializer.slice(value, false);

  if (!slice.isString()) {
    registerWarning(expressionContext, AFN,
                    TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH);
    return AqlValue(AqlValueHintNull());
  }

  VPackValueLength l;
  char const* p = slice.getStringUnchecked(l);

  try {
    std::shared_ptr<VPackBuilder> builder = VPackParser::fromJson(p, l);
    return AqlValue(builder->slice(), builder->size());
  } catch (...) {
    registerWarning(expressionContext, AFN,
                    TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH);
    return AqlValue(AqlValueHintNull());
  }
}

/// @brief function PARSE_IDENTIFIER
AqlValue Functions::ParseIdentifier(ExpressionContext* expressionContext,
                                    AstNode const&,
                                    VPackFunctionParametersView parameters) {
  static char const* AFN = "PARSE_IDENTIFIER";

  transaction::Methods* trx = &expressionContext->trx();
  AqlValue const& value = extractFunctionParameterValue(parameters, 0);
  std::string identifier;
  if (value.isObject() && value.hasKey(StaticStrings::IdString)) {
    auto resolver = trx->resolver();
    TRI_ASSERT(resolver != nullptr);
    bool localMustDestroy;
    AqlValue valueStr =
        value.get(*resolver, StaticStrings::IdString, localMustDestroy, false);
    AqlValueGuard guard(valueStr, localMustDestroy);

    if (valueStr.isString()) {
      identifier = valueStr.slice().copyString();
    }
  } else if (value.isString()) {
    identifier = value.slice().copyString();
  }

  if (identifier.empty()) {
    registerWarning(expressionContext, AFN,
                    TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH);
    return AqlValue(AqlValueHintNull());
  }

  size_t pos = identifier.find('/');
  if (pos == std::string::npos ||
      identifier.find('/', pos + 1) != std::string::npos) {
    registerWarning(expressionContext, AFN,
                    TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH);
    return AqlValue(AqlValueHintNull());
  }

  transaction::BuilderLeaser builder(trx);
  builder->openObject();
  builder->add("collection",
               VPackValuePair(identifier.data(), pos, VPackValueType::String));
  builder->add("key", VPackValuePair(identifier.data() + pos + 1,
                                     identifier.size() - pos - 1,
                                     VPackValueType::String));
  builder->close();
  return AqlValue(builder->slice(), builder->size());
}

/// @brief function Slice
AqlValue Functions::Slice(ExpressionContext* expressionContext, AstNode const&,
                          VPackFunctionParametersView parameters) {
  // cppcheck-suppress variableScope
  static char const* AFN = "SLICE";

  transaction::Methods* trx = &expressionContext->trx();
  auto* vopts = &trx->vpackOptions();
  AqlValue const& baseArray = extractFunctionParameterValue(parameters, 0);

  if (!baseArray.isArray()) {
    registerWarning(expressionContext, AFN,
                    TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH);
    return AqlValue(AqlValueHintNull());
  }

  // determine lower bound
  AqlValue fromValue = extractFunctionParameterValue(parameters, 1);
  int64_t from = fromValue.toInt64();
  if (from < 0) {
    from = baseArray.length() + from;
    if (from < 0) {
      from = 0;
    }
  }

  // determine upper bound
  AqlValue const& toValue = extractFunctionParameterValue(parameters, 2);
  int64_t to;
  if (toValue.isNull(true)) {
    to = baseArray.length();
  } else {
    to = toValue.toInt64();
    if (to >= 0) {
      to += from;
    } else {
      // negative to value
      to = baseArray.length() + to;
      if (to < 0) {
        to = 0;
      }
    }
  }

  AqlValueMaterializer materializer(vopts);
  VPackSlice arraySlice = materializer.slice(baseArray, false);

  transaction::BuilderLeaser builder(trx);
  builder->openArray();

  int64_t pos = 0;
  VPackArrayIterator it(arraySlice);
  while (it.valid()) {
    if (pos >= from && pos < to) {
      builder->add(it.value());
    }
    ++pos;
    if (pos >= to) {
      // done
      break;
    }
    it.next();
  }

  builder->close();
  return AqlValue(builder->slice(), builder->size());
}

/// @brief function Minus
AqlValue Functions::Minus(ExpressionContext* expressionContext, AstNode const&,
                          VPackFunctionParametersView parameters) {
  static char const* AFN = "MINUS";

  transaction::Methods* trx = &expressionContext->trx();
  auto* vopts = &trx->vpackOptions();
  AqlValue const& baseArray = extractFunctionParameterValue(parameters, 0);

  if (!baseArray.isArray()) {
    registerWarning(expressionContext, AFN,
                    TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH);
    return AqlValue(AqlValueHintNull());
  }

  auto options = trx->transactionContextPtr()->getVPackOptions();
  std::unordered_map<VPackSlice, size_t, basics::VelocyPackHelper::VPackHash,
                     basics::VelocyPackHelper::VPackEqual>
      contains(512, basics::VelocyPackHelper::VPackHash(),
               basics::VelocyPackHelper::VPackEqual(options));

  // Fill the original map
  AqlValueMaterializer materializer(vopts);
  VPackSlice arraySlice = materializer.slice(baseArray, false);

  VPackArrayIterator it(arraySlice);
  while (it.valid()) {
    contains.try_emplace(it.value(), it.index());
    it.next();
  }

  // Iterate through all following parameters and delete found elements from the
  // map
  for (size_t k = 1; k < parameters.size(); ++k) {
    AqlValue const& next = extractFunctionParameterValue(parameters, k);
    if (!next.isArray()) {
      registerWarning(expressionContext, AFN,
                      TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH);
      return AqlValue(AqlValueHintNull());
    }

    AqlValueMaterializer materializer(vopts);
    VPackSlice arraySlice = materializer.slice(next, false);

    for (VPackSlice search : VPackArrayIterator(arraySlice)) {
      auto find = contains.find(search);

      if (find != contains.end()) {
        contains.erase(find);
      }
    }
  }

  // We omit the normalize part from js, cannot occur here
  transaction::BuilderLeaser builder(trx);
  builder->openArray();
  for (auto const& it : contains) {
    builder->add(it.first);
  }
  builder->close();
  return AqlValue(builder->slice(), builder->size());
}

/// @brief function Document
AqlValue Functions::Document(ExpressionContext* expressionContext,
                             AstNode const&,
                             VPackFunctionParametersView parameters) {
  // cppcheck-suppress variableScope
  static char const* AFN = "DOCUMENT";

  OperationOptions options;
  options.documentCallFromAql = true;

  transaction::Methods* trx = &expressionContext->trx();
  auto* vopts = &trx->vpackOptions();
  if (parameters.size() == 1) {
    AqlValue const& id = extractFunctionParameterValue(parameters, 0);
    transaction::BuilderLeaser builder(trx);
    if (id.isString()) {
      std::string identifier(id.slice().copyString());
      std::string colName;
      ::getDocumentByIdentifier(trx, options, colName, identifier, true,
                                *builder.get());
      if (builder->isEmpty()) {
        // not found
        return AqlValue(AqlValueHintNull());
      }
      return AqlValue(builder->slice(), builder->size());
    }
    if (id.isArray()) {
      AqlValueMaterializer materializer(vopts);
      VPackSlice idSlice = materializer.slice(id, false);
      builder->openArray();
      for (auto const& next : VPackArrayIterator(idSlice)) {
        if (next.isString()) {
          std::string identifier = next.copyString();
          std::string colName;
          ::getDocumentByIdentifier(trx, options, colName, identifier, true,
                                    *builder.get());
        }
      }
      builder->close();
      return AqlValue(builder->slice(), builder->size());
    }
    return AqlValue(AqlValueHintNull());
  }

  AqlValue const& collectionValue =
      extractFunctionParameterValue(parameters, 0);
  if (!collectionValue.isString()) {
    registerWarning(expressionContext, AFN,
                    TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH);
    return AqlValue(AqlValueHintNull());
  }
  std::string collectionName(collectionValue.slice().copyString());

  AqlValue const& id = extractFunctionParameterValue(parameters, 1);
  if (id.isString()) {
    transaction::BuilderLeaser builder(trx);
    std::string identifier(id.slice().copyString());
    ::getDocumentByIdentifier(trx, options, collectionName, identifier, true,
                              *builder.get());
    if (builder->isEmpty()) {
      return AqlValue(AqlValueHintNull());
    }
    return AqlValue(builder->slice(), builder->size());
  }

  if (id.isArray()) {
    transaction::BuilderLeaser builder(trx);
    builder->openArray();

    AqlValueMaterializer materializer(vopts);
    VPackSlice idSlice = materializer.slice(id, false);
    for (auto const& next : VPackArrayIterator(idSlice)) {
      if (next.isString()) {
        std::string identifier(next.copyString());
        ::getDocumentByIdentifier(trx, options, collectionName, identifier,
                                  true, *builder.get());
      }
    }

    builder->close();
    return AqlValue(builder->slice(), builder->size());
  }

  // Id has invalid format
  return AqlValue(AqlValueHintNull());
}

/// @brief function MATCHES
AqlValue Functions::Matches(ExpressionContext* expressionContext,
                            AstNode const&,
                            VPackFunctionParametersView parameters) {
  static char const* AFN = "MATCHES";

  transaction::Methods* trx = &expressionContext->trx();
  auto* vopts = &trx->vpackOptions();
  AqlValue const& docToFind = extractFunctionParameterValue(parameters, 0);

  if (!docToFind.isObject()) {
    return AqlValue(AqlValueHintBool(false));
  }

  AqlValue const& exampleDocs = extractFunctionParameterValue(parameters, 1);

  bool retIdx = false;
  if (parameters.size() == 3) {
    retIdx = extractFunctionParameterValue(parameters, 2).toBoolean();
  }

  AqlValueMaterializer materializer(vopts);
  VPackSlice const docSlice = materializer.slice(docToFind, true);

  TRI_ASSERT(docSlice.isObject());

  transaction::BuilderLeaser builder(trx);
  AqlValueMaterializer exampleMaterializer(vopts);
  VPackSlice examples = exampleMaterializer.slice(exampleDocs, false);

  if (!examples.isArray()) {
    builder->openArray();
    builder->add(examples);
    builder->close();
    examples = builder->slice();
  }

  auto options = trx->transactionContextPtr()->getVPackOptions();

  bool foundMatch;
  int32_t idx = -1;

  for (auto const& example : VPackArrayIterator(examples)) {
    idx++;

    if (!example.isObject()) {
      registerWarning(expressionContext, AFN,
                      TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH);
      continue;
    }

    foundMatch = true;

    TRI_ASSERT(example.isObject());
    TRI_ASSERT(docSlice.isObject());
    for (auto it : VPackObjectIterator(example, true)) {
      VPackSlice keySlice = docSlice.get(it.key.stringView());

      if (it.value.isNull() && keySlice.isNone()) {
        continue;
      }

      if (keySlice.isNone() ||
          // compare inner content
          !basics::VelocyPackHelper::equal(keySlice, it.value, false, options,
                                           &docSlice, &example)) {
        foundMatch = false;
        break;
      }
    }

    if (foundMatch) {
      if (retIdx) {
        return AqlValue(AqlValueHintInt(idx));
      } else {
        return AqlValue(AqlValueHintBool(true));
      }
    }
  }

  if (retIdx) {
    return AqlValue(AqlValueHintInt(-1));
  }

  return AqlValue(AqlValueHintBool(false));
}

/// @brief function ROUND
AqlValue Functions::Round(ExpressionContext*, AstNode const&,
                          VPackFunctionParametersView parameters) {
  AqlValue const& value = extractFunctionParameterValue(parameters, 0);
  AqlValue const& precision = extractFunctionParameterValue(parameters, 1);

  double input = value.toDouble();
  double exp = std::pow(10, precision.toDouble() >= 0 ? precision.toDouble() : 0);

  // Rounds down for < x.4999 and up for > x.50000
  return ::numberValue(std::floor((input * exp) + 0.5) / exp, true);
}

/// @brief function ABS
AqlValue Functions::Abs(ExpressionContext*, AstNode const&,
                        VPackFunctionParametersView parameters) {
  AqlValue const& value = extractFunctionParameterValue(parameters, 0);

  double input = value.toDouble();
  return ::numberValue(std::abs(input), true);
}

/// @brief function CEIL
AqlValue Functions::Ceil(ExpressionContext*, AstNode const&,
                         VPackFunctionParametersView parameters) {
  AqlValue const& value = extractFunctionParameterValue(parameters, 0);

  double input = value.toDouble();
  return ::numberValue(std::ceil(input), true);
}

/// @brief function FLOOR
AqlValue Functions::Floor(ExpressionContext*, AstNode const&,
                          VPackFunctionParametersView parameters) {
  AqlValue const& value = extractFunctionParameterValue(parameters, 0);

  double input = value.toDouble();
  return ::numberValue(std::floor(input), true);
}

/// @brief function SQRT
AqlValue Functions::Sqrt(ExpressionContext*, AstNode const&,
                         VPackFunctionParametersView parameters) {
  AqlValue const& value = extractFunctionParameterValue(parameters, 0);

  double input = value.toDouble();
  return ::numberValue(std::sqrt(input), true);
}

/// @brief function POW
AqlValue Functions::Pow(ExpressionContext*, AstNode const&,
                        VPackFunctionParametersView parameters) {
  AqlValue const& baseValue = extractFunctionParameterValue(parameters, 0);
  AqlValue const& expValue = extractFunctionParameterValue(parameters, 1);

  double base = baseValue.toDouble();
  double exp = expValue.toDouble();

  return ::numberValue(std::pow(base, exp), true);
}

/// @brief function LOG
AqlValue Functions::Log(ExpressionContext*, AstNode const&,
                        VPackFunctionParametersView parameters) {
  AqlValue const& value = extractFunctionParameterValue(parameters, 0);

  double input = value.toDouble();
  return ::numberValue(std::log(input), true);
}

/// @brief function LOG2
AqlValue Functions::Log2(ExpressionContext*, AstNode const&,
                         VPackFunctionParametersView parameters) {
  AqlValue const& value = extractFunctionParameterValue(parameters, 0);

  double input = value.toDouble();
  return ::numberValue(std::log2(input), true);
}

/// @brief function LOG10
AqlValue Functions::Log10(ExpressionContext*, AstNode const&,
                          VPackFunctionParametersView parameters) {
  AqlValue const& value = extractFunctionParameterValue(parameters, 0);

  double input = value.toDouble();
  return ::numberValue(std::log10(input), true);
}

/// @brief function EXP
AqlValue Functions::Exp(ExpressionContext*, AstNode const&,
                        VPackFunctionParametersView parameters) {
  AqlValue const& value = extractFunctionParameterValue(parameters, 0);

  double input = value.toDouble();
  return ::numberValue(std::exp(input), true);
}

/// @brief function EXP2
AqlValue Functions::Exp2(ExpressionContext*, AstNode const&,
                         VPackFunctionParametersView parameters) {
  AqlValue const& value = extractFunctionParameterValue(parameters, 0);

  double input = value.toDouble();
  return ::numberValue(std::exp2(input), true);
}

/// @brief function SIN
AqlValue Functions::Sin(ExpressionContext*, AstNode const&,
                        VPackFunctionParametersView parameters) {
  AqlValue const& value = extractFunctionParameterValue(parameters, 0);

  double input = value.toDouble();
  return ::numberValue(std::sin(input), true);
}

/// @brief function COS
AqlValue Functions::Cos(ExpressionContext*, AstNode const&,
                        VPackFunctionParametersView parameters) {
  AqlValue const& value = extractFunctionParameterValue(parameters, 0);

  double input = value.toDouble();
  return ::numberValue(std::cos(input), true);
}

/// @brief function TAN
AqlValue Functions::Tan(ExpressionContext*, AstNode const&,
                        VPackFunctionParametersView parameters) {
  AqlValue const& value = extractFunctionParameterValue(parameters, 0);

  double input = value.toDouble();
  return ::numberValue(std::tan(input), true);
}

/// @brief function ASIN
AqlValue Functions::Asin(ExpressionContext*, AstNode const&,
                         VPackFunctionParametersView parameters) {
  AqlValue const& value = extractFunctionParameterValue(parameters, 0);

  double input = value.toDouble();
  return ::numberValue(std::asin(input), true);
}

/// @brief function ACOS
AqlValue Functions::Acos(ExpressionContext*, AstNode const&,
                         VPackFunctionParametersView parameters) {
  AqlValue const& value = extractFunctionParameterValue(parameters, 0);

  double input = value.toDouble();
  return ::numberValue(std::acos(input), true);
}

/// @brief function ATAN
AqlValue Functions::Atan(ExpressionContext*, AstNode const&,
                         VPackFunctionParametersView parameters) {
  AqlValue const& value = extractFunctionParameterValue(parameters, 0);

  double input = value.toDouble();
  return ::numberValue(std::atan(input), true);
}

/// @brief function ATAN2
AqlValue Functions::Atan2(ExpressionContext*, AstNode const&,
                          VPackFunctionParametersView parameters) {
  AqlValue value1 = extractFunctionParameterValue(parameters, 0);
  AqlValue value2 = extractFunctionParameterValue(parameters, 1);

  double input1 = value1.toDouble();
  double input2 = value2.toDouble();
  return ::numberValue(std::atan2(input1, input2), true);
}

/// @brief function RADIANS
AqlValue Functions::Radians(ExpressionContext*, AstNode const&,
                            VPackFunctionParametersView parameters) {
  AqlValue const& value = extractFunctionParameterValue(parameters, 0);

  double degrees = value.toDouble();
  // acos(-1) == PI
  return ::numberValue(degrees * (std::acos(-1.0) / 180.0), true);
}

/// @brief function DEGREES
AqlValue Functions::Degrees(ExpressionContext*, AstNode const&,
                            VPackFunctionParametersView parameters) {
  AqlValue const& value = extractFunctionParameterValue(parameters, 0);

  double radians = value.toDouble();
  // acos(-1) == PI
  return ::numberValue(radians * (180.0 / std::acos(-1.0)), true);
}

/// @brief function PI
AqlValue Functions::Pi(ExpressionContext*, AstNode const&,
                       VPackFunctionParametersView parameters) {
  // acos(-1) == PI
  return ::numberValue(std::acos(-1.0), true);
}

template<typename T>
std::optional<T> bitOperationValue(VPackSlice input) {
  if (input.isNumber() && input.getNumber<double>() >= 0) {
    uint64_t result = input.getNumber<uint64_t>();
    if (result <=
        static_cast<uint64_t>(Functions::bitFunctionsMaxSupportedValue)) {
      TRI_ASSERT(result <= UINT32_MAX);
      // value is valid
      return std::optional<T>{static_cast<T>(result)};
    }
  }
  // value is not valid
  return {};
}

AqlValue handleBitOperation(
    ExpressionContext* expressionContext, AstNode const& node,
    VPackFunctionParametersView parameters,
    std::function<uint64_t(uint64_t, uint64_t)> const& cb) {
  TRI_ASSERT(aql::NODE_TYPE_FCALL == node.type);

  // extract AQL function name
  auto const* impl = static_cast<aql::Function*>(node.getData());
  TRI_ASSERT(impl != nullptr);

  AqlValue const& value = extractFunctionParameterValue(parameters, 0);

  if (parameters.size() == 2) {
    // expect two numbers as individual parameters
    if (value.isNumber()) {
      auto result1 = bitOperationValue<uint64_t>(value.slice());
      if (result1.has_value()) {
        AqlValue const& value2 = extractFunctionParameterValue(parameters, 1);
        if (value2.isNumber()) {
          auto result2 = bitOperationValue<uint64_t>(value2.slice());
          if (result2.has_value()) {
            uint64_t result = cb(result1.value(), result2.value());
            return AqlValue(AqlValueHintUInt(result));
          }
        }
      }
    }
    registerInvalidArgumentWarning(expressionContext, impl->name.c_str());
    return AqlValue(AqlValueHintNull());
  }

  // expect an array of numbers (with null values being ignored)
  if (!value.isArray()) {
    registerInvalidArgumentWarning(expressionContext, impl->name.c_str());
    return AqlValue(AqlValueHintNull());
  }

  bool first = true;
  uint32_t result = 0;

  transaction::Methods* trx = &expressionContext->trx();
  auto* vopts = &trx->vpackOptions();
  AqlValueMaterializer materializer(vopts);
  VPackSlice s = materializer.slice(value, false);
  for (VPackSlice v : VPackArrayIterator(s)) {
    // skip null values in the input
    if (v.isNull()) {
      continue;
    }

    auto currentValue = bitOperationValue<uint64_t>(v);
    if (!currentValue.has_value()) {
      registerInvalidArgumentWarning(expressionContext, impl->name.c_str());
      return AqlValue(AqlValueHintNull());
    }
    if (first) {
      first = false;
      result = currentValue.value();
    } else {
      result = cb(result, currentValue.value());
    }
  }

  if (first) {
    return AqlValue(AqlValueHintNull());
  }

  return AqlValue(AqlValueHintUInt(result));
}

/// @brief function BIT_AND
AqlValue Functions::BitAnd(ExpressionContext* expressionContext,
                           AstNode const& node,
                           VPackFunctionParametersView parameters) {
  return handleBitOperation(
      expressionContext, node, parameters,
      [](uint64_t value1, uint64_t value2) { return value1 & value2; });
}

/// @brief function BIT_OR
AqlValue Functions::BitOr(ExpressionContext* expressionContext,
                          AstNode const& node,
                          VPackFunctionParametersView parameters) {
  return handleBitOperation(
      expressionContext, node, parameters,
      [](uint64_t value1, uint64_t value2) { return value1 | value2; });
}

/// @brief function BIT_XOR
AqlValue Functions::BitXOr(ExpressionContext* expressionContext,
                           AstNode const& node,
                           VPackFunctionParametersView parameters) {
  return handleBitOperation(
      expressionContext, node, parameters,
      [](uint64_t value1, uint64_t value2) { return value1 ^ value2; });
}

template<typename T1, typename T2>
std::optional<std::pair<T1, T2>> binaryBitFunctionParameters(
    VPackFunctionParametersView parameters) {
  AqlValue const& value1 = extractFunctionParameterValue(parameters, 0);
  if (value1.isNumber()) {
    auto result1 = bitOperationValue<T1>(value1.slice());
    if (result1.has_value()) {
      AqlValue const& value2 = extractFunctionParameterValue(parameters, 1);
      if (value2.isNumber()) {
        auto result2 = bitOperationValue<T2>(value2.slice());
        if (result2.has_value() &&
            result2.value() <=
                static_cast<T2>(Functions::bitFunctionsMaxSupportedBits)) {
          return std::optional<std::pair<T1, T2>>{
              {result1.value(), result2.value()}};
        }
      }
    }
  }

  return {};
}

/// @brief function BIT_NEGATE
AqlValue Functions::BitNegate(ExpressionContext* expressionContext,
                              AstNode const& node,
                              VPackFunctionParametersView parameters) {
  auto result = binaryBitFunctionParameters<uint64_t, uint64_t>(parameters);
  if (result.has_value()) {
    auto [testee, width] = result.value();
    // mask the lower bits of the result with up to 32 active bits
    uint64_t value = (~testee) & ((uint64_t(1) << width) - 1);
    return AqlValue(AqlValueHintUInt(value));
  }

  static char const* AFN = "BIT_NEGATE";
  registerInvalidArgumentWarning(expressionContext, AFN);
  return AqlValue(AqlValueHintNull());
}

/// @brief function BIT_TEST
AqlValue Functions::BitTest(ExpressionContext* expressionContext,
                            AstNode const& node,
                            VPackFunctionParametersView parameters) {
  auto result = binaryBitFunctionParameters<uint64_t, uint64_t>(parameters);
  if (result.has_value()) {
    auto [testee, index] = result.value();
    if (index < Functions::bitFunctionsMaxSupportedBits) {
      return AqlValue(AqlValueHintBool((testee & (uint64_t(1) << index)) != 0));
    }
  }

  static char const* AFN = "BIT_TEST";
  registerInvalidArgumentWarning(expressionContext, AFN);
  return AqlValue(AqlValueHintNull());
}

/// @brief function BIT_SHIFT_LEFT
AqlValue Functions::BitShiftLeft(ExpressionContext* expressionContext,
                                 AstNode const& node,
                                 VPackFunctionParametersView parameters) {
  auto result = binaryBitFunctionParameters<uint64_t, uint64_t>(parameters);
  if (result.has_value()) {
    auto [testee, shift] = result.value();
    AqlValue const& value = extractFunctionParameterValue(parameters, 2);
    if (value.isNumber()) {
      auto width = bitOperationValue<uint64_t>(value.slice());
      if (width.has_value() &&
          width.value() <=
              static_cast<uint64_t>(Functions::bitFunctionsMaxSupportedBits)) {
        // mask the lower bits of the result with up to 32 active bits
        return AqlValue(AqlValueHintUInt((testee << shift) &
                                         ((uint64_t(1) << width.value()) - 1)));
      }
    }
  }

  static char const* AFN = "BIT_SHIFT_LEFT";
  registerInvalidArgumentWarning(expressionContext, AFN);
  return AqlValue(AqlValueHintNull());
}

/// @brief function BIT_SHIFT_RIGHT
AqlValue Functions::BitShiftRight(ExpressionContext* expressionContext,
                                  AstNode const& node,
                                  VPackFunctionParametersView parameters) {
  auto result = binaryBitFunctionParameters<uint64_t, uint64_t>(parameters);
  if (result.has_value()) {
    auto [testee, shift] = result.value();
    AqlValue const& value = extractFunctionParameterValue(parameters, 2);
    if (value.isNumber()) {
      auto width = bitOperationValue<uint32_t>(value.slice());
      if (width.has_value() &&
          width.value() <=
              static_cast<uint64_t>(Functions::bitFunctionsMaxSupportedBits)) {
        // mask the lower bits of the result with up to 32 active bits
        return AqlValue(AqlValueHintUInt((testee >> shift) &
                                         ((uint64_t(1) << width.value()) - 1)));
      }
    }
  }

  static char const* AFN = "BIT_SHIFT_RIGHT";
  registerInvalidArgumentWarning(expressionContext, AFN);
  return AqlValue(AqlValueHintNull());
}

/// @brief function BIT_POPCOUNT
AqlValue Functions::BitPopcount(ExpressionContext* expressionContext,
                                AstNode const& node,
                                VPackFunctionParametersView parameters) {
  AqlValue const& value = extractFunctionParameterValue(parameters, 0);

  if (value.isNumber()) {
    VPackSlice v = value.slice();
    auto result = bitOperationValue<uint64_t>(v);
    if (result.has_value()) {
      uint64_t x = result.value();
      size_t const count = std::popcount(x);
      return AqlValue(AqlValueHintUInt(count));
    }
  }

  static char const* AFN = "BIT_POPCOUNT";
  registerInvalidArgumentWarning(expressionContext, AFN);
  return AqlValue(AqlValueHintNull());
}

AqlValue Functions::BitConstruct(ExpressionContext* expressionContext,
                                 AstNode const& node,
                                 VPackFunctionParametersView parameters) {
  static char const* AFN = "BIT_CONSTRUCT";
  AqlValue const& value = extractFunctionParameterValue(parameters, 0);

  if (value.isArray()) {
    transaction::Methods* trx = &expressionContext->trx();
    auto* vopts = &trx->vpackOptions();
    AqlValueMaterializer materializer(vopts);
    VPackSlice s = materializer.slice(value, false);

    uint64_t result = 0;
    for (VPackSlice v : VPackArrayIterator(s)) {
      auto currentValue = bitOperationValue<uint64_t>(v);
      bool valid = currentValue.has_value();
      if (valid) {
        if (currentValue.value() >= Functions::bitFunctionsMaxSupportedBits) {
          valid = false;
        }
      }
      if (!valid) {
        registerInvalidArgumentWarning(expressionContext, AFN);
        return AqlValue(AqlValueHintNull());
      }

      result |= uint64_t(1) << currentValue.value();
    }

    return AqlValue(AqlValueHintUInt(result));
  }

  registerInvalidArgumentWarning(expressionContext, AFN);
  return AqlValue(AqlValueHintNull());
}

AqlValue Functions::BitDeconstruct(ExpressionContext* expressionContext,
                                   AstNode const& node,
                                   VPackFunctionParametersView parameters) {
  AqlValue const& value = extractFunctionParameterValue(parameters, 0);

  if (value.isNumber()) {
    VPackSlice v = value.slice();
    auto num = bitOperationValue<uint64_t>(v);
    if (num.has_value()) {
      transaction::Methods* trx = &expressionContext->trx();
      transaction::BuilderLeaser builder(trx);
      builder->openArray();
      uint32_t compare = 1;
      for (uint32_t i = 0; i < Functions::bitFunctionsMaxSupportedBits; ++i) {
        // potential improvement: use C++20's std::countr_zero()
        if ((num.value() & compare) != 0) {
          builder->add(VPackValue(i));
        }
        compare <<= 1;
      }
      builder->close();
      return AqlValue(builder->slice(), builder->size());
    }
  }

  char const* AFN = "BIT_DECONSTRUCT";
  registerInvalidArgumentWarning(expressionContext, AFN);
  return AqlValue(AqlValueHintNull());
}

/// @brief function BIT_TO_STRING
AqlValue Functions::BitToString(ExpressionContext* expressionContext,
                                AstNode const& node,
                                VPackFunctionParametersView parameters) {
  auto result = binaryBitFunctionParameters<uint64_t, uint64_t>(parameters);
  if (result.has_value()) {
    auto [testee, index] = result.value();
    TRI_ASSERT(index <= Functions::bitFunctionsMaxSupportedBits);

    char buffer[Functions::bitFunctionsMaxSupportedBits];
    char* p = &buffer[0];

    if (index > 0) {
      uint64_t compare = uint64_t(1) << (index - 1);
      while (compare > 0) {
        *p = (testee & compare) ? '1' : '0';
        ++p;
        compare >>= 1;
      }
    }

    return AqlValue(&buffer[0], static_cast<size_t>(p - &buffer[0]));
  }

  static char const* AFN = "BIT_TO_STRING";
  registerInvalidArgumentWarning(expressionContext, AFN);
  return AqlValue(AqlValueHintNull());
}

/// @brief function BIT_FROM_STRING
AqlValue Functions::BitFromString(ExpressionContext* expressionContext,
                                  AstNode const& node,
                                  VPackFunctionParametersView parameters) {
  static char const* AFN = "BIT_FROM_STRING";

  AqlValue const& value = extractFunctionParameterValue(parameters, 0);
  if (value.isString()) {
    std::string_view v = value.slice().stringView();
    char const* p = v.data();
    char const* e = p + v.size();

    if (static_cast<uint64_t>(e - p) <=
        Functions::bitFunctionsMaxSupportedBits) {
      uint64_t result = 0;
      while (p != e) {
        char c = *p;
        if (c == '1') {
          /* only the 1s are interesting for us */
          result += (static_cast<uint64_t>(1) << (e - p - 1));
        } else if (c != '0') {
          // invalid input. abort
          registerInvalidArgumentWarning(expressionContext, AFN);
          return AqlValue(AqlValueHintNull());
        }
        ++p;
      }
      return AqlValue(AqlValueHintUInt(result));
    }
  }

  registerInvalidArgumentWarning(expressionContext, AFN);
  return AqlValue(AqlValueHintNull());
}

/// @brief function RAND
AqlValue Functions::Rand(ExpressionContext*, AstNode const&,
                         VPackFunctionParametersView parameters) {
  // This random functionality is not too good yet...
  return ::numberValue(static_cast<double>(std::rand()) / RAND_MAX, true);
}

/// @brief function FIRST_DOCUMENT
AqlValue Functions::FirstDocument(ExpressionContext*, AstNode const&,
                                  VPackFunctionParametersView parameters) {
  size_t const n = parameters.size();
  for (size_t i = 0; i < n; ++i) {
    AqlValue const& a = extractFunctionParameterValue(parameters, i);
    if (a.isObject()) {
      return a.clone();
    }
  }

  return AqlValue(AqlValueHintNull());
}

/// @brief function FIRST_LIST
AqlValue Functions::FirstList(ExpressionContext*, AstNode const&,
                              VPackFunctionParametersView parameters) {
  size_t const n = parameters.size();
  for (size_t i = 0; i < n; ++i) {
    AqlValue const& a = extractFunctionParameterValue(parameters, i);
    if (a.isArray()) {
      return a.clone();
    }
  }

  return AqlValue(AqlValueHintNull());
}

/// @brief function PUSH
AqlValue Functions::Push(ExpressionContext* expressionContext, AstNode const&,
                         VPackFunctionParametersView parameters) {
  // cppcheck-suppress variableScope
  static char const* AFN = "PUSH";

  transaction::Methods* trx = &expressionContext->trx();
  auto* vopts = &trx->vpackOptions();
  AqlValue const& list = extractFunctionParameterValue(parameters, 0);
  AqlValue const& toPush = extractFunctionParameterValue(parameters, 1);

  AqlValueMaterializer toPushMaterializer(vopts);
  VPackSlice p = toPushMaterializer.slice(toPush, false);

  if (list.isNull(true)) {
    transaction::BuilderLeaser builder(trx);
    builder->openArray();
    builder->add(p);
    builder->close();
    return AqlValue(builder->slice(), builder->size());
  }

  if (!list.isArray()) {
    registerInvalidArgumentWarning(expressionContext, AFN);
    return AqlValue(AqlValueHintNull());
  }

  transaction::BuilderLeaser builder(trx);
  builder->openArray();
  AqlValueMaterializer materializer(vopts);
  VPackSlice l = materializer.slice(list, false);

  for (VPackSlice it : VPackArrayIterator(l)) {
    builder->add(it);
  }
  if (parameters.size() == 3) {
    auto options = trx->transactionContextPtr()->getVPackOptions();
    AqlValue const& unique = extractFunctionParameterValue(parameters, 2);
    if (!unique.toBoolean() || !::listContainsElement(options, l, p)) {
      builder->add(p);
    }
  } else {
    builder->add(p);
  }
  builder->close();
  return AqlValue(builder->slice(), builder->size());
}

/// @brief function POP
AqlValue Functions::Pop(ExpressionContext* expressionContext, AstNode const&,
                        VPackFunctionParametersView parameters) {
  // cppcheck-suppress variableScope
  static char const* AFN = "POP";

  transaction::Methods* trx = &expressionContext->trx();
  auto* vopts = &trx->vpackOptions();
  AqlValue const& list = extractFunctionParameterValue(parameters, 0);

  if (list.isNull(true)) {
    return AqlValue(AqlValueHintNull());
  }

  if (!list.isArray()) {
    registerWarning(expressionContext, AFN,
                    TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH);
    return AqlValue(AqlValueHintNull());
  }

  AqlValueMaterializer materializer(vopts);
  VPackSlice slice = materializer.slice(list, false);

  transaction::BuilderLeaser builder(trx);
  builder->openArray();
  auto iterator = VPackArrayIterator(slice);
  while (iterator.valid() && !iterator.isLast()) {
    builder->add(iterator.value());
    iterator.next();
  }
  builder->close();
  return AqlValue(builder->slice(), builder->size());
}

/// @brief function APPEND
AqlValue Functions::Append(ExpressionContext* expressionContext, AstNode const&,
                           VPackFunctionParametersView parameters) {
  // cppcheck-suppress variableScope
  static char const* AFN = "APPEND";

  transaction::Methods* trx = &expressionContext->trx();
  auto* vopts = &trx->vpackOptions();
  AqlValue const& list = extractFunctionParameterValue(parameters, 0);
  AqlValue const& toAppend = extractFunctionParameterValue(parameters, 1);

  if (toAppend.isNull(true)) {
    return list.clone();
  }

  AqlValueMaterializer toAppendMaterializer(vopts);
  VPackSlice t = toAppendMaterializer.slice(toAppend, false);

  if (t.isArray() && t.length() == 0) {
    return list.clone();
  }

  bool unique = false;
  if (parameters.size() == 3) {
    AqlValue const& a = extractFunctionParameterValue(parameters, 2);
    unique = a.toBoolean();
  }

  AqlValueMaterializer materializer(vopts);
  VPackSlice l = materializer.slice(list, false);

  if (l.isNull()) {
    return toAppend.clone();
  }

  if (!l.isArray()) {
    registerInvalidArgumentWarning(expressionContext, AFN);
    return AqlValue(AqlValueHintNull());
  }

  auto options = trx->transactionContextPtr()->getVPackOptions();
  containers::FlatHashSet<VPackSlice, basics::VelocyPackHelper::VPackHash,
                          basics::VelocyPackHelper::VPackEqual>
      added(11, basics::VelocyPackHelper::VPackHash(),
            basics::VelocyPackHelper::VPackEqual(options));

  transaction::BuilderLeaser builder(trx);
  builder->openArray();

  for (VPackSlice it : VPackArrayIterator(l)) {
    if (!unique || added.insert(it).second) {
      builder->add(it);
    }
  }

  AqlValueMaterializer materializer2(vopts);
  VPackSlice slice = materializer2.slice(toAppend, false);

  if (!slice.isArray()) {
    if (!unique || added.find(slice) == added.end()) {
      builder->add(slice);
    }
  } else {
    for (VPackSlice it : VPackArrayIterator(slice)) {
      if (!unique || added.insert(it).second) {
        builder->add(it);
      }
    }
  }
  builder->close();
  return AqlValue(builder->slice(), builder->size());
}

/// @brief function UNSHIFT
AqlValue Functions::Unshift(ExpressionContext* expressionContext,
                            AstNode const&,
                            VPackFunctionParametersView parameters) {
  // cppcheck-suppress variableScope
  static char const* AFN = "UNSHIFT";

  transaction::Methods* trx = &expressionContext->trx();
  auto* vopts = &trx->vpackOptions();
  AqlValue const& list = extractFunctionParameterValue(parameters, 0);

  if (!list.isNull(true) && !list.isArray()) {
    registerInvalidArgumentWarning(expressionContext, AFN);
    return AqlValue(AqlValueHintNull());
  }

  AqlValue const& toAppend = extractFunctionParameterValue(parameters, 1);
  bool unique = false;
  if (parameters.size() == 3) {
    AqlValue const& a = extractFunctionParameterValue(parameters, 2);
    unique = a.toBoolean();
  }

  size_t unused;
  if (unique && list.isArray() &&
      ::listContainsElement(vopts, list, toAppend, unused)) {
    // Short circuit, nothing to do return list
    return list.clone();
  }

  AqlValueMaterializer materializer(vopts);
  VPackSlice a = materializer.slice(toAppend, false);

  transaction::BuilderLeaser builder(trx);
  builder->openArray();
  builder->add(a);

  if (list.isArray()) {
    AqlValueMaterializer listMaterializer(vopts);
    VPackSlice v = listMaterializer.slice(list, false);
    for (VPackSlice it : VPackArrayIterator(v)) {
      builder->add(it);
    }
  }
  builder->close();
  return AqlValue(builder->slice(), builder->size());
}

/// @brief function SHIFT
AqlValue Functions::Shift(ExpressionContext* expressionContext, AstNode const&,
                          VPackFunctionParametersView parameters) {
  // cppcheck-suppress variableScope
  static char const* AFN = "SHIFT";

  transaction::Methods* trx = &expressionContext->trx();
  auto* vopts = &trx->vpackOptions();
  AqlValue const& list = extractFunctionParameterValue(parameters, 0);
  if (list.isNull(true)) {
    return AqlValue(AqlValueHintNull());
  }

  if (!list.isArray()) {
    registerInvalidArgumentWarning(expressionContext, AFN);
    return AqlValue(AqlValueHintNull());
  }

  transaction::BuilderLeaser builder(trx);
  builder->openArray();

  if (list.length() > 0) {
    AqlValueMaterializer materializer(vopts);
    VPackSlice l = materializer.slice(list, false);

    auto iterator = VPackArrayIterator(l);
    // This jumps over the first element
    iterator.next();
    while (iterator.valid()) {
      builder->add(iterator.value());
      iterator.next();
    }
  }
  builder->close();

  return AqlValue(builder->slice(), builder->size());
}

/// @brief function REMOVE_VALUE
AqlValue Functions::RemoveValue(ExpressionContext* expressionContext,
                                AstNode const&,
                                VPackFunctionParametersView parameters) {
  // cppcheck-suppress variableScope
  static char const* AFN = "REMOVE_VALUE";

  transaction::Methods* trx = &expressionContext->trx();
  auto* vopts = &trx->vpackOptions();
  AqlValue const& list = extractFunctionParameterValue(parameters, 0);

  if (list.isNull(true)) {
    return AqlValue(AqlValueHintEmptyArray());
  }

  if (!list.isArray()) {
    registerInvalidArgumentWarning(expressionContext, AFN);
    return AqlValue(AqlValueHintNull());
  }

  auto options = trx->transactionContextPtr()->getVPackOptions();

  transaction::BuilderLeaser builder(trx);
  builder->openArray();
  bool useLimit = false;
  int64_t limit = list.length();

  if (parameters.size() == 3) {
    AqlValue const& limitValue = extractFunctionParameterValue(parameters, 2);
    if (!limitValue.isNull(true)) {
      limit = limitValue.toInt64();
      useLimit = true;
    }
  }

  AqlValue const& toRemove = extractFunctionParameterValue(parameters, 1);
  AqlValueMaterializer toRemoveMaterializer(vopts);
  VPackSlice r = toRemoveMaterializer.slice(toRemove, false);

  AqlValueMaterializer materializer(vopts);
  VPackSlice v = materializer.slice(list, false);

  for (VPackSlice it : VPackArrayIterator(v)) {
    if (useLimit && limit == 0) {
      // Just copy
      builder->add(it);
      continue;
    }
    if (basics::VelocyPackHelper::equal(r, it, false, options)) {
      --limit;
      continue;
    }
    builder->add(it);
  }
  builder->close();
  return AqlValue(builder->slice(), builder->size());
}

/// @brief function REMOVE_VALUES
AqlValue Functions::RemoveValues(ExpressionContext* expressionContext,
                                 AstNode const&,
                                 VPackFunctionParametersView parameters) {
  // cppcheck-suppress variableScope
  static char const* AFN = "REMOVE_VALUES";

  transaction::Methods* trx = &expressionContext->trx();
  auto* vopts = &trx->vpackOptions();
  AqlValue const& list = extractFunctionParameterValue(parameters, 0);
  AqlValue const& values = extractFunctionParameterValue(parameters, 1);

  if (values.isNull(true)) {
    return list.clone();
  }

  if (list.isNull(true)) {
    return AqlValue(AqlValueHintEmptyArray());
  }

  if (!list.isArray() || !values.isArray()) {
    registerInvalidArgumentWarning(expressionContext, AFN);
    return AqlValue(AqlValueHintNull());
  }

  AqlValueMaterializer valuesMaterializer(vopts);
  VPackSlice v = valuesMaterializer.slice(values, false);

  AqlValueMaterializer listMaterializer(vopts);
  VPackSlice l = listMaterializer.slice(list, false);

  transaction::BuilderLeaser builder(trx);
  builder->openArray();
  for (VPackSlice it : VPackArrayIterator(l)) {
    if (!::listContainsElement(vopts, v, it)) {
      builder->add(it);
    }
  }
  builder->close();
  return AqlValue(builder->slice(), builder->size());
}

/// @brief function REMOVE_NTH
AqlValue Functions::RemoveNth(ExpressionContext* expressionContext,
                              AstNode const&,
                              VPackFunctionParametersView parameters) {
  // cppcheck-suppress variableScope
  static char const* AFN = "REMOVE_NTH";

  transaction::Methods* trx = &expressionContext->trx();
  auto* vopts = &trx->vpackOptions();
  AqlValue const& list = extractFunctionParameterValue(parameters, 0);

  if (list.isNull(true)) {
    return AqlValue(AqlValueHintEmptyArray());
  }

  if (!list.isArray()) {
    registerInvalidArgumentWarning(expressionContext, AFN);
    return AqlValue(AqlValueHintNull());
  }

  double const count = static_cast<double>(list.length());
  AqlValue const& position = extractFunctionParameterValue(parameters, 1);
  double p = position.toDouble();
  if (p >= count || p < -count) {
    // out of bounds
    return list.clone();
  }

  if (p < 0) {
    p += count;
  }

  AqlValueMaterializer materializer(vopts);
  VPackSlice v = materializer.slice(list, false);

  transaction::BuilderLeaser builder(trx);
  size_t target = static_cast<size_t>(p);
  size_t cur = 0;
  builder->openArray();
  for (VPackSlice it : VPackArrayIterator(v)) {
    if (cur != target) {
      builder->add(it);
    }
    cur++;
  }
  builder->close();
  return AqlValue(builder->slice(), builder->size());
}

/// @brief function ReplaceNth
AqlValue Functions::ReplaceNth(ExpressionContext* expressionContext,
                               AstNode const&,
                               VPackFunctionParametersView parameters) {
  // cppcheck-suppress variableScope
  static char const* AFN = "REPLACE_NTH";

  transaction::Methods* trx = &expressionContext->trx();
  auto* vopts = &trx->vpackOptions();
  AqlValue const& baseArray = extractFunctionParameterValue(parameters, 0);
  AqlValue const& offset = extractFunctionParameterValue(parameters, 1);
  AqlValue const& newValue = extractFunctionParameterValue(parameters, 2);
  AqlValue const& paddValue = extractFunctionParameterValue(parameters, 3);

  bool havePadValue = parameters.size() == 4;

  if (!baseArray.isArray()) {
    registerInvalidArgumentWarning(expressionContext, AFN);
    return AqlValue(AqlValueHintNull());
  }

  if (offset.isNull(true)) {
    THROW_ARANGO_EXCEPTION_PARAMS(
        TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH, AFN);
  }
  auto length = baseArray.length();
  uint64_t replaceOffset;
  int64_t posParam = offset.toInt64();
  if (posParam >= 0) {
    replaceOffset = static_cast<uint64_t>(posParam);
  } else {
    replaceOffset = (static_cast<int64_t>(length) + posParam < 0)
                        ? 0
                        : static_cast<uint64_t>(length + posParam);
  }

  if (length < replaceOffset && !havePadValue) {
    THROW_ARANGO_EXCEPTION_PARAMS(
        TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH, AFN);
  }

  AqlValueMaterializer materializer1(vopts);
  VPackSlice arraySlice = materializer1.slice(baseArray, false);
  AqlValueMaterializer materializer2(vopts);
  VPackSlice replaceValue = materializer2.slice(newValue, false);

  transaction::BuilderLeaser builder(trx);
  builder->openArray();

  VPackArrayIterator it(arraySlice);
  while (it.valid()) {
    if (it.index() != replaceOffset) {
      builder->add(it.value());
    } else {
      builder->add(replaceValue);
    }
    it.next();
  }

  uint64_t pos = length;
  if (replaceOffset >= length) {
    AqlValueMaterializer materializer(vopts);
    VPackSlice paddVpValue = materializer.slice(paddValue, false);
    while (pos < replaceOffset) {
      builder->add(paddVpValue);
      ++pos;
    }
    builder->add(replaceValue);
  }
  builder->close();
  return AqlValue(builder->slice(), builder->size());
}

/// @brief function NOT_NULL
AqlValue Functions::NotNull(ExpressionContext*, AstNode const&,
                            VPackFunctionParametersView parameters) {
  size_t const n = parameters.size();
  for (size_t i = 0; i < n; ++i) {
    AqlValue const& element = extractFunctionParameterValue(parameters, i);
    if (!element.isNull(true)) {
      return element.clone();
    }
  }
  return AqlValue(AqlValueHintNull());
}

/// @brief function CURRENT_DATABASE
AqlValue Functions::CurrentDatabase(ExpressionContext* expressionContext,
                                    AstNode const&,
                                    VPackFunctionParametersView parameters) {
  return AqlValue(expressionContext->vocbase().name());
}

/// @brief function CURRENT_USER
AqlValue Functions::CurrentUser(ExpressionContext*, AstNode const&,
                                VPackFunctionParametersView parameters) {
  std::string const& username = ExecContext::current().user();
  if (username.empty()) {
    return AqlValue(AqlValueHintNull());
  }
  return AqlValue(username);
}

/// @brief function COLLECTION_COUNT
AqlValue Functions::CollectionCount(ExpressionContext* expressionContext,
                                    AstNode const&,
                                    VPackFunctionParametersView parameters) {
  // cppcheck-suppress variableScope
  static char const* AFN = "COLLECTION_COUNT";

  AqlValue const& element = extractFunctionParameterValue(parameters, 0);
  if (!element.isString()) {
    THROW_ARANGO_EXCEPTION_PARAMS(
        TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH, AFN);
  }

  transaction::Methods* trx = &expressionContext->trx();

  TRI_ASSERT(ServerState::instance()->isSingleServerOrCoordinator());
  std::string const collectionName = element.slice().copyString();
  OperationOptions options(ExecContext::current());
  OperationResult res =
      trx->count(collectionName, transaction::CountType::Normal, options);
  if (res.fail()) {
    THROW_ARANGO_EXCEPTION(res.result);
  }

  return AqlValue(res.slice());
}

/// @brief function CHECK_DOCUMENT
AqlValue Functions::CheckDocument(ExpressionContext* expressionContext,
                                  AstNode const&,
                                  VPackFunctionParametersView parameters) {
  AqlValue const& value = extractFunctionParameterValue(parameters, 0);
  if (!value.isObject()) {
    // no document at all
    return AqlValue(AqlValueHintBool(false));
  }

  transaction::Methods* trx = &expressionContext->trx();
  auto* vopts = &trx->vpackOptions();
  AqlValueMaterializer materializer(vopts);
  VPackSlice slice = materializer.slice(value, false);

  return AqlValue(AqlValueHintBool(::isValidDocument(slice)));
}

/// @brief function VARIANCE_SAMPLE
AqlValue Functions::VarianceSample(ExpressionContext* expressionContext,
                                   AstNode const&,
                                   VPackFunctionParametersView parameters) {
  static char const* AFN = "VARIANCE_SAMPLE";

  AqlValue const& list = extractFunctionParameterValue(parameters, 0);

  if (!list.isArray()) {
    registerWarning(expressionContext, AFN, TRI_ERROR_QUERY_ARRAY_EXPECTED);
    return AqlValue(AqlValueHintNull());
  }

  double value = 0.0;
  size_t count = 0;

  transaction::Methods* trx = &expressionContext->trx();
  auto* vopts = &trx->vpackOptions();
  if (!::variance(vopts, list, value, count)) {
    registerWarning(expressionContext, AFN,
                    TRI_ERROR_QUERY_INVALID_ARITHMETIC_VALUE);
    return AqlValue(AqlValueHintNull());
  }

  if (count < 2) {
    return AqlValue(AqlValueHintNull());
  }

  return ::numberValue(value / (count - 1), true);
}

/// @brief function VARIANCE_POPULATION
AqlValue Functions::VariancePopulation(ExpressionContext* expressionContext,
                                       AstNode const&,
                                       VPackFunctionParametersView parameters) {
  static char const* AFN = "VARIANCE_POPULATION";

  AqlValue const& list = extractFunctionParameterValue(parameters, 0);

  if (!list.isArray()) {
    registerWarning(expressionContext, AFN, TRI_ERROR_QUERY_ARRAY_EXPECTED);
    return AqlValue(AqlValueHintNull());
  }

  double value = 0.0;
  size_t count = 0;

  transaction::Methods* trx = &expressionContext->trx();
  auto* vopts = &trx->vpackOptions();
  if (!::variance(vopts, list, value, count)) {
    registerWarning(expressionContext, AFN,
                    TRI_ERROR_QUERY_INVALID_ARITHMETIC_VALUE);
    return AqlValue(AqlValueHintNull());
  }

  if (count < 1) {
    return AqlValue(AqlValueHintNull());
  }

  return ::numberValue(value / count, true);
}

/// @brief function STDDEV_SAMPLE
AqlValue Functions::StdDevSample(ExpressionContext* expressionContext,
                                 AstNode const&,
                                 VPackFunctionParametersView parameters) {
  static char const* AFN = "STDDEV_SAMPLE";

  AqlValue const& list = extractFunctionParameterValue(parameters, 0);

  if (!list.isArray()) {
    registerWarning(expressionContext, AFN, TRI_ERROR_QUERY_ARRAY_EXPECTED);
    return AqlValue(AqlValueHintNull());
  }

  double value = 0.0;
  size_t count = 0;

  transaction::Methods* trx = &expressionContext->trx();
  auto* vopts = &trx->vpackOptions();
  if (!::variance(vopts, list, value, count)) {
    registerWarning(expressionContext, AFN,
                    TRI_ERROR_QUERY_INVALID_ARITHMETIC_VALUE);
    return AqlValue(AqlValueHintNull());
  }

  if (count < 2) {
    return AqlValue(AqlValueHintNull());
  }

  return ::numberValue(std::sqrt(value / (count - 1)), true);
}

/// @brief function STDDEV_POPULATION
AqlValue Functions::StdDevPopulation(ExpressionContext* expressionContext,
                                     AstNode const&,
                                     VPackFunctionParametersView parameters) {
  static char const* AFN = "STDDEV_POPULATION";

  AqlValue const& list = extractFunctionParameterValue(parameters, 0);

  if (!list.isArray()) {
    registerWarning(expressionContext, AFN, TRI_ERROR_QUERY_ARRAY_EXPECTED);
    return AqlValue(AqlValueHintNull());
  }

  double value = 0.0;
  size_t count = 0;

  transaction::Methods* trx = &expressionContext->trx();
  auto* vopts = &trx->vpackOptions();
  if (!::variance(vopts, list, value, count)) {
    registerWarning(expressionContext, AFN,
                    TRI_ERROR_QUERY_INVALID_ARITHMETIC_VALUE);
    return AqlValue(AqlValueHintNull());
  }

  if (count < 1) {
    return AqlValue(AqlValueHintNull());
  }

  return ::numberValue(std::sqrt(value / count), true);
}

/// @brief function MEDIAN
AqlValue Functions::Median(ExpressionContext* expressionContext, AstNode const&,
                           VPackFunctionParametersView parameters) {
  static char const* AFN = "MEDIAN";

  AqlValue const& list = extractFunctionParameterValue(parameters, 0);

  if (!list.isArray()) {
    registerWarning(expressionContext, AFN, TRI_ERROR_QUERY_ARRAY_EXPECTED);
    return AqlValue(AqlValueHintNull());
  }

  transaction::Methods* trx = &expressionContext->trx();
  auto* vopts = &trx->vpackOptions();

  std::vector<double> values;
  if (!::sortNumberList(vopts, list, values)) {
    registerWarning(expressionContext, AFN,
                    TRI_ERROR_QUERY_INVALID_ARITHMETIC_VALUE);
    return AqlValue(AqlValueHintNull());
  }

  if (values.empty()) {
    return AqlValue(AqlValueHintNull());
  }
  size_t const l = values.size();
  size_t midpoint = l / 2;

  if (l % 2 == 0) {
    return ::numberValue((values[midpoint - 1] + values[midpoint]) / 2, true);
  }
  return ::numberValue(values[midpoint], true);
}

/// @brief function PERCENTILE
AqlValue Functions::Percentile(ExpressionContext* expressionContext,
                               AstNode const&,
                               VPackFunctionParametersView parameters) {
  static char const* AFN = "PERCENTILE";

  AqlValue const& list = extractFunctionParameterValue(parameters, 0);

  if (!list.isArray()) {
    registerWarning(expressionContext, AFN, TRI_ERROR_QUERY_ARRAY_EXPECTED);
    return AqlValue(AqlValueHintNull());
  }

  AqlValue const& border = extractFunctionParameterValue(parameters, 1);

  if (!border.isNumber()) {
    registerWarning(expressionContext, AFN,
                    TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH);
    return AqlValue(AqlValueHintNull());
  }

  double p = border.toDouble();
  if (p <= 0.0 || p > 100.0) {
    registerWarning(expressionContext, AFN,
                    TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH);
    return AqlValue(AqlValueHintNull());
  }

  bool useInterpolation = false;

  if (parameters.size() == 3) {
    AqlValue const& methodValue = extractFunctionParameterValue(parameters, 2);
    if (!methodValue.isString()) {
      registerWarning(expressionContext, AFN,
                      TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH);
      return AqlValue(AqlValueHintNull());
    }
    std::string method = methodValue.slice().copyString();
    if (method == "interpolation") {
      useInterpolation = true;
    } else if (method == "rank") {
      useInterpolation = false;
    } else {
      registerWarning(expressionContext, AFN,
                      TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH);
      return AqlValue(AqlValueHintNull());
    }
  }

  transaction::Methods* trx = &expressionContext->trx();
  auto* vopts = &trx->vpackOptions();

  std::vector<double> values;
  if (!::sortNumberList(vopts, list, values)) {
    registerWarning(expressionContext, AFN,
                    TRI_ERROR_QUERY_INVALID_ARITHMETIC_VALUE);
    return AqlValue(AqlValueHintNull());
  }

  if (values.empty()) {
    return AqlValue(AqlValueHintNull());
  }

  size_t l = values.size();
  if (l == 1) {
    return ::numberValue(values[0], true);
  }

  TRI_ASSERT(l > 1);

  if (useInterpolation) {
    double const idx = p * (l + 1) / 100.0;
    double const pos = floor(idx);

    if (pos >= l) {
      return ::numberValue(values[l - 1], true);
    }
    if (pos <= 0) {
      return AqlValue(AqlValueHintNull());
    }

    double const delta = idx - pos;
    return ::numberValue(delta * (values[static_cast<size_t>(pos)] -
                                  values[static_cast<size_t>(pos) - 1]) +
                             values[static_cast<size_t>(pos) - 1],
                         true);
  }

  double const idx = p * l / 100.0;
  double const pos = ceil(idx);
  if (pos >= l) {
    return ::numberValue(values[l - 1], true);
  }
  if (pos <= 0) {
    return AqlValue(AqlValueHintNull());
  }

  return ::numberValue(values[static_cast<size_t>(pos) - 1], true);
}

/// @brief function RANGE
AqlValue Functions::Range(ExpressionContext* expressionContext, AstNode const&,
                          VPackFunctionParametersView parameters) {
  // cppcheck-suppress variableScope
  static char const* AFN = "RANGE";

  AqlValue const& left = extractFunctionParameterValue(parameters, 0);
  AqlValue const& right = extractFunctionParameterValue(parameters, 1);

  double from = left.toDouble();
  double to = right.toDouble();

  if (parameters.size() < 3) {
    return AqlValue(left.toInt64(), right.toInt64());
  }

  AqlValue const& stepValue = extractFunctionParameterValue(parameters, 2);
  if (stepValue.isNull(true)) {
    // no step specified. return a real range object
    return AqlValue(left.toInt64(), right.toInt64());
  }

  double step = stepValue.toDouble();

  if (step == 0.0 || (from < to && step < 0.0) || (from > to && step > 0.0)) {
    registerWarning(expressionContext, AFN,
                    TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH);
    return AqlValue(AqlValueHintNull());
  }

  transaction::Methods* trx = &expressionContext->trx();

  transaction::BuilderLeaser builder(trx);
  builder->openArray(true);
  if (step < 0.0 && to <= from) {
    TRI_ASSERT(step != 0.0);
    Range::throwIfTooBigForMaterialization(
        static_cast<uint64_t>((from - to) / -step));
    for (; from >= to; from += step) {
      builder->add(VPackValue(from));
    }
  } else {
    TRI_ASSERT(step != 0.0);
    Range::throwIfTooBigForMaterialization(
        static_cast<uint64_t>((to - from) / step));
    for (; from <= to; from += step) {
      builder->add(VPackValue(from));
    }
  }
  builder->close();
  return AqlValue(builder->slice(), builder->size());
}

/// @brief function POSITION
AqlValue Functions::Position(ExpressionContext* expressionContext,
                             AstNode const&,
                             VPackFunctionParametersView parameters) {
  // cppcheck-suppress variableScope
  static char const* AFN = "POSITION";

  AqlValue const& list = extractFunctionParameterValue(parameters, 0);

  if (!list.isArray()) {
    registerWarning(expressionContext, AFN, TRI_ERROR_QUERY_ARRAY_EXPECTED);
    return AqlValue(AqlValueHintNull());
  }

  bool returnIndex = false;
  if (parameters.size() == 3) {
    AqlValue const& a = extractFunctionParameterValue(parameters, 2);
    returnIndex = a.toBoolean();
  }

  if (list.length() > 0) {
    AqlValue const& searchValue = extractFunctionParameterValue(parameters, 1);

    transaction::Methods* trx = &expressionContext->trx();
    auto* vopts = &trx->vpackOptions();

    size_t index;
    if (::listContainsElement(vopts, list, searchValue, index)) {
      if (!returnIndex) {
        // return true
        return AqlValue(AqlValueHintBool(true));
      }
      // return position
      return AqlValue(AqlValueHintUInt(index));
    }
  }

  // not found
  if (!returnIndex) {
    // return false
    return AqlValue(AqlValueHintBool(false));
  }

  // return -1
  return AqlValue(AqlValueHintInt(-1));
}

/// @brief function CALL
AqlValue Functions::Call(ExpressionContext* expressionContext,
                         AstNode const& node,
                         VPackFunctionParametersView parameters) {
  static char const* AFN = "CALL";

  AqlValue const& invokeFN = extractFunctionParameterValue(parameters, 0);
  if (!invokeFN.isString()) {
    registerError(expressionContext, AFN,
                  TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH);
    return AqlValue(AqlValueHintNull());
  }

  VPackFunctionParameters invokeParams;
  if (parameters.size() >= 2) {
    // we have a list of parameters, need to copy them over except the
    // functionname:
    invokeParams.reserve(parameters.size() - 1);

    for (uint64_t i = 1; i < parameters.size(); i++) {
      invokeParams.push_back(extractFunctionParameterValue(parameters, i));
    }
  }

  return ::callApplyBackend(expressionContext, node, AFN, invokeFN,
                            invokeParams);
}

/// @brief function APPLY
AqlValue Functions::Apply(ExpressionContext* expressionContext,
                          AstNode const& node,
                          VPackFunctionParametersView parameters) {
  static char const* AFN = "APPLY";

  AqlValue const& invokeFN = extractFunctionParameterValue(parameters, 0);
  if (!invokeFN.isString()) {
    registerError(expressionContext, AFN,
                  TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH);
    return AqlValue(AqlValueHintNull());
  }

  VPackFunctionParameters invokeParams;
  AqlValue rawParamArray;
  std::vector<bool> mustFree;

  auto guard = scopeGuard([&mustFree, &invokeParams]() noexcept {
    for (size_t i = 0; i < mustFree.size(); ++i) {
      if (mustFree[i]) {
        invokeParams[i].destroy();
      }
    }
  });

  if (parameters.size() == 2) {
    // We have a parameter that should be an array, whichs content we need to
    // make the sub functions parameters.
    rawParamArray = extractFunctionParameterValue(parameters, 1);

    if (!rawParamArray.isArray()) {
      registerWarning(expressionContext, AFN,
                      TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH);
      return AqlValue(AqlValueHintNull());
    }
    uint64_t len = rawParamArray.length();
    invokeParams.reserve(len);
    mustFree.reserve(len);
    for (uint64_t i = 0; i < len; i++) {
      bool f;
      invokeParams.push_back(rawParamArray.at(i, f, false));
      mustFree.push_back(f);
    }
  }

  return ::callApplyBackend(expressionContext, node, AFN, invokeFN,
                            invokeParams);
}

/// @brief function VERSION
AqlValue Functions::Version(ExpressionContext* expressionContext,
                            AstNode const&,
                            VPackFunctionParametersView parameters) {
  return AqlValue(rest::Version::getServerVersion());
}

/// @brief function IS_SAME_COLLECTION
AqlValue Functions::IsSameCollection(ExpressionContext* expressionContext,
                                     AstNode const&,
                                     VPackFunctionParametersView parameters) {
  static char const* AFN = "IS_SAME_COLLECTION";

  auto* trx = &expressionContext->trx();
  std::string const first = ::extractCollectionName(trx, parameters, 0);
  std::string const second = ::extractCollectionName(trx, parameters, 1);

  if (!first.empty() && !second.empty()) {
    return AqlValue(AqlValueHintBool(first == second));
  }

  registerWarning(expressionContext, AFN,
                  TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH);
  return AqlValue(AqlValueHintNull());
}

AqlValue Functions::PregelResult(ExpressionContext* expressionContext,
                                 AstNode const&,
                                 VPackFunctionParametersView parameters) {
  static char const* AFN = "PREGEL_RESULT";

  AqlValue arg1 = extractFunctionParameterValue(parameters, 0);
  if (!arg1.isNumber() && !arg1.isString()) {
    THROW_ARANGO_EXCEPTION_PARAMS(
        TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH, AFN);
  }
  bool withId = false;
  AqlValue arg2 = extractFunctionParameterValue(parameters, 1);
  if (arg2.isBoolean()) {
    withId = arg2.slice().getBool();
  }

  uint64_t execNr = arg1.toInt64();
  auto& server = expressionContext->trx().vocbase().server();
  if (!server.hasFeature<pregel::PregelFeature>()) {
    registerWarning(expressionContext, AFN, TRI_ERROR_FAILED);
    return AqlValue(AqlValueHintEmptyArray());
  }
  pregel::PregelFeature& feature = server.getFeature<pregel::PregelFeature>();

  VPackBuffer<uint8_t> buffer;
  VPackBuilder builder(buffer);
  if (ServerState::instance()->isCoordinator()) {
    std::shared_ptr<pregel::Conductor> c = feature.conductor(execNr);
    if (!c) {
      registerWarning(expressionContext, AFN, TRI_ERROR_HTTP_NOT_FOUND);
      return AqlValue(AqlValueHintEmptyArray());
    }
    c->collectAQLResults(builder, withId);

  } else {
    std::shared_ptr<pregel::IWorker> worker = feature.worker(execNr);
    if (!worker) {
      registerWarning(expressionContext, AFN, TRI_ERROR_HTTP_NOT_FOUND);
      return AqlValue(AqlValueHintEmptyArray());
    }
    worker->aqlResult(builder, withId);
  }

  if (builder.isEmpty()) {
    return AqlValue(AqlValueHintEmptyArray());
  }
  TRI_ASSERT(builder.slice().isArray());

  // move the buffer into
  return AqlValue(std::move(buffer));
}

AqlValue Functions::Assert(ExpressionContext* expressionContext, AstNode const&,
                           VPackFunctionParametersView parameters) {
  // cppcheck-suppress variableScope
  static char const* AFN = "ASSERT";

  auto const expr = extractFunctionParameterValue(parameters, 0);
  auto const message = extractFunctionParameterValue(parameters, 1);

  if (!message.isString()) {
    registerInvalidArgumentWarning(expressionContext, AFN);
    return AqlValue(AqlValueHintNull());
  }
  if (!expr.toBoolean()) {
    std::string msg = message.slice().copyString();
    expressionContext->registerError(TRI_ERROR_QUERY_USER_ASSERT, msg.data());
  }
  return AqlValue(AqlValueHintBool(true));
}

AqlValue Functions::Warn(ExpressionContext* expressionContext, AstNode const&,
                         VPackFunctionParametersView parameters) {
  // cppcheck-suppress variableScope
  static char const* AFN = "WARN";

  auto const expr = extractFunctionParameterValue(parameters, 0);
  auto const message = extractFunctionParameterValue(parameters, 1);

  if (!message.isString()) {
    registerInvalidArgumentWarning(expressionContext, AFN);
    return AqlValue(AqlValueHintNull());
  }

  if (!expr.toBoolean()) {
    std::string msg = message.slice().copyString();
    expressionContext->registerWarning(TRI_ERROR_QUERY_USER_WARN, msg.data());
    return AqlValue(AqlValueHintBool(false));
  }
  return AqlValue(AqlValueHintBool(true));
}

AqlValue Functions::Fail(ExpressionContext* expressionContext, AstNode const&,
                         VPackFunctionParametersView parameters) {
  if (parameters.size() == 0) {
    THROW_ARANGO_EXCEPTION_PARAMS(TRI_ERROR_QUERY_FAIL_CALLED, "");
  }

  AqlValue const& value = extractFunctionParameterValue(parameters, 0);

  if (!value.isString()) {
    THROW_ARANGO_EXCEPTION_PARAMS(TRI_ERROR_QUERY_FAIL_CALLED, "");
  }

  transaction::Methods* trx = &expressionContext->trx();
  auto* vopts = &trx->vpackOptions();
  AqlValueMaterializer materializer(vopts);
  VPackSlice str = materializer.slice(value, false);
  THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_QUERY_FAIL_CALLED, str.copyString());
}

/// @brief function DATE_FORMAT
AqlValue Functions::DateFormat(ExpressionContext* expressionContext,
                               AstNode const&,
                               VPackFunctionParametersView params) {
  static char const* AFN = "DATE_FORMAT";
  tp_sys_clock_ms tp;

  if (!::parameterToTimePoint(expressionContext, params, tp, AFN, 0)) {
    return AqlValue(AqlValueHintNull());
  }

  AqlValue const& aqlFormatString = extractFunctionParameterValue(params, 1);
  if (!aqlFormatString.isString()) {
    registerInvalidArgumentWarning(expressionContext, AFN);
    return AqlValue(AqlValueHintNull());
  }

  return AqlValue(basics::formatDate(aqlFormatString.slice().copyString(), tp));
}

AqlValue Functions::ShardId(ExpressionContext* expressionContext,
                            AstNode const&,
                            VPackFunctionParametersView parameters) {
  // Expecting 2 parameters
  // 0 : collection name or id
  // 1 : {shardKey1 : k1, shardKey2: k2, ..., shardKeyn: kn}
  auto const col = extractFunctionParameterValue(parameters, 0).slice();
  auto const keys = extractFunctionParameterValue(parameters, 1).slice();

  if (!col.isString()) {
    THROW_ARANGO_EXCEPTION_MESSAGE(
        TRI_ERROR_BAD_PARAMETER,
        "first parameter to SHARD_ID must be a string");
  }
  if (!keys.isObject()) {
    THROW_ARANGO_EXCEPTION_MESSAGE(
        TRI_ERROR_BAD_PARAMETER,
        "second parameter to SHARD_ID must be an object");
  }

  auto& vocbase = expressionContext->trx().vocbase();
  auto role = ServerState::instance()->getRole();
  std::shared_ptr<LogicalCollection> collection;
  auto const colName = col.copyString();
  auto const& dbName = vocbase.name();
  auto const cluster = role == ServerState::ROLE_COORDINATOR ||
                       role == ServerState::ROLE_DBSERVER;

  if (cluster) {
    auto& ci = vocbase.server().getFeature<ClusterFeature>().clusterInfo();
    collection = ci.getCollection(dbName, colName);
  } else {  // single server, agents, active failover try to not break cluster
            // ready code
    methods::Collections::lookup(vocbase, colName, collection);
  }

  if (collection == nullptr) {
    THROW_ARANGO_EXCEPTION_MESSAGE(
        TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND,
        "could not find collection: " + colName + " in database " + dbName);
  }

  std::string shardId;
  if (cluster) {
    auto const errorCode = collection->getResponsibleShard(keys, true, shardId);
    if (errorCode != TRI_ERROR_NO_ERROR) {
      THROW_ARANGO_EXCEPTION_MESSAGE(
          errorCode, "could not find shard for document by shard keys " +
                         keys.toJson() + " in " + colName);
    }
  } else {  // Agents, single server, AFO return the collection name in favour
            // of AQL universality
    shardId = colName;
  }

  return AqlValue{shardId};
}

/// @brief function DECODE_REV
AqlValue Functions::DecodeRev(ExpressionContext* expressionContext,
                              AstNode const&,
                              VPackFunctionParametersView parameters) {
  auto const rev = extractFunctionParameterValue(parameters, 0);
  if (!rev.isString()) {
    registerInvalidArgumentWarning(expressionContext, "DECODE_REV");
    return AqlValue(AqlValueHintNull());
  }

  VPackValueLength l;
  char const* p = rev.slice().getString(l);
  uint64_t revInt = basics::HybridLogicalClock::decodeTimeStamp(p, l);

  if (revInt == 0 || revInt == UINT64_MAX) {
    registerInvalidArgumentWarning(expressionContext, "DECODE_REV");
    return AqlValue(AqlValueHintNull());
  }

  transaction::Methods* trx = &expressionContext->trx();

  uint64_t timeMilli = basics::HybridLogicalClock::extractTime(revInt);
  uint64_t count = basics::HybridLogicalClock::extractCount(revInt);
  time_t timeSeconds = timeMilli / 1000;
  uint64_t millis = timeMilli % 1000;
  struct tm date;
  TRI_gmtime(timeSeconds, &date);

  char buffer[32];
  strftime(buffer, 32, "%Y-%m-%dT%H:%M:%S.000Z", &date);
  // fill millisecond part not covered by strftime
  buffer[20] = static_cast<char>(millis / 100) + '0';
  buffer[21] = ((millis / 10) % 10) + '0';
  buffer[22] = (millis % 10) + '0';
  // buffer[23] is 'Z'
  buffer[24] = 0;

  transaction::BuilderLeaser builder(trx);
  builder->openObject();
  builder->add("date", VPackValue(buffer));
  builder->add("count", VPackValue(count));
  builder->close();

  return AqlValue(builder->slice(), builder->size());
}

AqlValue Functions::SchemaGet(ExpressionContext* expressionContext,
                              AstNode const&,
                              VPackFunctionParametersView parameters) {
  transaction::Methods* trx = &expressionContext->trx();
  // SCHEMA_GET(collectionName) -> schema object
  std::string const collectionName =
      ::extractCollectionName(trx, parameters, 0);

  if (collectionName.empty()) {
    THROW_ARANGO_EXCEPTION_MESSAGE(
        TRI_ERROR_BAD_PARAMETER,
        "could not extract collection name from parameters");
  }

  std::shared_ptr<LogicalCollection> logicalCollection;
  methods::Collections::lookup(trx->vocbase(), collectionName,
                               logicalCollection);
  if (!logicalCollection) {
    THROW_ARANGO_EXCEPTION_MESSAGE(
        TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND,
        "could not find collection: " + collectionName);
  }

  transaction::BuilderLeaser builder(trx);
  logicalCollection->schemaToVelocyPack(*builder.get());
  VPackSlice slice = builder->slice();

  if (!slice.isObject()) {
    return AqlValue(AqlValueHintNull{});
  }

  auto ruleSlice = slice.get(StaticStrings::ValidationParameterRule);

  if (!ruleSlice.isObject()) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_BAD_PARAMETER,
                                   "schema definition for collection " +
                                       collectionName + " has no rule object");
  }

  return AqlValue(slice, builder->size());
}

AqlValue Functions::SchemaValidate(ExpressionContext* expressionContext,
                                   AstNode const&,
                                   VPackFunctionParametersView parameters) {
  // SCHEMA_VALIDATE(doc, schema object) -> { valid (bool), [errorMessage
  // (string)] }
  static char const* AFN = "SCHEMA_VALIDATE";
  transaction::Methods* trx = &expressionContext->trx();
  auto* vopts = &trx->vpackOptions();

  AqlValue const& docValue = extractFunctionParameterValue(parameters, 0);
  AqlValue const& schemaValue = extractFunctionParameterValue(parameters, 1);

  if (!docValue.isObject()) {
    registerWarning(expressionContext, AFN,
                    Result(TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH,
                           "expecting document object"));
    return AqlValue(AqlValueHintNull());
  }

  if (schemaValue.isNull(false) ||
      (schemaValue.isObject() && schemaValue.length() == 0)) {
    // schema is null or {}
    transaction::BuilderLeaser resultBuilder(trx);
    {
      VPackObjectBuilder guard(resultBuilder.builder());
      resultBuilder->add("valid", VPackValue(true));
    }
    return AqlValue(resultBuilder->slice(), resultBuilder->size());
  }

  if (!schemaValue.isObject()) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_BAD_PARAMETER,
                                   "second parameter is not a schema object: " +
                                       schemaValue.slice().toJson());
  }
  auto* validator = expressionContext->buildValidator(schemaValue.slice());

  // store and restore validation level this is cheaper than modifying the VPack
  auto storedLevel = validator->level();

  // override level so the validation will be executed no matter what
  validator->setLevel(ValidationLevel::Strict);

  Result res;
  {
    ScopeGuard guard([storedLevel, &validator]() noexcept {
      validator->setLevel(storedLevel);
    });

    res = validator->validateOne(docValue.slice(), vopts);
  }

  transaction::BuilderLeaser resultBuilder(trx);
  {
    VPackObjectBuilder guard(resultBuilder.builder());
    resultBuilder->add("valid", VPackValue(res.ok()));
    if (res.fail()) {
      resultBuilder->add(StaticStrings::ErrorMessage,
                         VPackValue(res.errorMessage()));
    }
  }

  return AqlValue(resultBuilder->slice(), resultBuilder->size());
}

AqlValue Functions::Interleave(aql::ExpressionContext* expressionContext,
                               AstNode const&,
                               VPackFunctionParametersView parameters) {
  // cppcheck-suppress variableScope
  static char const* AFN = "INTERLEAVE";

  transaction::Methods* trx = &expressionContext->trx();
  auto* vopts = &trx->vpackOptions();

  struct ArrayIteratorPair {
    VPackArrayIterator current;
    VPackArrayIterator end;
  };

  std::list<ArrayIteratorPair> iters;
  std::vector<AqlValueMaterializer> materializers;
  materializers.reserve(parameters.size());

  for (AqlValue const& parameter : parameters) {
    auto& materializer = materializers.emplace_back(vopts);
    VPackSlice slice = materializer.slice(parameter, true);

    if (!slice.isArray()) {
      // not an array
      registerWarning(expressionContext, AFN, TRI_ERROR_QUERY_ARRAY_EXPECTED);
      return AqlValue(AqlValueHintNull());
    } else if (slice.isEmptyArray()) {
      continue;  // skip empty array here
    }

    VPackArrayIterator iter(slice);
    ArrayIteratorPair pair{iter.begin(), iter.end()};
    iters.emplace_back(pair);
  }

  transaction::BuilderLeaser builder(trx);
  builder->openArray();

  while (!iters.empty()) {  // in this loop we only deal with nonempty arrays
    for (auto i = iters.begin(); i != iters.end();) {
      builder->add(i->current.value());  // thus this will always be valid on
                                         // the first iteration
      i->current++;
      if (i->current == i->end) {
        i = iters.erase(i);
      } else {
        i++;
      }
    }
  }

  builder->close();
  return AqlValue(builder->slice(), builder->size());
}

AqlValue Functions::CallGreenspun(aql::ExpressionContext* expressionContext,
                                  AstNode const&,
                                  VPackFunctionParametersView parameters) {
  transaction::Methods* trx = &expressionContext->trx();
  greenspun::Machine m;
  greenspun::InitMachine(m);

  transaction::BuilderLeaser programBuilder(trx);
  {
    VPackArrayBuilder array(programBuilder.builder());
    for (size_t p = 0; p < parameters.size(); p++) {
      programBuilder->add(extractFunctionParameterValue(parameters, p).slice());
    }
  }

  transaction::BuilderLeaser resultBuilder(trx);
  auto result =
      greenspun::Evaluate(m, programBuilder->slice(), *resultBuilder.builder());

  if (result) {
    return AqlValue(resultBuilder->slice(), resultBuilder->size());
  } else {
    auto msg = result.error().toString();
    expressionContext->registerError(TRI_ERROR_AIR_EXECUTION_ERROR, msg.data());
    return AqlValue(AqlValueHintNull());
  }
}

static void buildKeyObject(VPackBuilder& builder, std::string_view key,
                           bool closeObject) {
  builder.openObject(true);
  builder.add(StaticStrings::KeyString, VPackValue(key));
  if (closeObject) {
    builder.close();
  }
}

static AqlValue ConvertToObject(transaction::Methods& trx, VPackSlice input,
                                bool allowKeyConversionToObject,
                                bool canUseCustomKey, bool ignoreErrors) {
  // input is not an object.
  // if this happens, it must be a string key
  if (!input.isString() || !allowKeyConversionToObject) {
    if (ignoreErrors) {
      return AqlValue{};
    }
    THROW_ARANGO_EXCEPTION(TRI_ERROR_ARANGO_DOCUMENT_TYPE_INVALID);
  }

  // check if custom keys are allowed in this context
  if (!canUseCustomKey) {
    if (ignoreErrors) {
      return AqlValue{};
    }
    THROW_ARANGO_EXCEPTION(TRI_ERROR_CLUSTER_MUST_NOT_SPECIFY_KEY);
  }

  // convert string key into object with { _key: "string" }
  TRI_ASSERT(allowKeyConversionToObject);
  transaction::BuilderLeaser builder(&trx);
  buildKeyObject(*builder, input.stringView(), /*closeObject*/ true);
  return AqlValue{builder->slice()};
}

AqlValue Functions::MakeDistributeInput(
    aql::ExpressionContext* expressionContext, AstNode const&,
    VPackFunctionParametersView parameters) {
  AqlValue const& value = extractFunctionParameterValue(parameters, 0);
  VPackSlice const input = value.slice();  // will throw when wrong type

  if (!input.isObject()) {
    transaction::Methods& trx = expressionContext->trx();
    VPackSlice flags = extractFunctionParameterValue(parameters, 1).slice();
    bool allowKeyConversionToObject =
        flags["allowKeyConversionToObject"].getBool();
    bool canUseCustomKey = flags["canUseCustomKey"].getBool();
    bool ignoreErrors = flags["ignoreErrors"].getBool();
    return ConvertToObject(trx, input, allowKeyConversionToObject,
                           canUseCustomKey, ignoreErrors);
  }
  TRI_ASSERT(input.isObject());

  return AqlValue{input};
}

AqlValue Functions::MakeDistributeInputWithKeyCreation(
    aql::ExpressionContext* expressionContext, AstNode const&,
    VPackFunctionParametersView parameters) {
  transaction::Methods& trx = expressionContext->trx();
  AqlValue value = extractFunctionParameterValue(parameters, 0);

  VPackSlice opts = extractFunctionParameterValue(parameters, 2).slice();
  bool allowSpecifiedKeys = opts["allowSpecifiedKeys"].getBool();
  bool ignoreErrors = opts["ignoreErrors"].getBool();
  std::string const collectionName = opts["collection"].copyString();

  // if empty, check alternative input register
  if (value.isNull(true)) {
    // value is set, but null
    // check if there is a second input register available (UPSERT makes use of
    // two input registers,
    // one for the search document, the other for the insert document)
    value = extractFunctionParameterValue(parameters, 1);
    allowSpecifiedKeys = false;
  }

  if (collectionName.empty()) {
    if (ignoreErrors) {
      return AqlValue{};
    }
    THROW_ARANGO_EXCEPTION_MESSAGE(
        TRI_ERROR_BAD_PARAMETER,
        "could not extract collection name from parameters");
  }

  std::shared_ptr<LogicalCollection> logicalCollection;
  methods::Collections::lookup(trx.vocbase(), collectionName,
                               logicalCollection);
  if (logicalCollection == nullptr) {
    if (ignoreErrors) {
      return AqlValue{};
    }
    THROW_ARANGO_EXCEPTION_MESSAGE(
        TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND,
        "could not find collection " + collectionName);
  }

  bool canUseCustomKey =
      logicalCollection->usesDefaultShardKeys() || allowSpecifiedKeys;

  VPackSlice input = value.slice();  // will throw when wrong type
  if (!input.isObject()) {
    return ConvertToObject(trx, input, true, canUseCustomKey, ignoreErrors);
  }

  TRI_ASSERT(input.isObject());

  bool buildNewObject = !input.hasKey(StaticStrings::KeyString);
  // we are responsible for creating keys if none present

  if (!buildNewObject && !canUseCustomKey) {
    if (ignoreErrors) {
      return AqlValue{};
    }
    // the collection is not sharded by _key, but there is a _key present in
    // the data. a _key was given, but user is not allowed to specify _key
    THROW_ARANGO_EXCEPTION(TRI_ERROR_CLUSTER_MUST_NOT_SPECIFY_KEY);
  }

  if (buildNewObject && !logicalCollection->mustCreateKeyOnCoordinator()) {
    // if we only have a single shard, we let the DB server generate the
    // keys
    buildNewObject = false;
  }

  if (buildNewObject) {
    transaction::BuilderLeaser builder(&trx);
    buildKeyObject(
        *builder,
        std::string_view(logicalCollection->keyGenerator().generate(input)),
        /*closeObject*/ false);
    for (auto cur : VPackObjectIterator(input)) {
      builder->add(cur.key.stringView(), cur.value);
    }
    builder->close();
    return AqlValue{builder->slice()};
  }

  return AqlValue{input};
}

AqlValue Functions::MakeDistributeGraphInput(
    aql::ExpressionContext* expressionContext, AstNode const&,
    VPackFunctionParametersView parameters) {
  transaction::Methods& trx = expressionContext->trx();
  AqlValue const& value = extractFunctionParameterValue(parameters, 0);
  VPackSlice input = value.slice();  // will throw when wrong type
  if (input.isString()) {
    // Need to fix this document.
    // We need id and key as input.

    transaction::BuilderLeaser builder(&trx);
    std::string_view s(input.stringView());
    size_t pos = s.find('/');
    if (pos == s.npos) {
      buildKeyObject(*builder, s, /*closeObject*/ true);
    } else {
      // s is an id string, so let's create an object with id + key
      auto key = s.substr(pos + 1);
      buildKeyObject(*builder, key, /*closeObject*/ false);
      builder->add(StaticStrings::IdString, input);
      builder->close();
    }

    return AqlValue{builder->slice()};
  }

  // check input value
  VPackSlice idSlice;
  bool valid = input.isObject();
  if (valid) {
    idSlice = input.get(StaticStrings::IdString);
    // no _id, no cookies
    valid = !idSlice.isNone();
  }

  if (!valid) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_QUERY_PARSE,
                                   "invalid start vertex. Must either be "
                                   "an _id string value or an object with _id. "
                                   "Instead got: " +
                                       input.toJson());
  }

  if (!input.hasKey(StaticStrings::KeyString)) {
    // The input is an object, but only contains an _id, not a _key value that
    // could be extracted. We can work with _id value only however so let us do
    // this.
    auto keyPart = transaction::helpers::extractKeyPart(idSlice);
    transaction::BuilderLeaser builder(&trx);
    buildKeyObject(*builder, std::string_view(keyPart), /*closeObject*/ false);
    for (auto cur : VPackObjectIterator(input)) {
      builder->add(cur.key.stringView(), cur.value);
    }
    builder->close();
    return AqlValue{builder->slice()};
  }

  return AqlValue{input};
}

#ifdef USE_ENTERPRISE
AqlValue Functions::SelectSmartDistributeGraphInput(
    aql::ExpressionContext* expressionContext, AstNode const&,
    VPackFunctionParametersView parameters) {
  AqlValue const& from = extractFunctionParameterValue(parameters, 0);
  VPackSlice input = from.slice();  // will throw when wrong type
  if (ADB_UNLIKELY(!input.isObject() ||
                   !input.hasKey(StaticStrings::IdString) ||
                   !input.get(StaticStrings::IdString).isString())) {
    // This is an internal use function, so the if condition should always be
    // true Just a protection against users typing this method by hand.
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_QUERY_PARSE,
                                   "invalid start vertex. Must either be "
                                   "an _id string value or an object with _id. "
                                   "Instead got: " +
                                       input.toJson());
  }
  auto fromId = input.get(StaticStrings::IdString).stringView();
  auto res =
      SmartGraphValidationHelper::SmartValidationResult::validateVertexId(
          fromId, expressionContext->vocbase());
  if (res.ok()) {
    return AqlValue{input};
  }
  // From vertex is not smart. Use the other side.

  // It does not matter if the other side is actually smart.
  // Validity will be checked before (MAKE_DISTRIBUTE INPUT) and after
  // (Distribute/PathQuery)
  // If this vertex is Smart we shard by it.
  // If not, we assume it to be satellite, so it can be send anywhere.
  return extractFunctionParameterValue(parameters, 1);
}
#endif

template<typename F>
AqlValue decayFuncImpl(aql::ExpressionContext* expressionContext,
                       AstNode const& node,
                       VPackFunctionParametersView parameters,
                       F&& decayFuncFactory) {
  AqlValue const& argValue = extractFunctionParameterValue(parameters, 0);
  AqlValue const& originValue = extractFunctionParameterValue(parameters, 1);
  AqlValue const& scaleValue = extractFunctionParameterValue(parameters, 2);
  AqlValue const& offsetValue = extractFunctionParameterValue(parameters, 3);
  AqlValue const& decayValue = extractFunctionParameterValue(parameters, 4);

  // check type of arguments
  if ((!argValue.isRange() && !argValue.isArray() && !argValue.isNumber()) ||
      !originValue.isNumber() || !scaleValue.isNumber() ||
      !offsetValue.isNumber() || !decayValue.isNumber()) {
    registerInvalidArgumentWarning(expressionContext,
                                   getFunctionName(node).c_str());
    return AqlValue(AqlValueHintNull());
  }

  // extracting values
  bool failed;
  bool error = false;
  double origin = originValue.toDouble(failed);
  error |= failed;
  double scale = scaleValue.toDouble(failed);
  error |= failed;
  double offset = offsetValue.toDouble(failed);
  error |= failed;
  double decay = decayValue.toDouble(failed);
  error |= failed;

  if (error) {
    registerWarning(expressionContext, getFunctionName(node).c_str(),
                    TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH);
    return AqlValue(AqlValueHintNull());
  }

  // check that parameters are correct
  if (scale <= 0 || offset < 0 || decay <= 0 || decay >= 1) {
    registerWarning(expressionContext, getFunctionName(node).c_str(),
                    TRI_ERROR_QUERY_NUMBER_OUT_OF_RANGE);
    return AqlValue(AqlValueHintNull());
  }

  // get lambda for further calculation
  auto decayFunc = decayFuncFactory(origin, scale, offset, decay);

  // argument is number
  if (argValue.isNumber()) {
    double arg = argValue.slice().getNumber<double>();
    double funcRes = decayFunc(arg);
    return ::numberValue(funcRes, true);
  } else {
    // argument is array or range
    auto* trx = &expressionContext->trx();
    AqlValueMaterializer materializer(&trx->vpackOptions());
    VPackSlice slice = materializer.slice(argValue, true);
    TRI_ASSERT(slice.isArray());

    VPackBuilder builder;
    {
      VPackArrayBuilder arrayBuilder(&builder);
      for (VPackSlice currArg : VPackArrayIterator(slice)) {
        if (!currArg.isNumber()) {
          registerWarning(expressionContext, getFunctionName(node).c_str(),
                          TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH);
          return AqlValue(AqlValueHintNull());
        }
        double arg = currArg.getNumber<double>();
        double funcRes = decayFunc(arg);
        builder.add(VPackValue(funcRes));
      }
    }

    return AqlValue(std::move(*builder.steal()));
  }
}

AqlValue Functions::DecayGauss(aql::ExpressionContext* expressionContext,
                               AstNode const& node,
                               VPackFunctionParametersView parameters) {
  auto gaussDecayFactory = [](const double origin, const double scale,
                              const double offset, const double decay) {
    const double sigmaSqr = -(scale * scale) / (2 * std::log(decay));
    return [=](double arg) {
      double max = std::max(0.0, std::fabs(arg - origin) - offset);
      double numerator = max * max;
      double val = std::exp(-numerator / (2 * sigmaSqr));
      return val;
    };
  };

  return decayFuncImpl(expressionContext, node, parameters, gaussDecayFactory);
}

AqlValue Functions::DecayExp(aql::ExpressionContext* expressionContext,
                             AstNode const& node,
                             VPackFunctionParametersView parameters) {
  auto expDecayFactory = [](const double origin, const double scale,
                            const double offset, const double decay) {
    const double lambda = std::log(decay) / scale;
    return [=](double arg) {
      double numerator =
          lambda * std::max(0.0, std::abs(arg - origin) - offset);
      double val = std::exp(numerator);
      return val;
    };
  };

  return decayFuncImpl(expressionContext, node, parameters, expDecayFactory);
}

AqlValue Functions::DecayLinear(aql::ExpressionContext* expressionContext,
                                AstNode const& node,
                                VPackFunctionParametersView parameters) {
  auto linearDecayFactory = [](const double origin, const double scale,
                               const double offset, const double decay) {
    const double s = scale / (1.0 - decay);
    return [=](double arg) {
      double max = std::max(0.0, std::fabs(arg - origin) - offset);
      double val = std::max((s - max) / s, 0.0);
      return val;
    };
  };

  return decayFuncImpl(expressionContext, node, parameters, linearDecayFactory);
}

template<typename F>
AqlValue DistanceImpl(aql::ExpressionContext* expressionContext,
                      AstNode const& node,
                      VPackFunctionParametersView parameters,
                      F&& distanceFunc) {
  auto calculateDistance = [distanceFunc = std::forward<F>(distanceFunc),
                            expressionContext,
                            &node](const VPackSlice lhs, const VPackSlice rhs) {
    TRI_ASSERT(lhs.isArray());
    TRI_ASSERT(rhs.isArray());
    auto lhsLength = lhs.length();
    auto rhsLength = rhs.length();

    if (lhsLength != rhsLength) {
      registerWarning(expressionContext, getFunctionName(node).c_str(),
                      TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH);
      return AqlValue(AqlValueHintNull());
    }

    return distanceFunc(lhs, rhs, lhsLength);
  };

  // extract arguments
  AqlValue const& argLhs = extractFunctionParameterValue(parameters, 0);
  AqlValue const& argRhs = extractFunctionParameterValue(parameters, 1);

  // check type of arguments
  if (!argLhs.isArray() || !argRhs.isArray() || argLhs.length() == 0 ||
      argRhs.length() == 0) {
    registerInvalidArgumentWarning(expressionContext,
                                   getFunctionName(node).c_str());
    return AqlValue(AqlValueHintNull());
  }

  auto lhsFirstElem = argLhs.slice().at(0);
  auto rhsFirstElem = argRhs.slice().at(0);

  // one of the args is matrix
  if (lhsFirstElem.isArray() || rhsFirstElem.isArray()) {
    decltype(lhsFirstElem) matrix;
    decltype(lhsFirstElem) array;

    if (lhsFirstElem.isArray()) {
      matrix = argLhs.slice();
      array = argRhs.slice();
    } else {
      matrix = argRhs.slice();
      array = argLhs.slice();
    }

    VPackBuilder builder;
    {
      VPackArrayBuilder arrayBuilder(&builder);
      for (VPackSlice currRow : VPackArrayIterator(matrix)) {
        if (!currRow.isArray()) {
          registerWarning(expressionContext, getFunctionName(node).c_str(),
                          TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH);
          return AqlValue(AqlValueHintNull());
        }

        AqlValue dist = calculateDistance(currRow, array);
        if (!dist.isNumber()) {
          return AqlValue(AqlValueHintNull());
        }

        builder.add(dist.slice());
      }
    }
    // return array with values
    return AqlValue(std::move(*builder.steal()));

  } else {
    // calculate dist between 2 vectors and return number
    return calculateDistance(argLhs.slice(), argRhs.slice());
  }
}

AqlValue Functions::CosineSimilarity(aql::ExpressionContext* expressionContext,
                                     AstNode const& node,
                                     VPackFunctionParametersView parameters) {
  auto cosineSimilarityFunc = [expressionContext, &node](
                                  const VPackSlice lhs, const VPackSlice rhs,
                                  const VPackValueLength& length) {
    double numerator{};
    double lhsSum{};
    double rhsSum{};

    for (VPackValueLength i = 0; i < length; ++i) {
      auto lhsSlice = lhs.at(i);
      auto rhsSlice = rhs.at(i);
      if (!lhsSlice.isNumber() || !rhsSlice.isNumber()) {
        registerWarning(expressionContext, getFunctionName(node).c_str(),
                        TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH);
        return AqlValue(AqlValueHintNull());
      }

      double lhsVal = lhsSlice.getNumber<double>();
      double rhsVal = rhsSlice.getNumber<double>();

      numerator += lhsVal * rhsVal;
      lhsSum += lhsVal * lhsVal;
      rhsSum += rhsVal * rhsVal;
    }

    double denominator = std::sqrt(lhsSum) * std::sqrt(rhsSum);
    if (denominator == 0.0) {
      registerWarning(expressionContext, getFunctionName(node).c_str(),
                      TRI_ERROR_QUERY_INVALID_ARITHMETIC_VALUE);
      return AqlValue(AqlValueHintNull());
    }

    return ::numberValue(numerator / denominator, true);
  };

  return DistanceImpl(expressionContext, node, parameters,
                      cosineSimilarityFunc);
}

AqlValue Functions::L1Distance(aql::ExpressionContext* expressionContext,
                               AstNode const& node,
                               VPackFunctionParametersView parameters) {
  auto L1DistFunc = [expressionContext, &node](const VPackSlice lhs,
                                               const VPackSlice rhs,
                                               const VPackValueLength& length) {
    double dist{};

    for (VPackValueLength i = 0; i < length; ++i) {
      auto lhsSlice = lhs.at(i);
      auto rhsSlice = rhs.at(i);
      if (!lhsSlice.isNumber() || !rhsSlice.isNumber()) {
        registerWarning(expressionContext, getFunctionName(node).c_str(),
                        TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH);
        return AqlValue(AqlValueHintNull());
      }

      dist +=
          std::abs(lhsSlice.getNumber<double>() - rhsSlice.getNumber<double>());
    }

    return ::numberValue(dist, true);
  };

  return DistanceImpl(expressionContext, node, parameters, L1DistFunc);
}

AqlValue Functions::L2Distance(aql::ExpressionContext* expressionContext,
                               AstNode const& node,
                               VPackFunctionParametersView parameters) {
  auto L2DistFunc = [expressionContext, &node](const VPackSlice lhs,
                                               const VPackSlice rhs,
                                               const VPackValueLength& length) {
    double dist{};

    for (VPackValueLength i = 0; i < length; ++i) {
      auto lhsSlice = lhs.at(i);
      auto rhsSlice = rhs.at(i);
      if (!lhsSlice.isNumber() || !rhsSlice.isNumber()) {
        registerWarning(expressionContext, getFunctionName(node).c_str(),
                        TRI_ERROR_QUERY_FUNCTION_ARGUMENT_TYPE_MISMATCH);
        return AqlValue(AqlValueHintNull());
      }

      double diff = lhsSlice.getNumber<double>() - rhsSlice.getNumber<double>();

      dist += std::pow(diff, 2);
    }

    return ::numberValue(std::sqrt(dist), true);
  };

  return DistanceImpl(expressionContext, node, parameters, L2DistFunc);
}

AqlValue Functions::NotImplemented(ExpressionContext* expressionContext,
                                   AstNode const&,
                                   VPackFunctionParametersView params) {
  registerError(expressionContext, "UNKNOWN", TRI_ERROR_NOT_IMPLEMENTED);
  return AqlValue(AqlValueHintNull());
}
