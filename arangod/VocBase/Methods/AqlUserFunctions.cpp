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
/// @author Wilfried Goesgens
////////////////////////////////////////////////////////////////////////////////

#ifndef USE_V8
#error this file is not supposed to be used in builds with -DUSE_V8=Off
#endif

#include "AqlUserFunctions.h"

#include "ApplicationFeatures/ApplicationServer.h"
#include "Aql/Query.h"
#include "Aql/QueryAborter.h"
#include "Aql/QueryRegistry.h"
#include "Aql/QueryString.h"
#include "Basics/StaticStrings.h"
#include "Basics/StringUtils.h"
#include "Basics/VelocyPackHelper.h"
#include "RestServer/QueryRegistryFeature.h"
#include "Transaction/Helpers.h"
#include "Transaction/Methods.h"
#include "Transaction/StandaloneContext.h"
#include "Transaction/V8Context.h"
#include "Utils/OperationOptions.h"
#include "Utils/SingleCollectionTransaction.h"
#include "V8/JavaScriptSecurityContext.h"
#include "V8/v8-globals.h"
#include "V8/v8-utils.h"
#include "V8Server/V8DealerFeature.h"

#include <v8.h>
#include <absl/strings/str_cat.h>
#include <velocypack/Builder.h>
#include <velocypack/Iterator.h>

#include <regex>
#include <string>
#include <string_view>

using namespace arangodb;

namespace {
constexpr std::string_view moduleName = "AQL user functions administration";
// origin=REST here because we are not executing AQL but are managing
// AQL user-defined functions here.
constexpr transaction::OperationOriginREST operationOrigin{moduleName};

// Must not start with `_`, may contain alphanumerical characters, should have
// at least one set of double colons followed by more alphanumerical characters.
std::regex const funcRegEx("[a-zA-Z0-9][a-zA-Z0-9_]*(::[a-zA-Z0-9_]+)+",
                           std::regex::ECMAScript);

// we may filter less restrictive:
std::regex const funcFilterRegEx("[a-zA-Z0-9_]+(::[a-zA-Z0-9_]*)*",
                                 std::regex::ECMAScript);

bool isValidFunctionName(std::string const& testName) {
  return std::regex_match(testName, funcRegEx);
}

bool isValidFunctionNameFilter(std::string const& testName) {
  return std::regex_match(testName, funcFilterRegEx);
}

void reloadAqlUserFunctions(ArangodServer& server) {
  if (server.hasFeature<V8DealerFeature>() &&
      server.isEnabled<V8DealerFeature>() &&
      server.getFeature<V8DealerFeature>().isEnabled()) {
    server.getFeature<V8DealerFeature>().addGlobalExecutorMethod(
        GlobalExecutorMethods::MethodType::kReloadAql);
  }
}

}  // namespace

Result arangodb::unregisterUserFunction(TRI_vocbase_t& vocbase,
                                        std::string const& functionName) {
  if (functionName.empty() || !isValidFunctionNameFilter(functionName)) {
    return Result(TRI_ERROR_QUERY_FUNCTION_INVALID_NAME,
                  absl::StrCat("error deleting AQL user function: '",
                               functionName, "' contains invalid characters"));
  }

  std::string UCFN = basics::StringUtils::toupper(functionName);
  Result res;
  {
    VPackBuilder builder;
    {
      VPackObjectBuilder guard(&builder);
      builder.add(StaticStrings::KeyString, VPackValue(UCFN));
    }

    auto ctx = transaction::V8Context::createWhenRequired(
        vocbase, ::operationOrigin, true);
    SingleCollectionTransaction trx(std::move(ctx),
                                    StaticStrings::AqlFunctionsCollection,
                                    AccessMode::Type::WRITE);

    trx.addHint(transaction::Hints::Hint::SINGLE_OPERATION);

    res = trx.begin();

    if (res.ok()) {
      OperationResult result = trx.remove(StaticStrings::AqlFunctionsCollection,
                                          builder.slice(), OperationOptions());
      res = trx.finish(result.result);
    }
  }

  if (res.ok()) {
    reloadAqlUserFunctions(vocbase.server());
  } else if (res.is(TRI_ERROR_ARANGO_DOCUMENT_NOT_FOUND)) {
    return res.reset(TRI_ERROR_QUERY_FUNCTION_NOT_FOUND,
                     absl::StrCat("no AQL user function with name '",
                                  functionName, "' found"));
  }
  return res;
}

