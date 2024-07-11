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
/// @author Dr. Frank Celler
////////////////////////////////////////////////////////////////////////////////

#ifndef USE_V8
#error this file is not supposed to be used in builds with -DUSE_V8=Off
#endif

#include "v8-vocbaseprivate.h"

#include <unicode/dtfmtsym.h>
#include <unicode/smpdtfmt.h>
#include <unicode/timezone.h>

#include <velocypack/Iterator.h>
#include <velocypack/Slice.h>

#include <v8.h>
#include <iostream>
#include <thread>

#include "Agency/State.h"
#include "ApplicationFeatures/ApplicationServer.h"
#include "ApplicationFeatures/HttpEndpointProvider.h"
#include "Aql/ExpressionContext.h"
#include "Aql/Query.h"
#include "Aql/QueryAborter.h"
#include "Aql/QueryCache.h"
#include "Aql/QueryExecutionState.h"
#include "Aql/QueryList.h"
#include "Aql/QueryResultV8.h"
#include "Aql/QueryString.h"
#include "Basics/HybridLogicalClock.h"
#include "Basics/StaticStrings.h"
#include "Basics/Utf8Helper.h"
#include "Basics/application-exit.h"
#include "Basics/conversions.h"
#include "Basics/tri-strings.h"
#include "Cluster/ClusterFeature.h"
#include "Cluster/ClusterInfo.h"
#include "Cluster/ServerState.h"
#include "Cluster/TraverserEngine.h"
#include "GeneralServer/AuthenticationFeature.h"
#include "GeneralServer/GeneralServerFeature.h"
#include "Logger/LogMacros.h"
#include "Logger/Logger.h"
#include "Logger/LoggerStream.h"
#include "Rest/Version.h"
#include "RestHandler/RestVersionHandler.h"
#include "RestServer/ConsoleThread.h"
#include "RestServer/DatabaseFeature.h"
#include "RocksDBEngine/RocksDBEngine.h"
#include "Statistics/StatisticsFeature.h"
#include "StorageEngine/EngineSelectorFeature.h"
#include "StorageEngine/StorageEngine.h"
#include "Transaction/Manager.h"
#include "Transaction/ManagerFeature.h"
#include "Transaction/V8Context.h"
#include "Utils/Events.h"
#include "Utils/ExecContext.h"
#include "V8/JSLoader.h"
#include "V8/v8-conv.h"
#include "V8/v8-helper.h"
#include "V8/v8-utils.h"
#include "V8/v8-vpack.h"
#include "V8Server/V8DealerFeature.h"
#include "V8Server/v8-analyzers.h"
#include "V8Server/v8-collection.h"
#include "V8Server/v8-externals.h"
#include "V8Server/v8-general-graph.h"
#include "V8Server/v8-replicated-logs.h"
#include "V8Server/v8-replication.h"
#include "V8Server/v8-statistics.h"
#include "V8Server/v8-users.h"
#include "V8Server/v8-views.h"
#include "V8Server/v8-voccursor.h"
#include "V8Server/v8-vocindex.h"
#include "VocBase/LogicalCollection.h"
#include "VocBase/Methods/Databases.h"
#include "VocBase/Methods/Queries.h"
#include "VocBase/Methods/Transactions.h"

using namespace arangodb;
using namespace arangodb::basics;
using namespace arangodb::rest;

////////////////////////////////////////////////////////////////////////////////
/// @brief wraps a C++ into a v8::Object
////////////////////////////////////////////////////////////////////////////////

template<class T>
static v8::Handle<v8::Object> WrapClass(
    v8::Isolate* isolate, v8::Persistent<v8::ObjectTemplate>& classTempl,
    int32_t type, T* y) {
  v8::EscapableHandleScope scope(isolate);
  auto context = TRI_IGETC;
  auto localClassTemplate =
      v8::Local<v8::ObjectTemplate>::New(isolate, classTempl);
  // create the new handle to return, and set its template type
  v8::Handle<v8::Object> result =
      localClassTemplate->NewInstance(context).FromMaybe(
          v8::Local<v8::Object>());

  if (result.IsEmpty()) {
    // error
    return scope.Escape<v8::Object>(result);
  }

  // set the c++ pointer for unwrapping later
  result->SetInternalField(SLOT_CLASS_TYPE, v8::Integer::New(isolate, type));
  result->SetInternalField(SLOT_CLASS, v8::External::New(isolate, y));

  return scope.Escape<v8::Object>(result);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief executes a transaction
////////////////////////////////////////////////////////////////////////////////

static void JS_Transaction(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);

  // check if we have some transaction object
  if (args.Length() != 1 || !args[0]->IsObject()) {
    TRI_V8_THROW_EXCEPTION_USAGE("TRANSACTION(<object>)");
  }

  // filled by function
  v8::Handle<v8::Value> result;
  v8::TryCatch tryCatch(isolate);
  Result rv = executeTransactionJS(isolate, args[0], result, tryCatch);

  // do not rethrow if already canceled
  if (isContextCanceled(isolate)) {
    TRI_V8_RETURN(result);
  }

  // has caught and could not be converted to arangoError
  // otherwise it would have been reseted
  if (tryCatch.HasCaught()) {
    tryCatch.ReThrow();
    return;
  }

  if (rv.fail()) {
    THROW_ARANGO_EXCEPTION(rv);
  }

  TRI_V8_RETURN(result);
  TRI_V8_TRY_CATCH_END
}

/// @brief returns the list of currently running managed transactions
static void JS_Transactions(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);

  auto& vocbase = GetContextVocBase(isolate);

  // check if we have some transaction object
  if (args.Length() != 0) {
    TRI_V8_THROW_EXCEPTION_USAGE("TRANSACTIONS()");
  }

  VPackBuilder builder;
  builder.openArray();

  bool const fanout = ServerState::instance()->isCoordinator();
  transaction::Manager* mgr = transaction::ManagerFeature::manager();
  if (!mgr) {
    TRI_V8_THROW_EXCEPTION(TRI_ERROR_SHUTTING_DOWN);
  }
  std::string user;
  if (arangodb::ExecContext::isAuthEnabled()) {
    user = ExecContext::current().user();
  }
  mgr->toVelocyPack(builder, vocbase.name(), user, fanout, /*details*/ false);

  builder.close();

  v8::Handle<v8::Value> result = TRI_VPackToV8(isolate, builder.slice());

  TRI_V8_RETURN(result);
  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief normalize UTF 16 strings
////////////////////////////////////////////////////////////////////////////////

static void JS_NormalizeString(
    v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);

  if (args.Length() != 1) {
    TRI_V8_THROW_EXCEPTION_USAGE("NORMALIZE_STRING(<string>)");
  }

  TRI_normalize_V8_Obj(args, args[0]);
  TRI_V8_TRY_CATCH_END
}

static void JS_Compact(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);

  v8::Local<v8::Context> context = isolate->GetCurrentContext();

  if (ExecContext::isAuthEnabled() && !ExecContext::current().isSuperuser()) {
    TRI_V8_THROW_EXCEPTION(TRI_ERROR_FORBIDDEN);
  }

  bool changeLevel = false;
  bool compactBottomMostLevel = false;

  if (args.Length() > 0) {
    if (args[0]->IsObject()) {
      v8::Handle<v8::Object> obj =
          args[0]->ToObject(TRI_IGETC).FromMaybe(v8::Local<v8::Object>());
      if (TRI_HasProperty(context, isolate, obj, "changeLevel")) {
        changeLevel = TRI_ObjectToBoolean(
            isolate,
            obj->Get(context, TRI_V8_ASCII_STRING(isolate, "changeLevel"))
                .FromMaybe(v8::Local<v8::Value>()));
      }
      if (TRI_HasProperty(context, isolate, obj, "compactBottomMostLevel")) {
        compactBottomMostLevel = TRI_ObjectToBoolean(
            isolate, obj->Get(context, TRI_V8_ASCII_STRING(
                                           isolate, "compactBottomMostLevel"))
                         .FromMaybe(v8::Local<v8::Value>()));
      }
    }
  }

  TRI_GET_SERVER_GLOBALS(ArangodServer);
  StorageEngine& engine =
      v8g->server().getFeature<EngineSelectorFeature>().engine();
  Result res = engine.compactAll(changeLevel, compactBottomMostLevel);

  if (res.fail()) {
    TRI_V8_THROW_EXCEPTION_FULL(res.errorNumber(), res.errorMessage());
  }

  TRI_V8_RETURN_UNDEFINED();
  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief compare two UTF 16 strings
////////////////////////////////////////////////////////////////////////////////

static void JS_CompareString(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);

  if (args.Length() != 2) {
    TRI_V8_THROW_EXCEPTION_USAGE(
        "COMPARE_STRING(<left string>, <right string>)");
  }

  v8::String::Value left(isolate, args[0]);
  v8::String::Value right(isolate, args[1]);

  // ..........................................................................
  // Take note here: we are assuming that the ICU type UChar is two bytes.
  // There is no guarantee that this will be the case on all platforms and
  // compilers.
  // ..........................................................................
  int result = Utf8Helper::DefaultUtf8Helper.compareUtf16(
      *left, left.length(), *right, right.length());

  TRI_V8_RETURN(v8::Integer::New(isolate, result));
  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief get list of timezones
////////////////////////////////////////////////////////////////////////////////

static void JS_GetIcuTimezones(
    v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);
  auto context = TRI_IGETC;

  if (args.Length() != 0) {
    TRI_V8_THROW_EXCEPTION_USAGE("TIMEZONES()");
  }

  v8::Handle<v8::Array> result = v8::Array::New(isolate);

  UErrorCode status = U_ZERO_ERROR;

  icu_64_64::StringEnumeration* timeZones =
      icu_64_64::TimeZone::createEnumeration();
  if (timeZones) {
    int32_t idsCount = timeZones->count(status);

    for (int32_t i = 0; i < idsCount && U_ZERO_ERROR == status; ++i) {
      int32_t resultLength;
      char const* str = timeZones->next(&resultLength, status);
      result
          ->Set(context, (uint32_t)i,
                TRI_V8_PAIR_STRING(isolate, str, resultLength))
          .FromMaybe(false);
    }

    delete timeZones;
  }

  TRI_V8_RETURN(result);
  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief get list of locales
////////////////////////////////////////////////////////////////////////////////

static void JS_GetIcuLocales(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);
  auto context = TRI_IGETC;

  if (args.Length() != 0) {
    TRI_V8_THROW_EXCEPTION_USAGE("LOCALES()");
  }

  v8::Handle<v8::Array> result = v8::Array::New(isolate);

  int32_t count = 0;
  const icu_64_64::Locale* locales =
      icu_64_64::Locale::getAvailableLocales(count);
  if (locales) {
    for (int32_t i = 0; i < count; ++i) {
      const icu_64_64::Locale* l = locales + i;
      char const* str = l->getBaseName();

      result
          ->Set(context, (uint32_t)i,
                TRI_V8_PAIR_STRING(isolate, str, strlen(str)))
          .FromMaybe(false);
    }
  }

  TRI_V8_RETURN(result);
  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief format datetime
