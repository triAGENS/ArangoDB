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

#include "Aql/Query.h"
#include "Aql/QueryAborter.h"
#include "Aql/QueryResultV8.h"
#include "Aql/QueryString.h"
#include "Basics/GlobalResourceMonitor.h"
#include "Basics/ResourceUsage.h"
#include "Basics/StaticStrings.h"
#include "Basics/VelocyPackHelper.h"
#include "Indexes/Index.h"
#include "StorageEngine/PhysicalCollection.h"
#include "Transaction/Helpers.h"
#include "Transaction/V8Context.h"
#include "Utils/SingleCollectionTransaction.h"
#include "V8/v8-conv.h"
#include "V8/v8-utils.h"
#include "V8/v8-vpack.h"
#include "V8Server/v8-collection.h"
#include "V8Server/v8-externals.h"
#include "V8Server/v8-vocbase.h"
#include "V8Server/v8-vocindex.h"
#include "VocBase/Methods/Collections.h"
#include "VocBase/LogicalCollection.h"
#include "VocBase/vocbase.h"
#include "v8-query.h"

#include <velocypack/Builder.h>
#include <velocypack/Iterator.h>
#include <velocypack/Slice.h>

using namespace arangodb;
using namespace arangodb::basics;

////////////////////////////////////////////////////////////////////////////////
/// @brief run an AQL query and return the result as a V8 array
////////////////////////////////////////////////////////////////////////////////

namespace {
aql::QueryResultV8 AqlQuery(v8::Isolate* isolate,
                            arangodb::LogicalCollection const* col,
                            std::string const& aql,
                            std::shared_ptr<VPackBuilder> const& bindVars,
                            transaction::OperationOrigin operationOrigin) {
  TRI_ASSERT(col != nullptr);

  auto query = arangodb::aql::Query::create(
      transaction::V8Context::create(col->vocbase(), operationOrigin, true),
      arangodb::aql::QueryString(aql), bindVars);
  auto aborter = std::make_shared<aql::QueryAborter>(query);
  arangodb::aql::QueryResultV8 queryResult = query->executeV8(isolate, aborter);
  if (queryResult.result.fail()) {
    if (queryResult.result.is(TRI_ERROR_REQUEST_CANCELED) ||
        queryResult.result.is(TRI_ERROR_QUERY_KILLED)) {
      THROW_ARANGO_EXCEPTION(TRI_ERROR_REQUEST_CANCELED);
    }

    THROW_ARANGO_EXCEPTION(queryResult.result);
  }

  return queryResult;
}

}  // namespace

////////////////////////////////////////////////////////////////////////////////
/// @brief looks up edges for given direction
////////////////////////////////////////////////////////////////////////////////

static void EdgesQuery(TRI_edge_direction_e direction,
                       v8::FunctionCallbackInfo<v8::Value> const& args) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope scope(isolate);
  auto context = TRI_IGETC;

  // first and only argument should be a list of document identifier
  if (args.Length() != 1) {
    switch (direction) {
      case TRI_EDGE_IN:
        TRI_V8_THROW_EXCEPTION_USAGE("inEdges(<vertices>)");

      case TRI_EDGE_OUT:
        TRI_V8_THROW_EXCEPTION_USAGE("outEdges(<vertices>)");

      case TRI_EDGE_ANY:
      default: {
        TRI_V8_THROW_EXCEPTION_USAGE("edges(<vertices>)");
      }
    }
  }

  auto buildFilter = [](TRI_edge_direction_e direction,
                        std::string const& op) -> std::string {
    switch (direction) {
      case TRI_EDGE_IN:
        return "FILTER doc._to " + op + " @value";
      case TRI_EDGE_OUT:
        return "FILTER doc._from " + op + " @value";
      case TRI_EDGE_ANY:
        return "FILTER doc._from " + op + " @value || doc._to " + op +
               " @value";
    }

    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL,
                                   "invalid edge index direction");
  };

  auto* collection = UnwrapCollection(isolate, args.Holder());

  if (!collection) {
    TRI_V8_THROW_EXCEPTION_INTERNAL("cannot extract collection");
  }

  if (TRI_COL_TYPE_EDGE != collection->type()) {
    TRI_V8_THROW_EXCEPTION(TRI_ERROR_ARANGO_COLLECTION_TYPE_INVALID);
  }

  auto addOne = [isolate, &context](VPackBuilder* builder,
                                    v8::Handle<v8::Value> const val) {
    if (val->IsString() || val->IsStringObject()) {
      builder->add(VPackValue(TRI_ObjectToString(isolate, val)));
    } else if (val->IsObject()) {
      v8::Handle<v8::Object> obj =
          val->ToObject(TRI_IGETC).FromMaybe(v8::Local<v8::Object>());
      if (TRI_HasProperty(context, isolate, obj,
                          StaticStrings::IdString.c_str())) {
        builder->add(VPackValue(TRI_ObjectToString(
            isolate, obj->Get(TRI_IGETC, TRI_V8_ASCII_STD_STRING(
                                             isolate, StaticStrings::IdString))
                         .FromMaybe(v8::Local<v8::Value>()))));
      } else {
        builder->add(VPackValue(""));
      }
    } else {
      THROW_ARANGO_EXCEPTION_MESSAGE(
          TRI_ERROR_BAD_PARAMETER,
          "invalid value type. expecting string or object value");
    }
  };

  auto bindVars = std::make_shared<VPackBuilder>();
  bindVars->openObject();
  bindVars->add("@collection", VPackValue(collection->name()));
  bindVars->add(VPackValue("value"));

  if (args[0]->IsArray()) {
    bindVars->openArray();
    v8::Handle<v8::Array> arr = v8::Handle<v8::Array>::Cast(args[0]);
    uint32_t n = arr->Length();
    for (uint32_t i = 0; i < n; ++i) {
      addOne(bindVars.get(),
             arr->Get(context, i).FromMaybe(v8::Local<v8::Value>()));
    }
    bindVars->close();
  } else {
    addOne(bindVars.get(), args[0]);
  }
  bindVars->close();

  std::string filter;
  // argument is a list of vertices
  if (args[0]->IsArray()) {
    filter = buildFilter(direction, "IN");
  } else {
    filter = buildFilter(direction, "==");
  }

  std::string const queryString =
      "FOR doc IN @@collection " + filter + " RETURN doc";
  auto queryResult =
      AqlQuery(isolate, collection, queryString, bindVars,
               transaction::OperationOriginREST{"fetching edges"});

  if (!queryResult.v8Data.IsEmpty()) {
    TRI_V8_RETURN(queryResult.v8Data);
  }

  TRI_V8_RETURN_NULL();
}

