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
/// @author Dr. Frank Celler
////////////////////////////////////////////////////////////////////////////////

#include "v8-vocindex.h"
#include "ApplicationFeatures/ApplicationServer.h"
#include "Basics/ReadLocker.h"
#include "Basics/StringUtils.h"
#include "Basics/VelocyPackHelper.h"
#include "Basics/conversions.h"
#include "Basics/tri-strings.h"
#include "Cluster/ClusterFeature.h"
#include "Cluster/ClusterInfo.h"
#include "GeneralServer/AuthenticationFeature.h"
#include "Indexes/Index.h"
#include "Indexes/IndexFactory.h"
#include "StorageEngine/EngineSelectorFeature.h"
#include "StorageEngine/StorageEngine.h"
#include "Transaction/Helpers.h"
#include "Transaction/Hints.h"
#include "Transaction/V8Context.h"
#include "Utils/Events.h"
#include "Utils/ExecContext.h"
#include "V8/v8-conv.h"
#include "V8/v8-globals.h"
#include "V8/v8-utils.h"
#include "V8/v8-vpack.h"
#include "V8Server/v8-collection.h"
#include "V8Server/v8-externals.h"
#include "V8Server/v8-vocbase.h"
#include "V8Server/v8-vocbaseprivate.h"
#include "VocBase/LogicalCollection.h"
#include "VocBase/Methods/Collections.h"
#include "VocBase/Methods/Indexes.h"
#include "VocBase/Properties/PlanCollection.h"

#include <velocypack/Builder.h>
#include <velocypack/Iterator.h>

using namespace arangodb;
using namespace arangodb::basics;
using namespace arangodb::rest;

////////////////////////////////////////////////////////////////////////////////
/// @brief ensures an index
////////////////////////////////////////////////////////////////////////////////

static void EnsureIndex(v8::FunctionCallbackInfo<v8::Value> const& args,
                        bool create, char const* functionName) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope scope(isolate);

  auto* collection = UnwrapCollection(isolate, args.Holder());

  if (!collection) {
    TRI_V8_THROW_EXCEPTION_INTERNAL("cannot extract collection");
  }

  if (args.Length() != 1 || !args[0]->IsObject()) {
    std::string name(functionName);
    name.append("(<description>)");
    TRI_V8_THROW_EXCEPTION_USAGE(name.c_str());
  }

  VPackBuilder builder;
  TRI_V8ToVPack(isolate, builder, args[0], false, false);

  VPackBuilder output;
  auto res = methods::Indexes::ensureIndex(collection, builder.slice(), create,
                                           output);

  if (res.fail()) {
    TRI_V8_THROW_EXCEPTION(res);
  }

  v8::Handle<v8::Value> result = TRI_VPackToV8(isolate, output.slice());
  TRI_V8_RETURN(result);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief was docuBlock collectionEnsureIndex
////////////////////////////////////////////////////////////////////////////////

static void JS_EnsureIndexVocbaseCol(
    v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);

  PREVENT_EMBEDDED_TRANSACTION();

  EnsureIndex(args, true, "ensureIndex");
  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief looks up an index
////////////////////////////////////////////////////////////////////////////////

static void JS_LookupIndexVocbaseCol(
    v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);

  EnsureIndex(args, false, "lookupIndex");
  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief was docuBlock col_dropIndex
////////////////////////////////////////////////////////////////////////////////

static void JS_DropIndexVocbaseCol(
    v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);
  auto& vocbase = GetContextVocBase(isolate);

  PREVENT_EMBEDDED_TRANSACTION();

  auto* collection = UnwrapCollection(isolate, args.Holder());

  if (!collection) {
    events::DropIndex(vocbase.name(), "", "", TRI_ERROR_INTERNAL);
    TRI_V8_THROW_EXCEPTION_INTERNAL("cannot extract collection");
  }

  if (args.Length() != 1) {
    events::DropIndex(vocbase.name(), "", "", TRI_ERROR_BAD_PARAMETER);
    TRI_V8_THROW_EXCEPTION_USAGE("dropIndex(<index-id>)");
  }

  VPackBuilder builder;
  TRI_V8ToVPack(isolate, builder, args[0], false, false);

  auto res = methods::Indexes::drop(collection, builder.slice());

  if (res.ok()) {
    TRI_V8_RETURN_TRUE();
  }

  TRI_V8_RETURN_FALSE();
  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief was docuBlock collectionGetIndexes