////////////////////////////////////////////////////////////////////////////////

static void JS_FormatDatetime(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);
  auto context = TRI_IGETC;

  if (args.Length() < 2) {
    TRI_V8_THROW_EXCEPTION_USAGE(
        "FORMAT_DATETIME(<datetime in sec>, <pattern>, [<timezone>, "
        "[<locale>]])");
  }

  int64_t datetime = TRI_ObjectToInt64(isolate, args[0]);
  v8::String::Value pattern(
      isolate, args[1]->ToString(context).FromMaybe(v8::Handle<v8::String>()));

  icu_64_64::TimeZone* tz = nullptr;
  if (args.Length() > 2) {
    v8::String::Value value(isolate, args[2]->ToString(context).FromMaybe(
                                         v8::Handle<v8::String>()));

    // ..........................................................................
    // Take note here: we are assuming that the ICU type UChar is two bytes.
    // There is no guarantee that this will be the case on all platforms and
    // compilers.
    // ..........................................................................

    icu_64_64::UnicodeString ts((const UChar*)*value, value.length());
    tz = icu_64_64::TimeZone::createTimeZone(ts);
  } else {
    tz = icu_64_64::TimeZone::createDefault();
  }

  icu_64_64::Locale locale;
  if (args.Length() > 3) {
    std::string name = TRI_ObjectToString(isolate, args[3]);
    locale = icu_64_64::Locale::createFromName(name.c_str());
  } else {
    // use language of default collator
    std::string name = Utf8Helper::DefaultUtf8Helper.getCollatorLanguage();
    locale = icu_64_64::Locale::createFromName(name.c_str());
  }

  icu_64_64::UnicodeString formattedString;
  UErrorCode status = U_ZERO_ERROR;
  icu_64_64::UnicodeString aPattern((const UChar*)*pattern, pattern.length());
  icu_64_64::DateFormatSymbols* ds =
      new icu_64_64::DateFormatSymbols(locale, status);
  icu_64_64::SimpleDateFormat* s =
      new icu_64_64::SimpleDateFormat(aPattern, ds, status);
  s->setTimeZone(*tz);
  s->format((UDate)(datetime * 1000), formattedString);

  std::string resultString;
  formattedString.toUTF8String(resultString);
  delete s;
  delete tz;

  TRI_V8_RETURN_STD_STRING(resultString);
  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief parse datetime
////////////////////////////////////////////////////////////////////////////////

static void JS_ParseDatetime(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);
  auto context = TRI_IGETC;

  if (args.Length() < 2) {
    TRI_V8_THROW_EXCEPTION_USAGE(
        "PARSE_DATETIME(<datetime string>, <pattern>, [<timezone>, "
        "[<locale>]])");
  }

  v8::String::Value datetimeString(
      isolate, args[0]->ToString(context).FromMaybe(v8::Handle<v8::String>()));
  v8::String::Value pattern(
      isolate, args[1]->ToString(context).FromMaybe(v8::Handle<v8::String>()));

  icu_64_64::TimeZone* tz = nullptr;
  if (args.Length() > 2) {
    v8::String::Value value(isolate, args[2]->ToString(context).FromMaybe(
                                         v8::Handle<v8::String>()));

    // ..........................................................................
    // Take note here: we are assuming that the ICU type UChar is two bytes.
    // There is no guarantee that this will be the case on all platforms and
    // compilers.
    // ..........................................................................

    icu_64_64::UnicodeString ts((const UChar*)*value, value.length());
    tz = icu_64_64::TimeZone::createTimeZone(ts);
  } else {
    tz = icu_64_64::TimeZone::createDefault();
  }

  icu_64_64::Locale locale;
  if (args.Length() > 3) {
    std::string name = TRI_ObjectToString(isolate, args[3]);
    locale = icu_64_64::Locale::createFromName(name.c_str());
  } else {
    // use language of default collator
    std::string name = Utf8Helper::DefaultUtf8Helper.getCollatorLanguage();
    locale = icu_64_64::Locale::createFromName(name.c_str());
  }

  icu_64_64::UnicodeString formattedString((const UChar*)*datetimeString,
                                           datetimeString.length());
  UErrorCode status = U_ZERO_ERROR;
  icu_64_64::UnicodeString aPattern((const UChar*)*pattern, pattern.length());
  icu_64_64::DateFormatSymbols* ds =
      new icu_64_64::DateFormatSymbols(locale, status);
  icu_64_64::SimpleDateFormat* s =
      new icu_64_64::SimpleDateFormat(aPattern, ds, status);
  s->setTimeZone(*tz);

  UDate udate = s->parse(formattedString, status);

  delete s;
  delete tz;

  TRI_V8_RETURN(v8::Number::New(isolate, udate / 1000));
  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief parses an AQL query
////////////////////////////////////////////////////////////////////////////////

static void JS_ParseAql(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);
  auto context = TRI_IGETC;
  auto& vocbase = GetContextVocBase(isolate);

  if (args.Length() != 1) {
    TRI_V8_THROW_EXCEPTION_USAGE("AQL_PARSE(<querystring>)");
  }

  // get the query string
  if (!args[0]->IsString()) {
    TRI_V8_THROW_TYPE_ERROR("expecting string for <querystring>");
  }

  std::string const queryString(TRI_ObjectToString(isolate, args[0]));
  // If we execute an AQL query from V8 we need to unset the nolock headers
  auto origin = transaction::OperationOriginAQL{"parsing query"};
  auto query = arangodb::aql::Query::create(
      transaction::V8Context::create(vocbase, origin, true),
      aql::QueryString(queryString), nullptr);
  auto parseResult = query->parse();

  if (parseResult.result.fail()) {
    TRI_V8_THROW_EXCEPTION_FULL(parseResult.result.errorNumber(),
                                parseResult.result.errorMessage());
  }

  v8::Handle<v8::Object> result = v8::Object::New(isolate);
  result
      ->Set(context, TRI_V8_ASCII_STRING(isolate, "parsed"), v8::True(isolate))
      .FromMaybe(false);

  {
    v8::Handle<v8::Array> collections = v8::Array::New(isolate);
    result
        ->Set(context, TRI_V8_ASCII_STRING(isolate, "collections"), collections)
        .FromMaybe(false);
    uint32_t i = 0;
    for (auto& elem : parseResult.collectionNames) {
      collections->Set(context, i++, TRI_V8_STD_STRING(isolate, (elem)))
          .FromMaybe(false);
    }
  }

  {
    v8::Handle<v8::Array> bindVars = v8::Array::New(isolate);
    uint32_t i = 0;
    for (auto const& elem : parseResult.bindParameters) {
      bindVars->Set(context, i++, TRI_V8_STD_STRING(isolate, (elem)))
          .FromMaybe(false);
    }
    result->Set(context, TRI_V8_ASCII_STRING(isolate, "parameters"),
                bindVars)
        .FromMaybe(false);  // parameters is deprecated
    result->Set(context, TRI_V8_ASCII_STRING(isolate, "bindVars"), bindVars)
        .FromMaybe(false);
  }

  result
      ->Set(context, TRI_V8_ASCII_STRING(isolate, "ast"),
            TRI_VPackToV8(isolate, parseResult.data->slice()))
      .FromMaybe(false);

  if (parseResult.extra == nullptr ||
      !parseResult.extra->slice().hasKey("warnings")) {
    result
        ->Set(context, TRI_V8_ASCII_STRING(isolate, "warnings"),
              v8::Array::New(isolate))
        .FromMaybe(false);
  } else {
    result
        ->Set(
            context, TRI_V8_ASCII_STRING(isolate, "warnings"),
            TRI_VPackToV8(isolate, parseResult.extra->slice().get("warnings")))
        .FromMaybe(false);
  }

  TRI_V8_RETURN(result);
  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief registers a warning for the currently running AQL query
static void JS_WarningAql(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);

  if (args.Length() != 2) {
    TRI_V8_THROW_EXCEPTION_USAGE("AQL_WARNING(<code>, <message>)");
  }

  // get the query string
  if (!args[1]->IsString()) {
    TRI_V8_THROW_TYPE_ERROR("expecting string for <message>");
  }

  TRI_GET_GLOBALS();

  if (v8g->_expressionContext != nullptr) {
    // only register the error if we have a query...
    // note: we may not have a query if the AQL functions are called without
    // a query, e.g. during tests
    auto code =
        ErrorCode{static_cast<int>(TRI_ObjectToInt64(isolate, args[0]))};
    std::string const message = TRI_ObjectToString(isolate, args[1]);

    auto expressionContext =
        static_cast<arangodb::aql::ExpressionContext*>(v8g->_expressionContext);
    expressionContext->registerWarning(code, message.c_str());
  } else {
    TRI_V8_THROW_TYPE_ERROR(
        "must only be invoked from AQL user defined functions");
  }
  TRI_V8_RETURN_UNDEFINED();
  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief explains an AQL query
////////////////////////////////////////////////////////////////////////////////