Result arangodb::unregisterUserFunctionsGroup(
    TRI_vocbase_t& vocbase, std::string const& functionFilterPrefix,
    int& deleteCount) {
  deleteCount = 0;

  if (functionFilterPrefix.empty()) {
    return Result(TRI_ERROR_BAD_PARAMETER);
  }

  if (!isValidFunctionNameFilter(functionFilterPrefix)) {
    return Result(
        TRI_ERROR_QUERY_FUNCTION_INVALID_NAME,
        absl::StrCat("error deleting AQL user function: '",
                     functionFilterPrefix, "' contains invalid characters"));
  }

  std::string uc(functionFilterPrefix);
  basics::StringUtils::toupperInPlace(uc);

  if ((uc.length() < 2) || (uc[uc.length() - 1] != ':') ||
      (uc[uc.length() - 2] != ':')) {
    uc += "::";
  }

  auto binds = std::make_shared<VPackBuilder>();
  binds->openObject();
  binds->add("fnLength", VPackValue(uc.length()));
  binds->add("ucName", VPackValue(uc));
  binds->add("@col", VPackValue(StaticStrings::AqlFunctionsCollection));
  binds->close();

  std::string aql(
      "FOR fn IN @@col FILTER UPPER(LEFT(fn.name, @fnLength)) == @ucName "
      "REMOVE { _key: fn._key} in @@col RETURN 1");

  {
    auto query = arangodb::aql::Query::create(
        transaction::V8Context::createWhenRequired(vocbase, ::operationOrigin,
                                                   true),
        arangodb::aql::QueryString(aql), std::move(binds));
    auto aborter = std::make_shared<aql::QueryAborter>(query);
    aql::QueryResult queryResult = query->executeSync(aborter);

    if (queryResult.result.fail()) {
      if (queryResult.result.is(TRI_ERROR_REQUEST_CANCELED) ||
          (queryResult.result.is(TRI_ERROR_QUERY_KILLED))) {
        return Result(TRI_ERROR_REQUEST_CANCELED);
      }
      return queryResult.result;
    }

    VPackSlice countSlice = queryResult.data->slice();
    if (!countSlice.isArray()) {
      return Result(TRI_ERROR_INTERNAL,
                    "bad query result for deleting AQL user functions");
    }

    deleteCount = static_cast<int>(countSlice.length());
  }

  reloadAqlUserFunctions(vocbase.server());
  return Result();
}