////////////////////////////////////////////////////////////////////////////////

static void JS_GetIndexesVocbaseCol(
    v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);

  auto* collection = UnwrapCollection(isolate, args.Holder());

  if (!collection) {
    TRI_V8_THROW_EXCEPTION_INTERNAL("cannot extract collection");
  }

  auto flags = Index::makeFlags(Index::Serialize::Estimates);

  if (args.Length() > 0 && TRI_ObjectToBoolean(isolate, args[0])) {
    flags = Index::makeFlags(Index::Serialize::Estimates,
                             Index::Serialize::Figures);
  }

  bool withHidden = false;
  if (args.Length() > 1) {
    withHidden = TRI_ObjectToBoolean(isolate, args[1]);
  }

  VPackBuilder output;
  auto res = methods::Indexes::getAll(collection, flags, withHidden, output);

  if (res.fail()) {
    TRI_V8_THROW_EXCEPTION(res);
  }

  v8::Handle<v8::Value> result = TRI_VPackToV8(isolate, output.slice());
  TRI_V8_RETURN(result);
  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief create a collection
////////////////////////////////////////////////////////////////////////////////

static void CreateVocBase(v8::FunctionCallbackInfo<v8::Value> const& args,
                          TRI_col_type_e collectionType) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope scope(isolate);

  auto& vocbase = GetContextVocBase(isolate);

  if (vocbase.isDangling()) {
    events::CreateCollection(vocbase.name(), StaticStrings::Empty,
                             TRI_ERROR_ARANGO_DATABASE_NOT_FOUND);
    TRI_V8_THROW_EXCEPTION(TRI_ERROR_ARANGO_DATABASE_NOT_FOUND);
  } else if (args.Length() < 1 || args.Length() > 4) {
    events::CreateCollection(vocbase.name(), StaticStrings::Empty,
                             TRI_ERROR_BAD_PARAMETER);
    TRI_V8_THROW_EXCEPTION_USAGE(
        "_create(<name>, <properties>, <type>, <options>)");
  }

  if (!ExecContext::current().canUseDatabase(vocbase.name(), auth::Level::RW)) {
    events::CreateCollection(vocbase.name(), StaticStrings::Empty,
                             TRI_ERROR_FORBIDDEN);
    TRI_V8_THROW_EXCEPTION(TRI_ERROR_FORBIDDEN);
  }

  // optional, third parameter can override collection type
  if (args.Length() >= 3 && args[2]->IsString()) {
    std::string typeString = TRI_ObjectToString(isolate, args[2]);
    if (typeString == "edge") {
      collectionType = TRI_COL_TYPE_EDGE;
    } else if (typeString == "document") {
      collectionType = TRI_COL_TYPE_DOCUMENT;
    }
  }

  PREVENT_EMBEDDED_TRANSACTION();

  // extract the name
  std::string const name = TRI_ObjectToString(isolate, args[0]);

  VPackBuilder properties;
  VPackSlice propSlice = VPackSlice::emptyObjectSlice();
  if (args.Length() >= 2) {
    if (!args[1]->IsObject()) {
      TRI_V8_THROW_TYPE_ERROR("<properties> must be an object");
    }
    TRI_V8ToVPack(
        isolate, properties,
        args[1]->ToObject(TRI_IGETC).FromMaybe(v8::Local<v8::Object>()), false);
    propSlice = properties.slice();
  }

  // waitForSync can be 3. or 4. parameter
  TRI_GET_SERVER_GLOBALS(ArangodServer);
  auto& cluster = v8g->server().getFeature<ClusterFeature>();
  bool createWaitsForSyncReplication = cluster.createWaitsForSyncReplication();
  bool enforceReplicationFactor = true;

  if (args.Length() >= 3 && args[args.Length() - 1]->IsObject()) {
    v8::Handle<v8::Object> obj =
        args[args.Length() - 1]->ToObject(TRI_IGETC).FromMaybe(
            v8::Local<v8::Object>());
    createWaitsForSyncReplication = TRI_GetOptionalBooleanProperty(
        isolate, obj, "waitForSyncReplication", createWaitsForSyncReplication);

    enforceReplicationFactor = TRI_GetOptionalBooleanProperty(
        isolate, obj, "enforceReplicationFactor", enforceReplicationFactor);
  }

  auto planCollection = PlanCollection::fromCreateAPIV8(
      propSlice, name, collectionType,
      PlanCollection::DatabaseConfiguration{vocbase});

  if (planCollection.fail()) {
    events::CreateCollection(vocbase.name(), name,
                             planCollection.errorNumber());
    TRI_V8_THROW_EXCEPTION(planCollection.result());
  }

  std::vector<PlanCollection> collections{std::move(planCollection.get())};

  OperationOptions options(ExecContext::current());
  std::shared_ptr<LogicalCollection> coll;
  auto result = methods::Collections::create(
      vocbase,  // collection vocbase
      options, collections,
      createWaitsForSyncReplication,  // replication wait flag
      enforceReplicationFactor,       // replication factor flag
      /*isNewDatabase*/ false         // here always false
  );

  if (result.fail()) {
    TRI_V8_THROW_EXCEPTION(result.result());
  } else {
    TRI_ASSERT(result.get().size() == 1);
    coll = result.get().at(0);
  }

  v8::Handle<v8::Value> v8Result;
  if (coll) {
    v8Result = WrapCollection(isolate, coll);
  }

  TRI_V8_RETURN(v8Result);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief was docuBlock collectionDatabaseCreate