static void JS_ExplainAql(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);
  auto context = TRI_IGETC;
  auto& vocbase = GetContextVocBase(isolate);

  if (args.Length() < 1 || args.Length() > 3) {
    TRI_V8_THROW_EXCEPTION_USAGE(
        "AQL_EXPLAIN(<queryString>, <bindVars>, <options>)");
  }

  // get the query string
  if (!args[0]->IsString()) {
    TRI_V8_THROW_TYPE_ERROR("expecting string for <queryString>");
  }

  std::string queryString(TRI_ObjectToString(isolate, args[0]));

  // bind parameters
  std::shared_ptr<VPackBuilder> bindVars;

  if (args.Length() > 1) {
    if (!args[1]->IsUndefined() && !args[1]->IsNull() && !args[1]->IsObject()) {
      TRI_V8_THROW_TYPE_ERROR("expecting object for <bindVars>");
    }
    if (args[1]->IsObject()) {
      bindVars = std::make_shared<VPackBuilder>();

      TRI_V8ToVPack(isolate, *(bindVars.get()), args[1], false);
    }
  }

  VPackBuilder options;
  if (args.Length() > 2) {
    // handle options
    if (!args[2]->IsObject()) {
      TRI_V8_THROW_TYPE_ERROR("expecting object for <options>");
    }
    TRI_V8ToVPack(isolate, options, args[2], false);
  }

  // bind parameters will be freed by the query later
  auto origin = transaction::OperationOriginAQL{"explaining query"};
  auto query = arangodb::aql::Query::create(
      transaction::V8Context::create(vocbase, origin, true),
      aql::QueryString(std::move(queryString)), std::move(bindVars),
      aql::QueryOptions(options.slice()));
  auto queryResult = query->explain();

  if (queryResult.result.fail()) {
    TRI_V8_THROW_EXCEPTION_FULL(queryResult.result.errorNumber(),
                                queryResult.result.errorMessage());
  }

  v8::Handle<v8::Object> result = v8::Object::New(isolate);

  if (queryResult.data != nullptr) {
    if (query->queryOptions().allPlans) {
      result
          ->Set(context, TRI_V8_ASCII_STRING(isolate, "plans"),
                TRI_VPackToV8(isolate, queryResult.data->slice()))
          .FromMaybe(false);
    } else {
      result
          ->Set(context, TRI_V8_ASCII_STRING(isolate, "plan"),
                TRI_VPackToV8(isolate, queryResult.data->slice()))
          .FromMaybe(false);
      result
          ->Set(context, TRI_V8_ASCII_STRING(isolate, "cacheable"),
                v8::Boolean::New(isolate, queryResult.cached))
          .FromMaybe(false);
    }

    if (queryResult.extra != nullptr) {
      VPackSlice warnings = queryResult.extra->slice().get("warnings");
      if (warnings.isNone()) {
        result
            ->Set(context, TRI_V8_ASCII_STRING(isolate, "warnings"),
                  v8::Array::New(isolate))
            .FromMaybe(false);
      } else {
        result
            ->Set(context, TRI_V8_ASCII_STRING(isolate, "warnings"),
                  TRI_VPackToV8(isolate,
                                queryResult.extra->slice().get("warnings")))
            .FromMaybe(false);
      }
      VPackSlice stats = queryResult.extra->slice().get("stats");
      if (stats.isNone()) {
        result
            ->Set(context, TRI_V8_ASCII_STRING(isolate, "stats"),
                  v8::Object::New(isolate))
            .FromMaybe(false);
      } else {
        result
            ->Set(context, TRI_V8_ASCII_STRING(isolate, "stats"),
                  TRI_VPackToV8(isolate, stats))
            .FromMaybe(false);
      }
    } else {
      result
          ->Set(context, TRI_V8_ASCII_STRING(isolate, "warnings"),
                v8::Array::New(isolate))
          .FromMaybe(false);
      result
          ->Set(context, TRI_V8_ASCII_STRING(isolate, "stats"),
                v8::Object::New(isolate))
          .FromMaybe(false);
    }
  }

  TRI_V8_RETURN(result);
  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief executes an AQL query
////////////////////////////////////////////////////////////////////////////////

static void JS_ExecuteAqlJson(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);
  auto context = TRI_IGETC;
  auto& vocbase = GetContextVocBase(isolate);

  if (args.Length() < 1 || args.Length() > 2) {
    TRI_V8_THROW_EXCEPTION_USAGE("AQL_EXECUTEJSON(<queryjson>, <options>)");
  }

  if (!args[0]->IsObject()) {
    TRI_V8_THROW_TYPE_ERROR("expecting object for <queryjson>");
  }

  VPackBuilder queryBuilder;
  TRI_V8ToVPack(isolate, queryBuilder, args[0], false);

  VPackBuilder options;
  if (args.Length() > 1) {
    if (!args[1]->IsUndefined() && !args[1]->IsObject()) {
      TRI_V8_THROW_TYPE_ERROR("expecting object for <options>");
    }

    TRI_V8ToVPack(isolate, options, args[1], false);
  }

  auto origin = transaction::OperationOriginAQL{"executing query from JSON"};
  auto query = aql::Query::create(
      transaction::V8Context::create(vocbase, origin, true), aql::QueryString{},
      /*bindParameters*/ nullptr, aql::QueryOptions(options.slice()));

  VPackSlice collections = queryBuilder.slice().get("collections");
  VPackSlice variables = queryBuilder.slice().get("variables");

  QueryAnalyzerRevisions analyzersRevision;
  auto revisionRes = analyzersRevision.fromVelocyPack(queryBuilder.slice());
  if (ADB_UNLIKELY(revisionRes.fail())) {
    TRI_V8_THROW_EXCEPTION(revisionRes);
  }

  // simon: hack to get the behaviour of old second aql::Query constructor
  VPackBuilder snippetBuilder;  // simon: hack to make format conform
  snippetBuilder.openObject();
  snippetBuilder.add("0", VPackValue(VPackValueType::Object));
  snippetBuilder.add("nodes", queryBuilder.slice().get("nodes"));
  snippetBuilder.close();
  snippetBuilder.close();

  TRI_ASSERT(!ServerState::instance()->isDBServer());
  auto aborter = std::make_shared<aql::QueryAborter>(query);
  VPackBuilder ignoreResponse;
  query->prepareFromVelocyPack(
      /*querySlice*/ VPackSlice::emptyObjectSlice(), collections, variables,
      /*snippets*/ snippetBuilder.slice(), analyzersRevision, aborter);

  aql::QueryResult queryResult = query->executeSync(aborter);

  if (queryResult.result.fail()) {
    TRI_V8_THROW_EXCEPTION_FULL(queryResult.result.errorNumber(),
                                queryResult.result.errorMessage());
  }

  // return the array value as it is. this is a performance optimization
  v8::Handle<v8::Object> result = v8::Object::New(isolate);
  if (queryResult.data != nullptr) {
    result
        ->Set(context, TRI_V8_ASCII_STRING(isolate, "json"),
              TRI_VPackToV8(isolate, queryResult.data->slice(),
                            queryResult.context->getVPackOptions()))
        .FromMaybe(false);
  }
  if (queryResult.extra != nullptr) {
    VPackSlice stats = queryResult.extra->slice().get("stats");
    if (!stats.isNone()) {
      result
          ->Set(context, TRI_V8_ASCII_STRING(isolate, "stats"),
                TRI_VPackToV8(isolate, stats))
          .FromMaybe(false);
    }
    VPackSlice profile = queryResult.extra->slice().get("profile");
    if (!profile.isNone()) {
      result
          ->Set(context, TRI_V8_ASCII_STRING(isolate, "profile"),
                TRI_VPackToV8(isolate, profile))
          .FromMaybe(false);
    }
  }

  if (queryResult.extra == nullptr ||
      !queryResult.extra->slice().hasKey("warnings")) {
    result
        ->Set(context, TRI_V8_ASCII_STRING(isolate, "warnings"),
              v8::Array::New(isolate))
        .FromMaybe(false);
  } else {
    result
        ->Set(
            context, TRI_V8_ASCII_STRING(isolate, "warnings"),
            TRI_VPackToV8(isolate, queryResult.extra->slice().get("warnings")))
        .FromMaybe(false);
  }
  result
      ->Set(context, TRI_V8_ASCII_STRING(isolate, "cached"),
            v8::Boolean::New(isolate, queryResult.cached))
      .FromMaybe(false);

  TRI_V8_RETURN(result);
  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief executes an AQL query
////////////////////////////////////////////////////////////////////////////////

static void JS_ExecuteAql(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);
  auto context = TRI_IGETC;
  auto& vocbase = GetContextVocBase(isolate);

  if (args.Length() < 1 || args.Length() > 3) {
    TRI_V8_THROW_EXCEPTION_USAGE(
        "AQL_EXECUTE(<queryString>, <bindVars>, <options>)");
  }

  // get the query string
  if (!args[0]->IsString()) {
    TRI_V8_THROW_TYPE_ERROR("expecting string for <queryString>");
  }

  std::string queryString(TRI_ObjectToString(isolate, args[0]));

  // bind parameters
  std::shared_ptr<VPackBuilder> bindVars;

  if (args.Length() > 1) {
    if (!args[1]->IsUndefined() && !args[1]->IsNull() && !args[1]->IsObject()) {
      TRI_V8_THROW_TYPE_ERROR("expecting object for <bindVars>");
    }
    if (args[1]->IsObject()) {
      bindVars = std::make_shared<VPackBuilder>();
      TRI_V8ToVPack(isolate, *(bindVars.get()), args[1], false);
    }
  }

  // options
  VPackBuilder options;
  if (args.Length() > 2) {
    if (!args[2]->IsObject()) {
      TRI_V8_THROW_TYPE_ERROR("expecting object for <options>");
    }
    TRI_V8ToVPack(isolate, options, args[2], false);
  }

  TRI_GET_GLOBALS();
  auto origin = transaction::OperationOriginAQL{"executing query"};
  auto v8Context = transaction::V8Context::create(vocbase, origin, true);
  if (v8g->_transactionContext != nullptr) {
    if (v8g->_transactionContext->isTransactionJS()) {
      v8Context->setJStransaction();
    }
    if (v8g->_transactionContext->isReadOnlyTransaction()) {
      v8Context->setReadOnly();
    }
  }
  auto query = arangodb::aql::Query::create(
      std::move(v8Context), aql::QueryString(std::move(queryString)),
      std::move(bindVars), aql::QueryOptions(options.slice()));
  auto aborter = std::make_shared<aql::QueryAborter>(query);
  arangodb::aql::QueryResultV8 queryResult = query->executeV8(isolate, aborter);

  if (queryResult.result.fail()) {
    if (queryResult.result.is(TRI_ERROR_REQUEST_CANCELED)) {
      TRI_GET_GLOBALS();
      v8g->_canceled = true;
    }
    TRI_V8_THROW_EXCEPTION_FULL(queryResult.result.errorNumber(),
                                queryResult.result.errorMessage());
  }

  // return the array value as it is. this is a performance optimization
  v8::Handle<v8::Object> result = v8::Object::New(isolate);

  if (!queryResult.v8Data.IsEmpty()) {
    result
        ->Set(context, TRI_V8_ASCII_STRING(isolate, "json"), queryResult.v8Data)
        .FromMaybe(false);
  }

  if (queryResult.extra != nullptr) {
    VPackSlice extra = queryResult.extra->slice();
    VPackSlice stats = extra.get("stats");
    if (!stats.isNone()) {
      result
          ->Set(context, TRI_V8_ASCII_STRING(isolate, "stats"),
                TRI_VPackToV8(isolate, stats))
          .FromMaybe(false);
    }
    VPackSlice warnings = extra.get("warnings");
    if (warnings.isNone()) {
      result
          ->Set(context, TRI_V8_ASCII_STRING(isolate, "warnings"),
                v8::Array::New(isolate))
          .FromMaybe(false);
    } else {
      result
          ->Set(context, TRI_V8_ASCII_STRING(isolate, "warnings"),
                TRI_VPackToV8(isolate, warnings))
          .FromMaybe(false);
    }
    VPackSlice profile = extra.get("profile");
    if (!profile.isNone()) {
      result
          ->Set(context, TRI_V8_ASCII_STRING(isolate, "profile"),
                TRI_VPackToV8(isolate, profile))
          .FromMaybe(false);
    }
    VPackSlice plan = extra.get("plan");
    if (!plan.isNone()) {
      result
          ->Set(context, TRI_V8_ASCII_STRING(isolate, "plan"),
                TRI_VPackToV8(isolate, plan))
          .FromMaybe(false);
    }
  }

  result
      ->Set(context, TRI_V8_ASCII_STRING(isolate, "cached"),
            v8::Boolean::New(isolate, queryResult.cached))
      .FromMaybe(false);

  TRI_V8_RETURN(result);
  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief retrieve global query options or configure them