Result arangodb::registerUserFunction(TRI_vocbase_t& vocbase,
                                      velocypack::Slice userFunction,
                                      bool& replacedExisting) {
  replacedExisting = false;

  Result res;

  auto& server = vocbase.server();
  if (!server.hasFeature<V8DealerFeature>() ||
      !server.isEnabled<V8DealerFeature>() ||
      !server.getFeature<V8DealerFeature>().isEnabled()) {
    return res.reset(TRI_ERROR_DISABLED,
                     "JavaScript operations are not available");
  }

  std::string name;

  try {
    auto vname = userFunction.get("name");
    if (!vname.isString()) {
      return {TRI_ERROR_QUERY_FUNCTION_INVALID_NAME,
              "function name has to be provided as a string"};
    }
    name = vname.copyString();
    if (name.empty()) {
      return {TRI_ERROR_QUERY_FUNCTION_INVALID_NAME,
              "function name has to be provided and must not be empty"};
    }
  } catch (...) {
    return {TRI_ERROR_QUERY_FUNCTION_INVALID_NAME,
            "missing mandatory attribute 'name'"};
  }

  if (!isValidFunctionName(name)) {
    return {TRI_ERROR_QUERY_FUNCTION_INVALID_NAME,
            absl::StrCat("error creating AQL user function: '", name,
                         "' is not a valid name")};
  }

  auto cvString = userFunction.get("code");
  if (!cvString.isString() || cvString.getStringLength() == 0) {
    return {TRI_ERROR_QUERY_FUNCTION_INVALID_CODE,
            "expecting string with function definition"};
  }

  std::string code = absl::StrCat("(", cvString.stringView(), "\n)");

  bool isDeterministic = false;
  VPackSlice isDeterministicSlice = userFunction.get("isDeterministic");
  if (isDeterministicSlice.isBoolean()) {
    isDeterministic = isDeterministicSlice.getBoolean();
  }

  {
    bool throwV8Exception = (v8::Isolate::TryGetCurrent() != nullptr);

    JavaScriptSecurityContext securityContext =
        JavaScriptSecurityContext::createRestrictedContext();
    V8ConditionalExecutorGuard contextGuard(&vocbase, securityContext);

    if (contextGuard.isolate() == nullptr) {
      return {TRI_ERROR_INTERNAL, "could not acquire v8 executor in time"};
    }

    std::string testCode =
        absl::StrCat("(function() { var callback = ", cvString.stringView(),
                     "; return callback; })()");

    res = contextGuard.runInContext([&](v8::Isolate* isolate) -> Result {
      v8::HandleScope scope(isolate);

      v8::TryCatch tryCatch(isolate);

      v8::Handle<v8::Value> result =
          TRI_ExecuteJavaScriptString(isolate, testCode, "userFunction", false);

      Result res;

      if (result.IsEmpty() || !result->IsFunction() || tryCatch.HasCaught()) {
        if (tryCatch.HasCaught()) {
          res.reset(TRI_ERROR_QUERY_FUNCTION_INVALID_CODE,
                    absl::StrCat(
                        TRI_errno_string(TRI_ERROR_QUERY_FUNCTION_INVALID_CODE),
                        ": ", TRI_StringifyV8Exception(isolate, &tryCatch)));

          if (!tryCatch.CanContinue()) {
            if (throwV8Exception) {
              tryCatch.ReThrow();
            }
            TRI_GET_GLOBALS();
            v8g->_canceled = true;
          }
        } else {
          res.reset(TRI_ERROR_QUERY_FUNCTION_INVALID_CODE,
                    std::string(TRI_errno_string(
                        TRI_ERROR_QUERY_FUNCTION_INVALID_CODE)));
        }
      }
      return res;
    });
  }

  if (!res.ok()) {
    return res;
  }

  std::string _key(name);
  basics::StringUtils::toupperInPlace(_key);

  VPackBuilder oneFunctionDocument;
  oneFunctionDocument.openObject();
  oneFunctionDocument.add(StaticStrings::KeyString, VPackValue(_key));
  oneFunctionDocument.add("name", VPackValue(name));
  oneFunctionDocument.add("code", VPackValue(code));
  oneFunctionDocument.add("isDeterministic", VPackValue(isDeterministic));
  oneFunctionDocument.close();

  {
    arangodb::OperationOptions opOptions;
    opOptions.waitForSync = true;
    opOptions.returnOld = true;
    opOptions.overwriteMode = OperationOptions::OverwriteMode::Replace;

    // find and load collection given by name or identifier
    auto ctx = transaction::V8Context::createWhenRequired(
        vocbase, ::operationOrigin, true);
    SingleCollectionTransaction trx(std::move(ctx),
                                    StaticStrings::AqlFunctionsCollection,
                                    AccessMode::Type::WRITE);

    res = trx.begin();
    if (res.fail()) {
      return res;
    }

    arangodb::OperationResult result =
        trx.insert(StaticStrings::AqlFunctionsCollection,
                   oneFunctionDocument.slice(), opOptions);

    if (result.ok()) {
      VPackSlice oldSlice = result.slice().get(StaticStrings::Old);
      replacedExisting = !(oldSlice.isNone() || oldSlice.isNull());
    }
    // Will commit if no error occured.
    // or abort if an error occured.
    // result stays valid!
    res = trx.finish(result.result);
  }

  if (res.ok()) {
    reloadAqlUserFunctions(vocbase.server());
  }

  return res;
}