////////////////////////////////////////////////////////////////////////////////

static void JS_CreateVocbase(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  CreateVocBase(args, TRI_COL_TYPE_DOCUMENT);
  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief was docuBlock collectionCreateDocumentCollection
////////////////////////////////////////////////////////////////////////////////

static void JS_CreateDocumentCollectionVocbase(
    v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  CreateVocBase(args, TRI_COL_TYPE_DOCUMENT);
  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief was docuBlock collectionCreateEdgeCollection
////////////////////////////////////////////////////////////////////////////////

static void JS_CreateEdgeCollectionVocbase(
    v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  CreateVocBase(args, TRI_COL_TYPE_EDGE);
  TRI_V8_TRY_CATCH_END
}

void TRI_InitV8IndexArangoDB(v8::Isolate* isolate,
                             v8::Handle<v8::ObjectTemplate> rt) {
  TRI_AddMethodVocbase(isolate, rt, TRI_V8_ASCII_STRING(isolate, "_create"),
                       JS_CreateVocbase, true);
  TRI_AddMethodVocbase(isolate, rt,
                       TRI_V8_ASCII_STRING(isolate, "_createEdgeCollection"),
                       JS_CreateEdgeCollectionVocbase);
  TRI_AddMethodVocbase(
      isolate, rt, TRI_V8_ASCII_STRING(isolate, "_createDocumentCollection"),
      JS_CreateDocumentCollectionVocbase);
}

void TRI_InitV8IndexCollection(v8::Isolate* isolate,
                               v8::Handle<v8::ObjectTemplate> rt) {
  TRI_AddMethodVocbase(isolate, rt, TRI_V8_ASCII_STRING(isolate, "dropIndex"),
                       JS_DropIndexVocbaseCol);
  TRI_AddMethodVocbase(isolate, rt, TRI_V8_ASCII_STRING(isolate, "ensureIndex"),
                       JS_EnsureIndexVocbaseCol);
  TRI_AddMethodVocbase(isolate, rt, TRI_V8_ASCII_STRING(isolate, "lookupIndex"),
                       JS_LookupIndexVocbaseCol);
  TRI_AddMethodVocbase(isolate, rt, TRI_V8_ASCII_STRING(isolate, "getIndexes"),
                       JS_GetIndexesVocbaseCol);
  TRI_AddMethodVocbase(
      isolate, rt, TRI_V8_ASCII_STRING(isolate, "indexes"),
      JS_GetIndexesVocbaseCol);  // indexes() is an alias for getIndexes() now!
}