////////////////////////////////////////////////////////////////////////////////

static void JS_QueriesPropertiesAql(
    v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);
  auto context = TRI_IGETC;
  auto& vocbase = GetContextVocBase(isolate);
  auto* queryList = vocbase.queryList();
  TRI_ASSERT(queryList != nullptr);

  if (args.Length() > 1) {
    TRI_V8_THROW_EXCEPTION_USAGE("AQL_QUERIES_PROPERTIES(<options>)");
  }

  if (args.Length() == 1) {
    // store options
    if (!args[0]->IsObject()) {
      TRI_V8_THROW_EXCEPTION_USAGE("AQL_QUERIES_PROPERTIES(<options>)");
    }

    auto obj = args[0]->ToObject(context).FromMaybe(v8::Handle<v8::Object>());
    if (TRI_HasProperty(context, isolate, obj, "enabled")) {
      queryList->enabled(TRI_ObjectToBoolean(
          isolate, obj->Get(context, TRI_V8_ASCII_STRING(isolate, "enabled"))
                       .FromMaybe(v8::Local<v8::Value>())));
    }
    if (TRI_HasProperty(context, isolate, obj, "trackSlowQueries")) {
      queryList->trackSlowQueries(TRI_ObjectToBoolean(
          isolate,
          obj->Get(context, TRI_V8_ASCII_STRING(isolate, "trackSlowQueries"))
              .FromMaybe(v8::Local<v8::Value>())));
    }
    if (TRI_HasProperty(context, isolate, obj, "trackBindVars")) {
      queryList->trackBindVars(TRI_ObjectToBoolean(
          isolate,
          obj->Get(context, TRI_V8_ASCII_STRING(isolate, "trackBindVars"))
              .FromMaybe(v8::Local<v8::Value>())));
    }
    if (TRI_HasProperty(context, isolate, obj, "maxSlowQueries")) {
      queryList->maxSlowQueries(static_cast<size_t>(TRI_ObjectToInt64(
          isolate,
          obj->Get(context, TRI_V8_ASCII_STRING(isolate, "maxSlowQueries"))
              .FromMaybe(v8::Local<v8::Value>()))));
    }
    if (TRI_HasProperty(context, isolate, obj, "slowQueryThreshold")) {
      queryList->slowQueryThreshold(TRI_ObjectToDouble(
          isolate,
          obj->Get(context, TRI_V8_ASCII_STRING(isolate, "slowQueryThreshold"))
              .FromMaybe(v8::Local<v8::Value>())));
    }
    if (TRI_HasProperty(context, isolate, obj, "slowStreamingQueryThreshold")) {
      queryList->slowStreamingQueryThreshold(TRI_ObjectToDouble(
          isolate,
          obj->Get(context,
                   TRI_V8_ASCII_STRING(isolate, "slowStreamingQueryThreshold"))
              .FromMaybe(v8::Local<v8::Value>())));
    }
    if (TRI_HasProperty(context, isolate, obj, "maxQueryStringLength")) {
      queryList->maxQueryStringLength(static_cast<size_t>(TRI_ObjectToInt64(
          isolate, obj->Get(context, TRI_V8_ASCII_STRING(
                                         isolate, "maxQueryStringLength"))
                       .FromMaybe(v8::Local<v8::Value>()))));
    }

    // intentionally falls through
  }

  // return current settings
  auto result = v8::Object::New(isolate);
  result
      ->Set(context, TRI_V8_ASCII_STRING(isolate, "enabled"),
            v8::Boolean::New(isolate, queryList->enabled()))
      .FromMaybe(false);
  result
      ->Set(context, TRI_V8_ASCII_STRING(isolate, "trackSlowQueries"),
            v8::Boolean::New(isolate, queryList->trackSlowQueries()))
      .FromMaybe(false);
  result
      ->Set(context, TRI_V8_ASCII_STRING(isolate, "trackBindVars"),
            v8::Boolean::New(isolate, queryList->trackBindVars()))
      .FromMaybe(false);
  result
      ->Set(context, TRI_V8_ASCII_STRING(isolate, "maxSlowQueries"),
            v8::Number::New(isolate,
                            static_cast<double>(queryList->maxSlowQueries())))
      .FromMaybe(false);
  result
      ->Set(context, TRI_V8_ASCII_STRING(isolate, "slowQueryThreshold"),
            v8::Number::New(isolate, queryList->slowQueryThreshold()))
      .FromMaybe(false);
  result
      ->Set(context,
            TRI_V8_ASCII_STRING(isolate, "slowStreamingQueryThreshold"),
            v8::Number::New(isolate, queryList->slowStreamingQueryThreshold()))
      .FromMaybe(false);
  result
      ->Set(context, TRI_V8_ASCII_STRING(isolate, "maxQueryStringLength"),
            v8::Number::New(isolate, static_cast<double>(
                                         queryList->maxQueryStringLength())))
      .FromMaybe(false);

  TRI_V8_RETURN(result);
  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief returns the list of currently running queries
////////////////////////////////////////////////////////////////////////////////

static void JS_QueriesCurrentAql(
    v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);

  if (args.Length() > 1) {
    TRI_V8_THROW_EXCEPTION_USAGE("AQL_QUERIES_CURRENT(params)");
  }

  auto& vocbase = GetContextVocBase(isolate);
  bool allDatabases = false;
  if (args.Length() > 0) {
    if (args[0]->IsObject()) {
      v8::Handle<v8::Object> obj =
          args[0]->ToObject(TRI_IGETC).FromMaybe(v8::Local<v8::Object>());
      allDatabases = TRI_GetOptionalBooleanProperty(isolate, obj, "all", false);
    } else {
      allDatabases = TRI_ObjectToBoolean(isolate, args[0]);
    }
  }

  bool const fanout = ServerState::instance()->isCoordinator();

  VPackBuilder b;
  Result res = methods::Queries::listCurrent(vocbase, b, allDatabases, fanout);

  if (res.fail()) {
    TRI_V8_THROW_EXCEPTION(res);
  }
  TRI_V8_RETURN(TRI_VPackToV8(isolate, b.slice()));

  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief returns the list of slow running queries
////////////////////////////////////////////////////////////////////////////////

static void JS_QueriesSlowAql(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);

  if (args.Length() > 1) {
    TRI_V8_THROW_EXCEPTION_USAGE("AQL_QUERIES_SLOW(params)");
  }

  auto& vocbase = GetContextVocBase(isolate);
  bool allDatabases = false;
  if (args.Length() > 0) {
    if (args[0]->IsObject()) {
      v8::Handle<v8::Object> obj =
          args[0]->ToObject(TRI_IGETC).FromMaybe(v8::Local<v8::Object>());
      allDatabases = TRI_GetOptionalBooleanProperty(isolate, obj, "all", false);
    } else {
      allDatabases = TRI_ObjectToBoolean(isolate, args[0]);
    }
  }

  bool const fanout = ServerState::instance()->isCoordinator();

  VPackBuilder b;
  Result res = methods::Queries::listSlow(vocbase, b, allDatabases, fanout);

  if (res.fail()) {
    TRI_V8_THROW_EXCEPTION(res);
  }
  TRI_V8_RETURN(TRI_VPackToV8(isolate, b.slice()));

  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief clears the list of slow queries
////////////////////////////////////////////////////////////////////////////////

static void JS_QueriesClearSlowAql(
    v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);

  if (args.Length() > 1) {
    TRI_V8_THROW_EXCEPTION_USAGE("AQL_QUERIES_CLEAR_SLOW(params)");
  }

  auto& vocbase = GetContextVocBase(isolate);
  bool allDatabases = false;
  if (args.Length() > 0) {
    if (args[0]->IsObject()) {
      v8::Handle<v8::Object> obj =
          args[0]->ToObject(TRI_IGETC).FromMaybe(v8::Local<v8::Object>());
      allDatabases = TRI_GetOptionalBooleanProperty(isolate, obj, "all", false);
    } else {
      allDatabases = TRI_ObjectToBoolean(isolate, args[0]);
    }
  }

  bool const fanout = ServerState::instance()->isCoordinator();

  Result res = methods::Queries::clearSlow(vocbase, allDatabases, fanout);

  if (res.fail()) {
    TRI_V8_THROW_EXCEPTION(res);
  }
  TRI_V8_RETURN_TRUE();

  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief kills an AQL query
////////////////////////////////////////////////////////////////////////////////

static void JS_QueriesKillAql(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);

  if (args.Length() != 1) {
    TRI_V8_THROW_EXCEPTION_USAGE("AQL_QUERIES_KILL(<params>)");
  }

  auto& vocbase = GetContextVocBase(isolate);
  uint64_t id = 0;
  bool allDatabases = false;
  if (args.Length() > 0) {
    if (args[0]->IsObject()) {
      auto context = TRI_IGETC;

      v8::Handle<v8::Object> obj =
          args[0]->ToObject(TRI_IGETC).FromMaybe(v8::Local<v8::Object>());
      allDatabases = TRI_GetOptionalBooleanProperty(isolate, obj, "all", false);
      id = TRI_GetOptionalBooleanProperty(isolate, obj, "all", false);
      if (TRI_HasProperty(context, isolate, obj, "id")) {
        id = TRI_ObjectToUInt64(
            isolate,
            obj->Get(context, TRI_V8_ASCII_STRING(isolate, "id"))
                .FromMaybe(v8::Local<v8::Value>()),
            true);
      }
    } else {
      id = TRI_ObjectToUInt64(isolate, args[0], true);
    }
  }

  Result res = methods::Queries::kill(vocbase, id, allDatabases);

  if (res.fail()) {
    TRI_V8_THROW_EXCEPTION(res);
  }
  TRI_V8_RETURN_TRUE();

  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief configures the AQL query cache
////////////////////////////////////////////////////////////////////////////////