Result arangodb::toArrayUserFunctions(TRI_vocbase_t& vocbase,
                                      std::string const& functionFilterPrefix,
                                      velocypack::Builder& result) {
  std::string aql;
  auto binds = std::make_shared<VPackBuilder>();

  binds->openObject();

  if (!functionFilterPrefix.empty()) {
    aql =
        "FOR function IN @@col FILTER LEFT(function._key, @fnLength) == "
        "@ucName RETURN function";

    std::string uc(functionFilterPrefix);
    basics::StringUtils::toupperInPlace(uc);

    if ((uc.length() < 2) || (uc[uc.length() - 1] != ':') ||
        (uc[uc.length() - 2] != ':')) {
      uc += "::";
    }

    binds->add("fnLength", VPackValue(uc.length()));
    binds->add("ucName", VPackValue(uc));
  } else {
    aql = "FOR function IN @@col RETURN function";
  }

  binds->add("@col", VPackValue(StaticStrings::AqlFunctionsCollection));
  binds->close();

  auto query = arangodb::aql::Query::create(
      transaction::V8Context::createWhenRequired(vocbase, ::operationOrigin,
                                                 true),
      arangodb::aql::QueryString(aql), std::move(binds));
  auto aborter = std::make_shared<aql::QueryAborter>(query);
  aql::QueryResult queryResult = query->executeSync(aborter);

  if (queryResult.result.fail()) {
    if (queryResult.result.is(TRI_ERROR_REQUEST_CANCELED) ||
        (queryResult.result.is(TRI_ERROR_QUERY_KILLED))) {
      return Result(TRI_ERROR_REQUEST_CANCELED);
    }
    return queryResult.result;
  }

  auto usersFunctionsSlice = queryResult.data->slice();

  if (!usersFunctionsSlice.isArray()) {
    return Result(TRI_ERROR_INTERNAL,
                  "bad query result for AQL user functions");
  }

  result.openArray();
  std::string tmp;
  for (VPackSlice it : VPackArrayIterator(usersFunctionsSlice)) {
    VPackSlice resolved;
    resolved = it.resolveExternal();

    if (!resolved.isObject()) {
      return Result(TRI_ERROR_INTERNAL,
                    "element that stores AQL user function is not an object");
    }

    VPackSlice name, fn, dtm;
    bool isDeterministic = false;
    name = resolved.get("name");
    fn = resolved.get("code");
    dtm = resolved.get("isDeterministic");
    if (dtm.isBoolean()) {
      isDeterministic = dtm.getBool();
    }
    // We simply ignore invalid entries in the _functions collection:
    if (name.isString() && fn.isString() && (fn.getStringLength() > 2)) {
      std::string_view ref = fn.stringView();

      ref = ref.substr(1, ref.length() - 2);
      tmp = basics::StringUtils::trim(std::string(ref));

      VPackBuilder oneFunction;
      oneFunction.openObject();
      oneFunction.add("name", name);
      oneFunction.add("code", VPackValue(tmp));
      oneFunction.add("isDeterministic", VPackValue(isDeterministic));
      oneFunction.close();
      result.add(oneFunction.slice());
    }
  }
  result.close();  // close Array

  return Result();
}