////////////////////////////////////////////////////////////////////////////////
/// @brief selects all documents from a collection
////////////////////////////////////////////////////////////////////////////////

static void JS_AllQuery(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);
  auto context = TRI_IGETC;

  auto* collection = UnwrapCollection(isolate, args.Holder());

  if (!collection) {
    TRI_V8_THROW_EXCEPTION_INTERNAL("cannot extract collection");
  }

  std::string const collectionName(collection->name());

  ResourceMonitor monitor(GlobalResourceMonitor::instance());

  auto operationOrigin =
      transaction::OperationOriginREST{"enumerating collection documents"};
  auto transactionContext = transaction::V8Context::create(
      collection->vocbase(), operationOrigin, true);
  SingleCollectionTransaction trx(transactionContext, *collection,
                                  AccessMode::Type::READ);

  Result res = trx.begin();

  if (!res.ok()) {
    TRI_V8_THROW_EXCEPTION(res);
  }

  // copy default options
  VPackOptions resultOptions = VPackOptions::Defaults;
  resultOptions.customTypeHandler =
      transactionContext->orderCustomTypeHandler();

  VPackBuilder resultBuilder;
  resultBuilder.openArray();

  // will track memory usage for the documents built here
  ResourceUsageScope memoryScope(monitor);

  // We directly read the entire cursor. so batchsize == limit
  auto iterator =
      trx.indexScan(collectionName, transaction::Methods::CursorType::ALL,
                    ReadOwnWrites::no)
          .waitAndGet();
  auto cb = [&](LocalDocumentId, aql::DocumentData&&, VPackSlice slice) {
    auto const& buffer = resultBuilder.bufferRef();
    size_t memoryUsageOld = buffer.size();
    resultBuilder.add(slice);
    TRI_ASSERT(buffer.size() > memoryUsageOld);
    memoryScope.increase(buffer.size() - memoryUsageOld);
    return true;
  };
  iterator->allDocuments(cb);

  resultBuilder.close();

  res = trx.finish(Result());

  if (res.fail()) {
    TRI_V8_THROW_EXCEPTION(res);
  }

  VPackSlice docs = resultBuilder.slice();
  TRI_ASSERT(docs.isArray());
  // setup result
  v8::Handle<v8::Object> result = v8::Object::New(isolate);
  auto documents = TRI_VPackToV8(isolate, docs, &resultOptions);
  result->Set(context, TRI_V8_ASCII_STRING(isolate, "documents"), documents)
      .FromMaybe(false);
  result
      ->Set(context, TRI_V8_ASCII_STRING(isolate, "total"),
            v8::Number::New(isolate, static_cast<double>(docs.length())))
      .FromMaybe(false);
  result
      ->Set(context, TRI_V8_ASCII_STRING(isolate, "count"),
            v8::Number::New(isolate, static_cast<double>(docs.length())))
      .FromMaybe(false);

  TRI_V8_RETURN(result);
  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief selects a random document