static void JS_QueryCachePropertiesAql(
    v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);

  if (args.Length() > 1 || (args.Length() == 1 && !args[0]->IsObject())) {
    TRI_V8_THROW_EXCEPTION_USAGE("AQL_QUERY_CACHE_PROPERTIES(<properties>)");
  }

  auto queryCache = arangodb::aql::QueryCache::instance();
  VPackBuilder builder;

  if (args.Length() == 1) {
    // called with options
    TRI_V8ToVPack(isolate, builder, args[0], false);
    queryCache->properties(builder.slice());
  }

  builder.clear();
  queryCache->toVelocyPack(builder);
  TRI_V8_RETURN(TRI_VPackToV8(isolate, builder.slice()));

  // fetch current configuration and return it
  TRI_V8_TRY_CATCH_END
}

static void JS_QueryCacheQueriesAql(
    v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);

  if (args.Length() != 0) {
    TRI_V8_THROW_EXCEPTION_USAGE("AQL_QUERY_CACHE_QUERIES()");
  }

  auto& vocbase = GetContextVocBase(isolate);

  VPackBuilder builder;
  arangodb::aql::QueryCache::instance()->queriesToVelocyPack(&vocbase, builder);
  TRI_V8_RETURN(TRI_VPackToV8(isolate, builder.slice()));
  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief invalidates the AQL query cache
////////////////////////////////////////////////////////////////////////////////

static void JS_QueryCacheInvalidateAql(
    v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);

  if (args.Length() != 0) {
    TRI_V8_THROW_EXCEPTION_USAGE("AQL_QUERY_CACHE_INVALIDATE()");
  }

  arangodb::aql::QueryCache::instance()->invalidate();
  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief wraps a TRI_vocbase_t
////////////////////////////////////////////////////////////////////////////////

static v8::Handle<v8::Object> WrapVocBase(v8::Isolate* isolate,
                                          TRI_vocbase_t* database) {
  TRI_GET_GLOBALS();

  v8::Handle<v8::Object> result =
      WrapClass(isolate, v8g->VocbaseTempl, WRP_VOCBASE_TYPE, database);
  return result;
}

static void MapGetVocBase(v8::Local<v8::Name> const name,
                          v8::PropertyCallbackInfo<v8::Value> const& args) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope scope(isolate);
  auto context = TRI_IGETC;
  auto& vocbase = GetContextVocBase(isolate);

  // convert the JavaScript string to a string
  v8::String::Utf8Value s(isolate, name);
  char* key = *s;

  size_t keyLength = s.length();
  if (keyLength > 2 && key[keyLength - 2] == '(') {
    keyLength -= 2;
    key[keyLength] = '\0';
  }

  // empty or null
  if (key == nullptr || *key == '\0') {
    TRI_V8_RETURN(v8::Handle<v8::Value>());
  }

  if (strcmp(key, "hasOwnProperty") == 0 ||  // this prevents calling the
                                             // property getter again (i.e.
                                             // recursion!)
      strcmp(key, "toString") == 0 || strcmp(key, "toJSON") == 0) {
    TRI_V8_RETURN(v8::Handle<v8::Value>());
  }

  // generate a name under which the cached property is stored
  std::string cacheKey(key, keyLength);
  cacheKey.push_back('*');

  v8::Local<v8::String> cacheName = TRI_V8_STD_STRING(isolate, cacheKey);
  v8::Handle<v8::Object> holder =
      args.Holder()->ToObject(context).FromMaybe(v8::Local<v8::Object>());

  if (*key == '_') {
    // special treatment for all properties starting with _
    v8::Local<v8::String> const l =
        TRI_V8_PAIR_STRING(isolate, key, (int)keyLength);

    if (TRI_HasRealNamedProperty(context, isolate, holder, l)) {
      // some internal function inside db
      TRI_V8_RETURN(v8::Handle<v8::Value>());
    }

    // something in the prototype chain?
    v8::Local<v8::Value> v =
        holder->GetRealNamedPropertyInPrototypeChain(context, l)
            .FromMaybe(v8::Local<v8::Value>());

    if (!v.IsEmpty()) {
      if (!v->IsExternal()) {
        // something but an external... this means we can directly return this
        TRI_V8_RETURN(v8::Handle<v8::Value>());
      }
    }
  }

  TRI_GET_GLOBALS();

  auto globals = isolate->GetCurrentContext()->Global();

  v8::Handle<v8::Object> cacheObject;
  TRI_GET_GLOBAL_STRING(_DbCacheKey);
  if (TRI_HasProperty(context, isolate, globals, _DbCacheKey)) {
    cacheObject = globals->Get(context, _DbCacheKey)
                      .FromMaybe(v8::Local<v8::Value>())
                      ->ToObject(context)
                      .FromMaybe(v8::Local<v8::Object>());
  }

  if (!cacheObject.IsEmpty() &&
      TRI_HasRealNamedProperty(context, isolate, cacheObject, cacheName)) {
    v8::Handle<v8::Object> value =
        cacheObject->GetRealNamedProperty(context, cacheName)
            .FromMaybe(v8::Local<v8::Value>())
            ->ToObject(context)
            .FromMaybe(v8::Local<v8::Object>());
    auto* collection = UnwrapCollection(isolate, value);

    // check if the collection is from the same database and
    // hasn't been deleted
    if (!ServerState::instance()->isCoordinator() && collection &&
        &(collection->vocbase()) == &vocbase && !collection->deleted()) {
      if (auto cid = collection->id(); cid.isSet()) {
        TRI_GET_GLOBAL_STRING(_IdKey);
        if (TRI_HasProperty(context, isolate, value, _IdKey)) {
          DataSourceId cachedCid{TRI_ObjectToUInt64(
              isolate,
              value->Get(context, _IdKey).FromMaybe(v8::Local<v8::Value>()),
              true)};

          TRI_GET_GLOBAL_STRING(VersionKeyHidden);
          uint32_t cachedVersion = static_cast<uint32_t>(TRI_ObjectToInt64(
              isolate, value->Get(context, VersionKeyHidden)
                           .FromMaybe(v8::Local<v8::Value>())));
          auto internalVersion = collection->v8CacheVersion();

          if (cachedCid == cid && cachedVersion == internalVersion) {
            // cache hit
            TRI_V8_RETURN(value);
          }
          // store the updated version number in the object for future
          // comparisons
          value
              ->DefineOwnProperty(
                  context, VersionKeyHidden,
                  v8::Number::New(isolate, (double)internalVersion),
                  v8::DontEnum)
              .FromMaybe(false);  // Ignore result...

          // cid has changed (i.e. collection has been dropped and re-created)
          // or version has changed
        }
      }
    }

    // cache miss
    cacheObject->Delete(context, cacheName).FromMaybe(false);  // Ignore result
  }

  std::shared_ptr<arangodb::LogicalCollection> collection;

  if (ServerState::instance()->isCoordinator()) {
    if (vocbase.server().hasFeature<ClusterFeature>()) {
      collection = vocbase.server()
                       .getFeature<ClusterFeature>()
                       .clusterInfo()
                       .getCollectionNT(vocbase.name(), std::string(key));
    }
  } else {
    collection = vocbase.lookupCollection(std::string_view(key, keyLength));
  }

  if (collection == nullptr) {
    if (*key == '_') {
      TRI_V8_RETURN(v8::Handle<v8::Value>());
    }

    TRI_V8_RETURN_UNDEFINED();
  }

  v8::Handle<v8::Value> result = WrapCollection(isolate, collection);

  if (result.IsEmpty()) {
    TRI_V8_RETURN_UNDEFINED();
  }

  if (!cacheObject.IsEmpty()) {
    cacheObject->Set(context, cacheName, result).FromMaybe(false);
  }

  TRI_V8_RETURN(result);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief return the name and capabilities of the storage engine
////////////////////////////////////////////////////////////////////////////////

static void JS_Engine(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);

  // return engine data
  TRI_GET_SERVER_GLOBALS(ArangodServer);
  StorageEngine& engine =
      v8g->server().getFeature<EngineSelectorFeature>().engine();
  VPackBuilder builder;
  engine.getCapabilities(builder);

  TRI_V8_RETURN(TRI_VPackToV8(isolate, builder.slice()));

  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief return statistics for the storage engine
////////////////////////////////////////////////////////////////////////////////

static void JS_EngineStats(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);

  // return engine data
  TRI_GET_SERVER_GLOBALS(ArangodServer);
  StorageEngine& engine =
      v8g->server().getFeature<EngineSelectorFeature>().engine();
  VPackBuilder builder;
  engine.getStatistics(builder);

  v8::Handle<v8::Value> result = TRI_VPackToV8(isolate, builder.slice());
  TRI_V8_RETURN(result);
  TRI_V8_TRY_CATCH_END
}

static void JS_VersionServer(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);

  bool details = false;
  if (args.Length() > 0) {
    details = TRI_ObjectToBoolean(isolate, args[0]);
  }

  if (!details) {
    // return version string
    TRI_V8_RETURN(TRI_V8_ASCII_STRING(isolate, ARANGODB_VERSION));
  }

  TRI_GET_SERVER_GLOBALS(ArangodServer);
  VPackBuilder builder;
  arangodb::RestVersionHandler::getVersion(v8g->server(), true, true, builder);

  TRI_V8_RETURN(TRI_VPackToV8(isolate, builder.slice()));
  TRI_V8_TRY_CATCH_END
}

static void JS_PathDatabase(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);
  TRI_GET_SERVER_GLOBALS(ArangodServer);
  StorageEngine& engine =
      v8g->server().getFeature<EngineSelectorFeature>().engine();

  TRI_V8_RETURN_STD_STRING(engine.databasePath());
  TRI_V8_TRY_CATCH_END
}

static void JS_VersionFilenameDatabase(
    v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);
  auto& vocbase = GetContextVocBase(isolate);
  TRI_GET_SERVER_GLOBALS(ArangodServer);
  StorageEngine& engine =
      v8g->server().getFeature<EngineSelectorFeature>().engine();

  TRI_V8_RETURN_STD_STRING(engine.versionFilename(vocbase.id()));
  TRI_V8_TRY_CATCH_END
}

static void JS_IdDatabase(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);
  auto& vocbase = GetContextVocBase(isolate);

  TRI_V8_RETURN(TRI_V8UInt64String<TRI_voc_tick_t>(isolate, vocbase.id()));
  TRI_V8_TRY_CATCH_END
}

static void JS_NameDatabase(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);
  auto& vocbase = GetContextVocBase(isolate);
  auto& n = vocbase.name();

  TRI_V8_RETURN_STD_STRING(n);
  TRI_V8_TRY_CATCH_END
}

static void JS_IsSystemDatabase(
    v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);
  auto& vocbase = GetContextVocBase(isolate);

  TRI_V8_RETURN(v8::Boolean::New(isolate, vocbase.isSystem()));
  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief fake this method so the interface is similar to the client.
////////////////////////////////////////////////////////////////////////////////

static void JS_FakeFlushCache(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  TRI_V8_RETURN_UNDEFINED();
  TRI_V8_TRY_CATCH_END;
}

static void JS_UseDatabase(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);

  if (args.Length() != 1) {
    TRI_V8_THROW_EXCEPTION_USAGE("db._useDatabase(<name>)");
  }

  TRI_GET_SERVER_GLOBALS(ArangodServer);

  if (!v8g->_securityContext.canUseDatabase()) {
    TRI_V8_THROW_EXCEPTION(TRI_ERROR_FORBIDDEN);
  }

  auto& databaseFeature = v8g->server().getFeature<DatabaseFeature>();
  std::string const name = TRI_ObjectToString(isolate, args[0]);

  if (auto* vocbase = &GetContextVocBase(isolate);
      vocbase->isDropped() && name != StaticStrings::SystemDatabase) {
    // still allow changing back into the _system database even if
    // the current database has been dropped
    TRI_V8_THROW_EXCEPTION(TRI_ERROR_ARANGO_DATABASE_NOT_FOUND);
  }

  // check if the other database exists, and increase its refcount
  auto vocbase = databaseFeature.useDatabase(name);

  if (vocbase == nullptr) {
    TRI_V8_THROW_EXCEPTION(TRI_ERROR_ARANGO_DATABASE_NOT_FOUND);
  }

  TRI_ASSERT(!vocbase->isDangling());

  // switch databases
  VocbasePtr orig{std::exchange(v8g->_vocbase, vocbase.release())};
  TRI_ASSERT(orig != nullptr);
  orig.reset();

  TRI_V8_RETURN(WrapVocBase(isolate, v8g->_vocbase));
  TRI_V8_TRY_CATCH_END
}

static void JS_Databases(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);
  auto context = TRI_IGETC;

  uint32_t const argc = args.Length();
  if (argc > 1) {
    TRI_V8_THROW_EXCEPTION_USAGE("db._databases()");
  }

  auto& vocbase = GetContextVocBase(isolate);

  if (argc == 0 && !vocbase.isSystem()) {
    TRI_V8_THROW_EXCEPTION(TRI_ERROR_ARANGO_USE_SYSTEM_DATABASE);
  }

  std::string user;

  if (argc > 0) {
    user = TRI_ObjectToString(isolate, args[0]);
  }

  TRI_GET_SERVER_GLOBALS(ArangodServer);
  std::vector<std::string> names =
      methods::Databases::list(v8g->server(), user);
  v8::Handle<v8::Array> result = v8::Array::New(isolate, (int)names.size());

  for (size_t i = 0; i < names.size(); ++i) {
    result->Set(context, (uint32_t)i, TRI_V8_STD_STRING(isolate, names[i]))
        .FromMaybe(false);
  }

  TRI_V8_RETURN(result);
  TRI_V8_TRY_CATCH_END
}

static void JS_CreateDatabase(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);
  auto context = TRI_IGETC;

  if (args.Length() < 1 || args.Length() > 3) {
    events::CreateDatabase("", Result(TRI_ERROR_BAD_PARAMETER),
                           ExecContext::current());
    TRI_V8_THROW_EXCEPTION_USAGE(
        "db._createDatabase(<name>, <options>, <users>)");
  }

  auto& vocbase = GetContextVocBase(isolate);

  TRI_ASSERT(!vocbase.isDangling());

  if (!vocbase.isSystem()) {
    events::CreateDatabase("", Result(TRI_ERROR_ARANGO_USE_SYSTEM_DATABASE),
                           ExecContext::current());
    TRI_V8_THROW_EXCEPTION(TRI_ERROR_ARANGO_USE_SYSTEM_DATABASE);
  }

  VPackBuilder options;
  if (args.Length() >= 2 && args[1]->IsObject()) {
    TRI_V8ToVPack(isolate, options, args[1], false);
  } else {
    options.openObject();
    options.close();
  }

  VPackBuilder users;

  if (args.Length() >= 3 && args[2]->IsArray()) {
    VPackArrayBuilder a(&users);
    v8::Handle<v8::Array> ar = v8::Handle<v8::Array>::Cast(args[2]);

    for (uint32_t i = 0; i < ar->Length(); ++i) {
      v8::Handle<v8::Value> user =
          ar->Get(context, i).FromMaybe(v8::Local<v8::Value>());

      if (!user->IsObject()) {
        events::CreateDatabase("", Result(TRI_ERROR_BAD_PARAMETER),
                               ExecContext::current());
        TRI_V8_THROW_EXCEPTION_MESSAGE(TRI_ERROR_BAD_PARAMETER,
                                       "user is not an object");
      }

      TRI_V8ToVPack(isolate, users, user, false, false);
    }
  }

  std::string const dbName = TRI_ObjectToString(isolate, args[0]);
  Result res =
      methods::Databases::create(vocbase.server(), ExecContext::current(),
                                 dbName, users.slice(), options.slice());

  if (res.fail()) {
    TRI_V8_THROW_EXCEPTION(res);
  }

  TRI_V8_RETURN_TRUE();
  TRI_V8_TRY_CATCH_END
}

static void JS_DropDatabase(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);

  if (args.Length() != 1) {
    events::DropDatabase("", Result(TRI_ERROR_BAD_PARAMETER),
                         ExecContext::current());
    TRI_V8_THROW_EXCEPTION_USAGE("db._dropDatabase(<name>)");
  }

  auto& vocbase = GetContextVocBase(isolate);

  if (!vocbase.isSystem()) {
    events::DropDatabase("", Result(TRI_ERROR_ARANGO_USE_SYSTEM_DATABASE),
                         ExecContext::current());
    TRI_V8_THROW_EXCEPTION(TRI_ERROR_ARANGO_USE_SYSTEM_DATABASE);
  }

  std::string const name = TRI_ObjectToString(isolate, args[0]);
  auto res = methods::Databases::drop(ExecContext::current(), &vocbase, name);

  if (res.fail()) {
    TRI_V8_THROW_EXCEPTION(res);
  }

  TRI_V8_RETURN_TRUE();
  TRI_V8_TRY_CATCH_END
}

static void JS_DBProperties(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);

  uint32_t const argc = args.Length();
  if (argc > 0) {
    TRI_V8_THROW_EXCEPTION_USAGE("db._properties()");
  }

  auto& vocbase = GetContextVocBase(isolate);

  VPackBuilder builder;
  vocbase.toVelocyPack(builder);

  auto result = TRI_VPackToV8(isolate, builder.slice());

  TRI_V8_RETURN(result);
  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief returns a list of all endpoints
///
/// @FUN{ENDPOINTS}
////////////////////////////////////////////////////////////////////////////////

static void JS_Endpoints(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);
  auto context = TRI_IGETC;

  if (args.Length() != 0) {
    TRI_V8_THROW_EXCEPTION_USAGE("db._endpoints()");
  }

  TRI_GET_SERVER_GLOBALS(ArangodServer);
  TRI_ASSERT(v8g->server().hasFeature<HttpEndpointProvider>());
  auto& endpoints = v8g->server().getFeature<HttpEndpointProvider>();
  auto& vocbase = GetContextVocBase(isolate);

  if (!vocbase.isSystem()) {
    TRI_V8_THROW_EXCEPTION(TRI_ERROR_ARANGO_USE_SYSTEM_DATABASE);
  }

  v8::Handle<v8::Array> result = v8::Array::New(isolate);
  uint32_t j = 0;

  for (auto const& it : endpoints.httpEndpoints()) {
    v8::Handle<v8::Object> item = v8::Object::New(isolate);
    item->Set(context, TRI_V8_ASCII_STRING(isolate, "endpoint"),
              TRI_V8_STD_STRING(isolate, it))
        .FromMaybe(false);

    result->Set(context, j++, item).FromMaybe(false);
  }

  TRI_V8_RETURN(result);
  TRI_V8_TRY_CATCH_END
}

static void JS_TrustedProxies(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  auto context = TRI_IGETC;

  TRI_GET_SERVER_GLOBALS(ArangodServer);
  auto& gs = v8g->server().getFeature<GeneralServerFeature>();
  if (gs.proxyCheck()) {
    v8::Handle<v8::Array> result = v8::Array::New(isolate);

    uint32_t i = 0;
    for (auto const& proxyDef : gs.trustedProxies()) {
      result->Set(context, i++, TRI_V8_STD_STRING(isolate, proxyDef))
          .FromMaybe(false);
    }
    TRI_V8_RETURN(result);
  } else {
    TRI_V8_RETURN(v8::Null(isolate));
  }

  TRI_V8_TRY_CATCH_END
}

static void JS_AuthenticationEnabled(
    v8::FunctionCallbackInfo<v8::Value> const& args) {
  // mop: one could argue that this is a function because this might be
  // changable on the fly at some time but the sad truth is server startup
  // order
  // v8 is initialized after GeneralServerFeature
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);

  TRI_GET_SERVER_GLOBALS(ArangodServer);
  auto& authentication = v8g->server().getFeature<AuthenticationFeature>();

  v8::Handle<v8::Boolean> result =
      v8::Boolean::New(isolate, authentication.isActive());

  TRI_V8_RETURN(result);
  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief decode a _rev time stamp
////////////////////////////////////////////////////////////////////////////////

static void JS_DecodeRev(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);
  auto context = TRI_IGETC;

  if (args.Length() != 1 || !args[0]->IsString()) {
    TRI_V8_THROW_EXCEPTION_USAGE("DECODE_REV(<string>)");
  }

  std::string rev = TRI_ObjectToString(isolate, args[0]);
  uint64_t revInt = HybridLogicalClock::decodeTimeStamp(rev);
  v8::Handle<v8::Object> result = v8::Object::New(isolate);
  if (revInt == UINT64_MAX) {
    result
        ->Set(context, TRI_V8_ASCII_STRING(isolate, "date"),
              TRI_V8_ASCII_STRING(isolate, "illegal"))
        .FromMaybe(false);
    result
        ->Set(context, TRI_V8_ASCII_STRING(isolate, "count"),
              v8::Number::New(isolate, 0.0))
        .FromMaybe(false);
  } else {
    uint64_t timeMilli = HybridLogicalClock::extractTime(revInt);
    uint64_t count = HybridLogicalClock::extractCount(revInt);

    time_t timeSeconds = timeMilli / 1000;
    uint64_t millis = timeMilli % 1000;
    struct tm date;
    TRI_gmtime(timeSeconds, &date);
    char buffer[32];
    strftime(buffer, 32, "%Y-%m-%dT%H:%M:%S.000Z", &date);
    buffer[20] = static_cast<char>(millis / 100) + '0';
    buffer[21] = ((millis / 10) % 10) + '0';
    buffer[22] = (millis % 10) + '0';
    buffer[24] = 0;

    result
        ->Set(context, TRI_V8_ASCII_STRING(isolate, "date"),
              TRI_V8_ASCII_STRING(isolate, buffer))
        .FromMaybe(false);
    result
        ->Set(context, TRI_V8_ASCII_STRING(isolate, "count"),
              v8::Number::New(isolate, static_cast<double>(count)))
        .FromMaybe(false);
  }

  TRI_V8_RETURN(result);

  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief returns the current context