///
/// @FUN{@FA{collection}.any()}
///
/// The @FN{any} method returns a random document from the collection.  It
/// returns
/// @LIT{null} if the collection is empty.
////////////////////////////////////////////////////////////////////////////////

static void JS_AnyQuery(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);

  auto* col = UnwrapCollection(isolate, args.Holder());

  if (!col) {
    TRI_V8_THROW_EXCEPTION_INTERNAL("cannot extract collection");
  }

  std::string const collectionName(col->name());

  auto operationOrigin =
      transaction::OperationOriginREST{"fetching random document"};
  auto transactionContext =
      transaction::V8Context::create(col->vocbase(), operationOrigin, true);
  SingleCollectionTransaction trx(transactionContext, *col,
                                  AccessMode::Type::READ);

  Result res = trx.begin();

  if (!res.ok()) {
    TRI_V8_THROW_EXCEPTION(res);
  }

  OperationOptions options(ExecContext::current());
  OperationResult cursor = trx.any(collectionName, options).waitAndGet();

  res = trx.finish(cursor.result);

  if (cursor.fail()) {
    TRI_V8_THROW_EXCEPTION(cursor.result);
  }

  if (!res.ok()) {
    TRI_V8_THROW_EXCEPTION(res);
  }

  VPackSlice doc = cursor.slice();
  TRI_ASSERT(doc.isArray());

  if (doc.length() == 0) {
    // The collection is empty.
    TRI_V8_RETURN_NULL();
  }

  // copy default options
  VPackOptions resultOptions = VPackOptions::Defaults;
  resultOptions.customTypeHandler =
      transactionContext->orderCustomTypeHandler();
  TRI_V8_RETURN(TRI_VPackToV8(isolate, doc.at(0), &resultOptions));
  TRI_V8_TRY_CATCH_END
}

static void JS_ChecksumCollection(
    v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);
  auto context = TRI_IGETC;

  auto* col = UnwrapCollection(isolate, args.Holder());

  if (!col) {
    TRI_V8_THROW_EXCEPTION_INTERNAL("cannot extract collection");
  }

  bool withRevisions = false;
  bool withData = false;

  if (args.Length() > 0) {
    withRevisions = TRI_ObjectToBoolean(isolate, args[0]);
    if (args.Length() > 1) {
      withData = TRI_ObjectToBoolean(isolate, args[1]);
    }
  }

  uint64_t checksum;
  RevisionId revId;

  Result r = methods::Collections::checksum(*col, withRevisions, withData,
                                            checksum, revId)
                 .waitAndGet();

  if (!r.ok()) {
    TRI_V8_THROW_EXCEPTION(r);
  }

  v8::Local<v8::Object> obj = v8::Object::New(isolate);
  obj->Set(context, TRI_V8_ASCII_STRING(isolate, "checksum"),
           TRI_V8_ASCII_STD_STRING(isolate, std::to_string(checksum)))
      .FromMaybe(false);
  obj->Set(context, TRI_V8_ASCII_STRING(isolate, "revision"),
           TRI_V8_ASCII_STD_STRING(isolate, revId.toString()))
      .FromMaybe(false);

  TRI_V8_RETURN(obj);
  TRI_V8_TRY_CATCH_END
}

static void JS_EdgesQuery(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  EdgesQuery(TRI_EDGE_ANY, args);
  // cppcheck-suppress *
  TRI_V8_TRY_CATCH_END
}

static void JS_InEdgesQuery(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  EdgesQuery(TRI_EDGE_IN, args);
  // cppcheck-suppress *
  TRI_V8_TRY_CATCH_END
}

static void JS_OutEdgesQuery(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  EdgesQuery(TRI_EDGE_OUT, args);
  // cppcheck-suppress *
  TRI_V8_TRY_CATCH_END
}

static void JS_LookupByKeys(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);
  auto context = TRI_IGETC;

  auto* collection = UnwrapCollection(isolate, args.Holder());

  if (!collection) {
    TRI_V8_THROW_EXCEPTION_INTERNAL("cannot extract collection");
  }

  if (args.Length() != 1 || !args[0]->IsArray()) {
    TRI_V8_THROW_EXCEPTION_USAGE("documents(<keys>)");
  }

  auto bindVars = std::make_shared<VPackBuilder>();
  bindVars->openObject();
  bindVars->add("@collection", VPackValue(collection->name()));

  VPackBuilder keys;
  TRI_V8ToVPack(isolate, keys, args[0], false);

  bindVars->add(VPackValue("keys"));
  arangodb::aql::BindParameters::stripCollectionNames(
      keys.slice(), collection->name(), *bindVars.get());
  bindVars->close();

  std::string const queryString(
      "FOR doc IN @@collection FILTER doc._key IN @keys RETURN doc");
  auto queryResult =
      AqlQuery(isolate, collection, queryString, bindVars,
               transaction::OperationOriginREST{"looking up documents"});

  v8::Handle<v8::Object> result = v8::Object::New(isolate);
  if (!queryResult.v8Data.IsEmpty()) {
    result
        ->Set(context, TRI_V8_ASCII_STRING(isolate, "documents"),
              queryResult.v8Data)
        .FromMaybe(false);
  }

  TRI_V8_RETURN(result);
  TRI_V8_TRY_CATCH_END
}