////////////////////////////////////////////////////////////////////////////////

void JS_ArangoDBContext(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);
  auto context = TRI_IGETC;

  if (args.Length() != 0) {
    TRI_V8_THROW_EXCEPTION_USAGE("ARANGODB_CONTEXT()");
  }

  v8::Handle<v8::Object> result = v8::Object::New(isolate);

  ExecContext const& exec = ExecContext::current();
  if (!exec.user().empty()) {
    result
        ->Set(context, TRI_V8_ASCII_STRING(isolate, "user"),
              TRI_V8_STD_STRING(isolate, exec.user()))
        .FromMaybe(false);
  }

  TRI_V8_RETURN(result);

  TRI_V8_TRY_CATCH_END;
}

/// @brief return a list of all wal files (empty list if not rocksdb)
static void JS_CurrentWalFiles(
    v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);
  auto context = TRI_IGETC;

  TRI_GET_SERVER_GLOBALS(ArangodServer);
  StorageEngine& engine =
      v8g->server().getFeature<EngineSelectorFeature>().engine();
  std::vector<std::string> names = engine.currentWalFiles();
  std::sort(names.begin(), names.end());

  // already create an array of the correct size
  uint32_t const n = static_cast<uint32_t>(names.size());
  v8::Handle<v8::Array> result = v8::Array::New(isolate, static_cast<int>(n));

  for (uint32_t i = 0; i < n; ++i) {
    result->Set(context, i, TRI_V8_STD_STRING(isolate, names[i]))
        .FromMaybe(false);
  }

  TRI_V8_RETURN(result);
  TRI_V8_TRY_CATCH_END
}

static void JS_SystemStatistics(
    v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);

  auto& vocbase = GetContextVocBase(isolate);

  if (args.Length() != 1 || !args[0]->IsNumber()) {
    TRI_V8_THROW_EXCEPTION_USAGE("SYSTEM_STATISTICS(start)");
  }

  double start = TRI_ObjectToDouble(isolate, args[0]);

  VPackBuilder builder;
  Result res = vocbase.server()
                   .getFeature<StatisticsFeature>()
                   .getClusterSystemStatistics(vocbase, start, builder);

  if (res.fail()) {
    TRI_V8_THROW_EXCEPTION(res);
  }

  v8::Handle<v8::Value> result = TRI_VPackToV8(isolate, builder.slice());

  TRI_V8_RETURN(result);
  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief this is for single server mode to dump an agency
////////////////////////////////////////////////////////////////////////////////

static void JS_AgencyDump(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);

  auto context = TRI_IGETC;
  auto& vocbase = GetContextVocBase(isolate);

  if (args.Length() != 0) {
    TRI_V8_THROW_EXCEPTION_USAGE("AGENCY_DUMP()");
  }

  uint64_t index = 0;
  uint64_t term = 0;
  auto b = arangodb::consensus::State::latestAgencyState(vocbase, index, term);

  v8::Handle<v8::Object> result = v8::Object::New(isolate);
  result
      ->Set(context, TRI_V8_ASCII_STRING(isolate, "index"),
            v8::Number::New(isolate, static_cast<double>(index)))
      .FromMaybe(false);
  result
      ->Set(context, TRI_V8_ASCII_STRING(isolate, "term"),
            v8::Number::New(isolate, static_cast<double>(term)))
      .FromMaybe(false);
  result
      ->Set(context, TRI_V8_ASCII_STRING(isolate, "data"),
            TRI_VPackToV8(isolate, b->slice()))
      .FromMaybe(false);

  TRI_V8_RETURN(result);
  TRI_V8_TRY_CATCH_END
}

#ifdef USE_ENTERPRISE

////////////////////////////////////////////////////////////////////////////////
/// @brief this is rotates the encryption keys, only for testing
////////////////////////////////////////////////////////////////////////////////

static void JS_EncryptionKeyReload(
    v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);

  if (args.Length() != 0) {
    TRI_V8_THROW_EXCEPTION_USAGE("encryptionKeyReload()");
  }

  TRI_GET_SERVER_GLOBALS(ArangodServer);
  auto& selector = v8g->server().getFeature<EngineSelectorFeature>();
  if (!selector.isRocksDB()) {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_NOT_IMPLEMENTED);
  }

  auto& engine = selector.engine<RocksDBEngine>();
  auto res = engine.rotateUserEncryptionKeys();
  if (res.fail()) {
    TRI_V8_THROW_EXCEPTION(res);
  }

  TRI_V8_RETURN_TRUE();
  TRI_V8_TRY_CATCH_END
}

#endif

////////////////////////////////////////////////////////////////////////////////
/// @brief creates a TRI_vocbase_t global context
////////////////////////////////////////////////////////////////////////////////