static void JS_RemoveByKeys(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);
  auto context = TRI_IGETC;

  auto* collection = UnwrapCollection(isolate, args.Holder());

  if (!collection) {
    TRI_V8_THROW_EXCEPTION_INTERNAL("cannot extract collection");
  }

  if (args.Length() != 1 || !args[0]->IsArray()) {
    TRI_V8_THROW_EXCEPTION_USAGE("removeByKeys(<keys>)");
  }

  auto bindVars = std::make_shared<VPackBuilder>();
  bindVars->openObject();
  bindVars->add("@collection", VPackValue(collection->name()));
  bindVars->add(VPackValue("keys"));

  TRI_V8ToVPack(isolate, *(bindVars.get()), args[0], false);
  bindVars->close();

  std::string const queryString(
      "FOR key IN @keys REMOVE key IN @@collection OPTIONS { ignoreErrors: "
      "true }");
  auto queryResult =
      AqlQuery(isolate, collection, queryString, bindVars,
               transaction::OperationOriginREST{"removing documents"});

  size_t ignored = 0;
  size_t removed = 0;

  if (queryResult.extra) {
    VPackSlice stats = queryResult.extra->slice().get("stats");
    if (!stats.isNone()) {
      TRI_ASSERT(stats.isObject());
      VPackSlice found = stats.get("writesIgnored");
      if (found.isNumber()) {
        ignored = found.getNumericValue<size_t>();
      }

      found = stats.get("writesExecuted");
      if (found.isNumber()) {
        removed = found.getNumericValue<size_t>();
      }
    }
  }

  v8::Handle<v8::Object> result = v8::Object::New(isolate);
  result
      ->Set(context, TRI_V8_ASCII_STRING(isolate, "removed"),
            v8::Number::New(isolate, static_cast<double>(removed)))
      .FromMaybe(false);
  result
      ->Set(context, TRI_V8_ASCII_STRING(isolate, "ignored"),
            v8::Number::New(isolate, static_cast<double>(ignored)))
      .FromMaybe(false);

  TRI_V8_RETURN(result);
  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief creates the query functions
////////////////////////////////////////////////////////////////////////////////

void TRI_InitV8Queries(v8::Isolate* isolate, v8::Handle<v8::Context> context) {
  v8::HandleScope scope(isolate);

  TRI_GET_GLOBALS();
  TRI_ASSERT(v8g != nullptr);

  TRI_GET_GLOBAL(VocbaseColTempl, v8::ObjectTemplate);
  TRI_ASSERT(!VocbaseColTempl.IsEmpty());

  // .............................................................................
  // generate the arangodb::LogicalCollection template
  // .............................................................................

  TRI_AddMethodVocbase(isolate, VocbaseColTempl,
                       TRI_V8_ASCII_STRING(isolate, "ALL"), JS_AllQuery, true);
  TRI_AddMethodVocbase(isolate, VocbaseColTempl,
                       TRI_V8_ASCII_STRING(isolate, "ANY"), JS_AnyQuery, true);
  TRI_AddMethodVocbase(isolate, VocbaseColTempl,
                       TRI_V8_ASCII_STRING(isolate, "checksum"),
                       JS_ChecksumCollection);
  TRI_AddMethodVocbase(isolate, VocbaseColTempl,
                       TRI_V8_ASCII_STRING(isolate, "EDGES"), JS_EdgesQuery,
                       true);
  TRI_AddMethodVocbase(isolate, VocbaseColTempl,
                       TRI_V8_ASCII_STRING(isolate, "INEDGES"), JS_InEdgesQuery,
                       true);
  TRI_AddMethodVocbase(isolate, VocbaseColTempl,
                       TRI_V8_ASCII_STRING(isolate, "OUTEDGES"),
                       JS_OutEdgesQuery, true);
  TRI_AddMethodVocbase(isolate, VocbaseColTempl,
                       TRI_V8_ASCII_STRING(isolate, "lookupByKeys"),
                       JS_LookupByKeys,
                       true);  // an alias for .documents
  TRI_AddMethodVocbase(isolate, VocbaseColTempl,
                       TRI_V8_ASCII_STRING(isolate, "documents"),
                       JS_LookupByKeys, true);
  TRI_AddMethodVocbase(isolate, VocbaseColTempl,
                       TRI_V8_ASCII_STRING(isolate, "removeByKeys"),
                       JS_RemoveByKeys, true);
}