void TRI_InitV8VocBridge(v8::Isolate* isolate, v8::Handle<v8::Context> context,
                         arangodb::aql::QueryRegistry* /*queryRegistry*/,
                         TRI_vocbase_t& vocbase, size_t threadNumber) {
  auto& server = vocbase.server();

  v8::HandleScope scope(isolate);

  // check the isolate
  TRI_GET_SERVER_GLOBALS(ArangodServer);

  TRI_ASSERT(v8g->_transactionContext == nullptr);
  // register the database
  v8g->_vocbase = &vocbase;

  // ...........................................................................
  // generate the TRI_vocbase_t template
  // ...........................................................................

  v8::Handle<v8::FunctionTemplate> ft = v8::FunctionTemplate::New(isolate);
  ft->SetClassName(TRI_V8_ASCII_STRING(isolate, "ArangoDatabase"));

  v8::Handle<v8::ObjectTemplate> ArangoNS = ft->InstanceTemplate();
  ArangoNS->SetInternalFieldCount(2);

  ArangoNS->SetHandler(v8::NamedPropertyHandlerConfiguration(MapGetVocBase));

  // for any database function added here, be sure to add it to in function
  // JS_CompletionsVocbase, too for the auto-completion
  TRI_AddMethodVocbase(isolate, ArangoNS,
                       TRI_V8_ASCII_STRING(isolate, "_compact"), JS_Compact);

  TRI_AddMethodVocbase(isolate, ArangoNS,
                       TRI_V8_ASCII_STRING(isolate, "_engine"), JS_Engine);
  TRI_AddMethodVocbase(isolate, ArangoNS,
                       TRI_V8_ASCII_STRING(isolate, "_engineStats"),
                       JS_EngineStats);
  TRI_AddMethodVocbase(isolate, ArangoNS,
                       TRI_V8_ASCII_STRING(isolate, "_version"),
                       JS_VersionServer);
  TRI_AddMethodVocbase(isolate, ArangoNS, TRI_V8_ASCII_STRING(isolate, "_id"),
                       JS_IdDatabase);
  TRI_AddMethodVocbase(isolate, ArangoNS,
                       TRI_V8_ASCII_STRING(isolate, "_isSystem"),
                       JS_IsSystemDatabase);
  TRI_AddMethodVocbase(isolate, ArangoNS, TRI_V8_ASCII_STRING(isolate, "_name"),
                       JS_NameDatabase);
  TRI_AddMethodVocbase(isolate, ArangoNS, TRI_V8_ASCII_STRING(isolate, "_path"),
                       JS_PathDatabase);
  TRI_AddMethodVocbase(isolate, ArangoNS,
                       TRI_V8_ASCII_STRING(isolate, "_currentWalFiles"),
                       JS_CurrentWalFiles, true);
  TRI_AddMethodVocbase(isolate, ArangoNS,
                       TRI_V8_ASCII_STRING(isolate, "_versionFilename"),
                       JS_VersionFilenameDatabase, true);
  TRI_AddMethodVocbase(isolate, ArangoNS,
                       TRI_V8_ASCII_STRING(isolate, "_createDatabase"),
                       JS_CreateDatabase);
  TRI_AddMethodVocbase(isolate, ArangoNS,
                       TRI_V8_ASCII_STRING(isolate, "_dropDatabase"),
                       JS_DropDatabase);
  TRI_AddMethodVocbase(isolate, ArangoNS,
                       TRI_V8_ASCII_STRING(isolate, "_databases"),
                       JS_Databases);
  TRI_AddMethodVocbase(isolate, ArangoNS,
                       TRI_V8_ASCII_STRING(isolate, "_useDatabase"),
                       JS_UseDatabase);
  TRI_AddMethodVocbase(isolate, ArangoNS,
                       TRI_V8_ASCII_STRING(isolate, "_properties"),
                       JS_DBProperties);
  TRI_AddMethodVocbase(isolate, ArangoNS,
                       TRI_V8_ASCII_STRING(isolate, "_flushCache"),
                       JS_FakeFlushCache, true);

  v8g->VocbaseTempl.Reset(isolate, ArangoNS);
  TRI_AddGlobalFunctionVocbase(
      isolate, TRI_V8_ASCII_STRING(isolate, "ArangoDatabase"),
      ft->GetFunction(context).FromMaybe(v8::Local<v8::Function>()));

  arangodb::iresearch::TRI_InitV8Analyzers(*v8g, isolate);
  TRI_InitV8Statistics(isolate, context);

  TRI_InitV8IndexArangoDB(isolate, ArangoNS);

  TRI_InitV8Collections(context, &vocbase, v8g, isolate, ArangoNS);
  TRI_InitV8Views(*v8g, isolate);
  TRI_InitV8ReplicatedLogs(v8g, isolate);
  TRI_InitV8Users(context, &vocbase, v8g, isolate);
  TRI_InitV8GeneralGraph(context, &vocbase, v8g, isolate);

  TRI_InitV8cursor(context, v8g);

  StorageEngine& engine = server.getFeature<EngineSelectorFeature>().engine();
  engine.addV8Functions();

  // .............................................................................
  // generate global functions
  // .............................................................................

  // AQL functions. not intended to be used directly by end users
  TRI_AddGlobalFunctionVocbase(isolate,
                               TRI_V8_ASCII_STRING(isolate, "AQL_EXECUTE"),
                               JS_ExecuteAql, true);
  TRI_AddGlobalFunctionVocbase(isolate,
                               TRI_V8_ASCII_STRING(isolate, "AQL_EXECUTEJSON"),
                               JS_ExecuteAqlJson, true);
  TRI_AddGlobalFunctionVocbase(isolate,
                               TRI_V8_ASCII_STRING(isolate, "AQL_EXPLAIN"),
                               JS_ExplainAql, true);
  TRI_AddGlobalFunctionVocbase(
      isolate, TRI_V8_ASCII_STRING(isolate, "AQL_PARSE"), JS_ParseAql, true);
  TRI_AddGlobalFunctionVocbase(isolate,
                               TRI_V8_ASCII_STRING(isolate, "AQL_WARNING"),
                               JS_WarningAql, true);
  TRI_AddGlobalFunctionVocbase(
      isolate, TRI_V8_ASCII_STRING(isolate, "AQL_QUERIES_PROPERTIES"),
      JS_QueriesPropertiesAql, true);
  TRI_AddGlobalFunctionVocbase(
      isolate, TRI_V8_ASCII_STRING(isolate, "AQL_QUERIES_CURRENT"),
      JS_QueriesCurrentAql, true);
  TRI_AddGlobalFunctionVocbase(isolate,
                               TRI_V8_ASCII_STRING(isolate, "AQL_QUERIES_SLOW"),
                               JS_QueriesSlowAql, true);
  TRI_AddGlobalFunctionVocbase(
      isolate, TRI_V8_ASCII_STRING(isolate, "AQL_QUERIES_CLEAR_SLOW"),
      JS_QueriesClearSlowAql, true);
  TRI_AddGlobalFunctionVocbase(isolate,
                               TRI_V8_ASCII_STRING(isolate, "AQL_QUERIES_KILL"),
                               JS_QueriesKillAql, true);
  TRI_AddGlobalFunctionVocbase(
      isolate, TRI_V8_ASCII_STRING(isolate, "AQL_QUERY_CACHE_PROPERTIES"),
      JS_QueryCachePropertiesAql, true);
  TRI_AddGlobalFunctionVocbase(
      isolate, TRI_V8_ASCII_STRING(isolate, "AQL_QUERY_CACHE_QUERIES"),
      JS_QueryCacheQueriesAql, true);
  TRI_AddGlobalFunctionVocbase(
      isolate, TRI_V8_ASCII_STRING(isolate, "AQL_QUERY_CACHE_INVALIDATE"),
      JS_QueryCacheInvalidateAql, true);

  TRI_InitV8Replication(isolate, context, &vocbase, threadNumber, v8g);

  TRI_AddGlobalFunctionVocbase(isolate,
                               TRI_V8_ASCII_STRING(isolate, "COMPARE_STRING"),
                               JS_CompareString, true);
  TRI_AddGlobalFunctionVocbase(isolate,
                               TRI_V8_ASCII_STRING(isolate, "NORMALIZE_STRING"),
                               JS_NormalizeString, true);
  TRI_AddGlobalFunctionVocbase(isolate,
                               TRI_V8_ASCII_STRING(isolate, "TIMEZONES"),
                               JS_GetIcuTimezones, true);
  TRI_AddGlobalFunctionVocbase(isolate, TRI_V8_ASCII_STRING(isolate, "LOCALES"),
                               JS_GetIcuLocales, true);
  TRI_AddGlobalFunctionVocbase(isolate,
                               TRI_V8_ASCII_STRING(isolate, "FORMAT_DATETIME"),
                               JS_FormatDatetime, true);
  TRI_AddGlobalFunctionVocbase(isolate,
                               TRI_V8_ASCII_STRING(isolate, "PARSE_DATETIME"),
                               JS_ParseDatetime, true);

  TRI_AddGlobalFunctionVocbase(
      isolate, TRI_V8_ASCII_STRING(isolate, "ENDPOINTS"), JS_Endpoints, true);
  TRI_AddGlobalFunctionVocbase(isolate,
                               TRI_V8_ASCII_STRING(isolate, "TRANSACTION"),
                               JS_Transaction, true);
  TRI_AddGlobalFunctionVocbase(isolate,
                               TRI_V8_ASCII_STRING(isolate, "TRANSACTIONS"),
                               JS_Transactions, true);

  TRI_AddGlobalFunctionVocbase(
      isolate, TRI_V8_ASCII_STRING(isolate, "AUTHENTICATION_ENABLED"),
      JS_AuthenticationEnabled, true);

  TRI_AddGlobalFunctionVocbase(isolate,
                               TRI_V8_ASCII_STRING(isolate, "TRUSTED_PROXIES"),
                               JS_TrustedProxies, true);

  TRI_AddGlobalFunctionVocbase(
      isolate, TRI_V8_ASCII_STRING(isolate, "DECODE_REV"), JS_DecodeRev, true);

  TRI_AddGlobalFunctionVocbase(isolate,
                               TRI_V8_ASCII_STRING(isolate, "ARANGODB_CONTEXT"),
                               JS_ArangoDBContext, true);

  TRI_AddGlobalFunctionVocbase(isolate,
                               TRI_V8_ASCII_STRING(isolate, "AGENCY_DUMP"),
                               JS_AgencyDump, true);

  TRI_AddGlobalFunctionVocbase(
      isolate, TRI_V8_ASCII_STRING(isolate, "SYSTEM_STATISTICS"),
      JS_SystemStatistics, true);

#ifdef USE_ENTERPRISE
  if (server.hasFeature<V8DealerFeature>() &&
      server.getFeature<V8DealerFeature>().allowAdminExecute()) {
    TRI_AddGlobalFunctionVocbase(
        isolate, TRI_V8_ASCII_STRING(isolate, "ENCRYPTION_KEY_RELOAD"),
        JS_EncryptionKeyReload, true);
  }
#endif

  // .............................................................................
  // create global variables
  // .............................................................................

  v8::Handle<v8::Object> v = WrapVocBase(isolate, &vocbase);

  if (v.IsEmpty()) {
    LOG_TOPIC("a97c7", FATAL, arangodb::Logger::FIXME)
        << "out of memory when initializing VocBase";
    FATAL_ERROR_ABORT();
  }

  TRI_AddGlobalVariableVocbase(isolate, TRI_V8_ASCII_STRING(isolate, "db"), v);

  // add collections cache object
  context->Global()
      ->DefineOwnProperty(TRI_IGETC,
                          TRI_V8_ASCII_STRING(isolate, "__dbcache__"),
                          v8::Object::New(isolate), v8::DontEnum)
      .FromMaybe(false);  // ignore result

  // extended names enabled
  context->Global()
      ->DefineOwnProperty(
          TRI_IGETC, TRI_V8_ASCII_STRING(isolate, "FE_EXTENDED_NAMES"),
          v8::Boolean::New(
              isolate, server.getFeature<DatabaseFeature>().extendedNames()),
          v8::PropertyAttribute(v8::ReadOnly | v8::DontEnum))
      .FromMaybe(false);  // ignore result

  // current thread number
  context->Global()
      ->DefineOwnProperty(
          TRI_IGETC, TRI_V8_ASCII_STRING(isolate, "THREAD_NUMBER"),
          v8::Number::New(isolate, (double)threadNumber), v8::ReadOnly)
      .FromMaybe(false);  // ignore result

  // whether or not statistics are enabled
  context->Global()
      ->DefineOwnProperty(
          TRI_IGETC, TRI_V8_ASCII_STRING(isolate, "ENABLE_STATISTICS"),
          v8::Boolean::New(isolate,
                           server.getFeature<StatisticsFeature>().isEnabled()),
          v8::PropertyAttribute(v8::ReadOnly | v8::DontEnum))
      .FromMaybe(false);  // ignore result

  // replication factors
  context->Global()
      ->DefineOwnProperty(
          TRI_IGETC, TRI_V8_ASCII_STRING(isolate, "DEFAULT_REPLICATION_FACTOR"),
          v8::Number::New(
              isolate,
              server.getFeature<ClusterFeature>().defaultReplicationFactor()),
          v8::PropertyAttribute(v8::ReadOnly | v8::DontEnum))
      .FromMaybe(false);  // ignore result

  context->Global()
      ->DefineOwnProperty(
          TRI_IGETC, TRI_V8_ASCII_STRING(isolate, "MIN_REPLICATION_FACTOR"),
          v8::Number::New(
              isolate,
              server.getFeature<ClusterFeature>().minReplicationFactor()),
          v8::PropertyAttribute(v8::ReadOnly | v8::DontEnum))
      .FromMaybe(false);  // ignore result

  context->Global()
      ->DefineOwnProperty(
          TRI_IGETC, TRI_V8_ASCII_STRING(isolate, "MAX_REPLICATION_FACTOR"),
          v8::Number::New(
              isolate,
              server.getFeature<ClusterFeature>().maxReplicationFactor()),
          v8::PropertyAttribute(v8::ReadOnly | v8::DontEnum))
      .FromMaybe(false);  // ignore result

  // max number of shards
  context->Global()
      ->DefineOwnProperty(
          TRI_IGETC, TRI_V8_ASCII_STRING(isolate, "MAX_NUMBER_OF_SHARDS"),
          v8::Number::New(
              isolate, server.getFeature<ClusterFeature>().maxNumberOfShards()),
          v8::PropertyAttribute(v8::ReadOnly | v8::DontEnum))
      .FromMaybe(false);  // ignore result

  // max number of move shards
  context->Global()
      ->DefineOwnProperty(
          TRI_IGETC, TRI_V8_ASCII_STRING(isolate, "MAX_NUMBER_OF_MOVE_SHARDS"),
          v8::Number::New(
              isolate,
              server.getFeature<ClusterFeature>().maxNumberOfMoveShards()),
          v8::PropertyAttribute(v8::ReadOnly | v8::DontEnum))
      .FromMaybe(false);  // ignore result

  // force one shard collections?
  context->Global()
      ->DefineOwnProperty(
          TRI_IGETC, TRI_V8_ASCII_STRING(isolate, "FORCE_ONE_SHARD"),
          v8::Boolean::New(isolate,
                           server.getFeature<ClusterFeature>().forceOneShard()),
          v8::PropertyAttribute(v8::ReadOnly | v8::DontEnum))
      .FromMaybe(false);  // ignore result

  // session timeout (used by web interface)
  context->Global()
      ->DefineOwnProperty(
          TRI_IGETC, TRI_V8_ASCII_STRING(isolate, "SESSION_TIMEOUT"),
          v8::Number::New(
              isolate,
              static_cast<double>(
                  AuthenticationFeature::instance()->sessionTimeout())),
          v8::PropertyAttribute(v8::ReadOnly | v8::DontEnum))
      .FromMaybe(false);  // ignore result

  // a thread-global variable that will contain the AQL module.
  // do not remove this, otherwise AQL queries will break
  context->Global()
      ->DefineOwnProperty(TRI_IGETC, TRI_V8_ASCII_STRING(isolate, "_AQL"),
                          v8::Undefined(isolate), v8::DontEnum)
      .FromMaybe(false);  // ignore result
}
