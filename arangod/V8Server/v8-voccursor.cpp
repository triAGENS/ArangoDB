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

#include "V8Server/v8-voccursor.h"
#include "Aql/Query.h"
#include "Aql/QueryCursor.h"
#include "Aql/QueryResult.h"
#include "Basics/StringUtils.h"
#include "Basics/VelocyPackHelper.h"
#include "Basics/ScopeGuard.h"
#include "Transaction/Context.h"
#include "Transaction/OperationOrigin.h"
#include "Transaction/V8Context.h"
#include "Utils/CollectionNameResolver.h"
#include "Utils/Cursor.h"
#include "Utils/CursorRepository.h"
#include "V8/v8-conv.h"
#include "V8/v8-globals.h"
#include "V8/v8-utils.h"
#include "V8/v8-vpack.h"
#include "v8-vocbaseprivate.h"

#include <velocypack/Iterator.h>

using namespace arangodb;
using namespace arangodb::basics;

////////////////////////////////////////////////////////////////////////////////
/// @brief generates a general cursor from an array
////////////////////////////////////////////////////////////////////////////////

static void JS_CreateCursor(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);
  auto& vocbase = GetContextVocBase(isolate);

  if (args.Length() < 1) {
    TRI_V8_THROW_EXCEPTION_USAGE(
        "CREATE_CURSOR(<data>, <batchSize>, <ttl>, <allowRetry>)");
  }

  if (!args[0]->IsArray()) {
    TRI_V8_THROW_TYPE_ERROR("<data> must be an array");
  }

  // extract objects
  v8::Handle<v8::Array> array = v8::Handle<v8::Array>::Cast(args[0]);
  auto builder = std::make_shared<VPackBuilder>();
  TRI_V8ToVPack(isolate, *builder, array, false);

  // maximum number of results to return at once
  uint32_t batchSize = 1000;

  if (args.Length() >= 2) {
    int64_t maxValue = TRI_ObjectToInt64(isolate, args[1]);
    if (maxValue > 0 && maxValue < (int64_t)UINT32_MAX) {
      batchSize = static_cast<uint32_t>(maxValue);
    }
  }

  double ttl = 0.0;
  if (args.Length() >= 3) {
    ttl = TRI_ObjectToDouble(isolate, args[2]);
  }

  if (ttl <= 0.0) {
    ttl = 30.0;  // default ttl
  }

  bool isRetriable = false;

  if (args.Length() >= 4) {
    isRetriable = TRI_ObjectToBoolean(isolate, args[3]);
  }

  auto origin = transaction::OperationOriginAQL{"running AQL query"};

  arangodb::aql::QueryResult result(TRI_ERROR_NO_ERROR);
  result.data = builder;
  result.cached = false;
  result.context =
      transaction::V8Context::createWhenRequired(vocbase, origin, false);

  TRI_ASSERT(builder.get() != nullptr);

  auto* cursors = vocbase.cursorRepository();  // create a cursor
  arangodb::Cursor* cursor = cursors->createFromQueryResult(
      std::move(result), static_cast<size_t>(batchSize), ttl, true,
      isRetriable);
  TRI_ASSERT(cursor != nullptr);
  auto sg = arangodb::scopeGuard([&]() noexcept { cursors->release(cursor); });

  // need to fetch id before release() as release() might delete the cursor
  CursorId id = cursor->id();

  auto result2 = TRI_V8UInt64String<TRI_voc_tick_t>(isolate, id);
  TRI_V8_RETURN(result2);
  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief generates a JSON object from the specified cursor
////////////////////////////////////////////////////////////////////////////////

static void JS_JsonCursor(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);
  auto& vocbase = GetContextVocBase(isolate);

  if (args.Length() != 1) {
    TRI_V8_THROW_EXCEPTION_USAGE("JSON_CURSOR(<id>)");
  }

  std::string const id = TRI_ObjectToString(isolate, args[0]);
  auto cursorId = static_cast<arangodb::CursorId>(
      arangodb::basics::StringUtils::uint64(id));

  // find the cursor
  auto cursors = vocbase.cursorRepository();
  TRI_ASSERT(cursors != nullptr);

  bool busy;
  Cursor* cursor = cursors->find(cursorId, busy);
  auto sg = arangodb::scopeGuard([&]() noexcept { cursors->release(cursor); });

  if (cursor == nullptr) {
    if (busy) {
      TRI_V8_THROW_EXCEPTION(TRI_ERROR_CURSOR_BUSY);
    }

    TRI_V8_THROW_EXCEPTION(TRI_ERROR_CURSOR_NOT_FOUND);
  }

  VPackOptions* opts = cursor->context()->getVPackOptions();
  VPackBuilder builder(opts);
  builder.openObject(true);  // conversion uses sequential iterator, no indexing
  Result r = cursor->dumpSync(builder);
  if (r.fail()) {
    // On any error the cursor needs to be deleted
    TRI_ASSERT(cursor->isDeleted());
    TRI_V8_THROW_EXCEPTION_MEMORY();  // for compatibility
  }
  builder.close();

  v8::Handle<v8::Value> result = TRI_VPackToV8(isolate, builder.slice());
  TRI_V8_RETURN(result);
  TRI_V8_TRY_CATCH_END
}

// .............................................................................
// cursor v8 class
// .............................................................................

struct V8Cursor final {
  static constexpr uint16_t CID = 4956;

  V8Cursor(v8::Isolate* isolate, v8::Handle<v8::Object> holder,
           TRI_vocbase_t& vocbase, CursorId cursorId)
      : _isolate(isolate),
        _cursorId(cursorId),
        _resolver(vocbase),
        _cte(
            transaction::Context::createCustomTypeHandler(vocbase, _resolver)) {
    TRI_ASSERT(_handle.IsEmpty());
    TRI_ASSERT(holder->InternalFieldCount() > 0);

    // create a new persistent handle
    holder->SetAlignedPointerInInternalField(0, this);
    _handle.Reset(_isolate, holder);
    _handle.SetWrapperClassId(CID);

    // and make it weak, so that we can garbage collect
    _handle.SetWeak(this, weakCallback, v8::WeakCallbackType::kParameter);
    _options.customTypeHandler = _cte.get();
  }

  ~V8Cursor() {
    if (!_handle.IsEmpty()) {
      _handle.ClearWeak();

      _handle.Reset();
    }
    if (_isolate) {
      TRI_v8_global_t* v8g = static_cast<TRI_v8_global_t*>(
          _isolate->GetData(arangodb::V8PlatformFeature::V8_DATA_SLOT));
      TRI_ASSERT(v8g != nullptr);
      TRI_vocbase_t* vocbase = v8g->_vocbase;
      if (vocbase) {
        CursorRepository* cursors = vocbase->cursorRepository();
        cursors->remove(_cursorId);
      }
    }
  }

  //////////////////////////////////////////////////////////////////////////////
  /// @brief unwraps a structure
  //////////////////////////////////////////////////////////////////////////////

  static V8Cursor* unwrap(v8::Local<v8::Object> handle) {
    TRI_ASSERT(handle->InternalFieldCount() > 0);
    return static_cast<V8Cursor*>(
        handle->GetAlignedPointerFromInternalField(0));
  }

  /// @brief return false on error
  bool maybeFetchBatch(v8::Isolate* isolate) {
    if (_dataIterator == nullptr && _hasMore) {  // fetch more data
      TRI_GET_GLOBALS();
      CursorRepository* cursors = v8g->_vocbase->cursorRepository();
      bool busy;
      Cursor* cc = cursors->find(_cursorId, busy);
      if (busy || cc == nullptr) {
        TRI_V8_SET_ERROR(TRI_errno_string(TRI_ERROR_CURSOR_BUSY));
        return false;  // someone else is using it
      }
      auto sg = arangodb::scopeGuard([&]() noexcept { cc->release(); });

      Result r = fetchData(cc);
      if (r.fail()) {
        TRI_CreateErrorObject(isolate, r);
        return false;
      }
    }
    return true;  // still got some data
  }

  /// @brief fetch the next batch
  Result fetchData(Cursor* cursor) {
    TRI_ASSERT(cursor != nullptr && cursor->isUsed());

    TRI_ASSERT(_hasMore);
    TRI_ASSERT(_dataIterator == nullptr);
    _dataSlice = VPackSlice::noneSlice();
    _extraSlice = VPackSlice::noneSlice();

    _tmpResult.clear();
    _tmpResult.openObject();
    Result r = cursor->dumpSync(_tmpResult);
    if (r.fail()) {
      // On any error the cursor needs to be deleted
      TRI_ASSERT(cursor->isDeleted());
      return r;
    }
    _tmpResult.close();

    TRI_ASSERT(_tmpResult.slice().isObject());
    // TODO as an optimization
    for (auto pair : VPackObjectIterator(_tmpResult.slice(), true)) {
      if (pair.key.isEqualString("result")) {
        _dataSlice = pair.value;
        TRI_ASSERT(_dataSlice.isArray());
        if (!_dataSlice.isEmptyArray()) {
          _dataIterator = std::make_unique<VPackArrayIterator>(_dataSlice);
        }
      } else if (pair.key.isEqualString("hasMore")) {
        _hasMore = pair.value.getBool();
      } else if (pair.key.isEqualString("extra")) {
        _extraSlice = pair.value;
      }
    }
    // cursor should delete itself
    TRI_ASSERT(_hasMore || cursor->isDeleted());
    return Result{};
  }

  ////////////////////////////////////////////////////////////////////////////////
  /// @brief constructs a new streaming cursor from arguments
  ////////////////////////////////////////////////////////////////////////////////

  static void New(v8::FunctionCallbackInfo<v8::Value> const& args) {
    TRI_V8_TRY_CATCH_BEGIN(isolate);
    TRI_GET_GLOBALS();

    if (!args.IsConstructCall()) {  // if not call as a constructor call it
      TRI_V8_THROW_EXCEPTION_USAGE("only instance-able by constructor");
    }

    if (args.Length() < 1 || args.Length() > 3) {
      TRI_V8_THROW_EXCEPTION_USAGE(
          "ArangoQueryStreamCursor(<queryString>, <bindVars>, <options>)");
    }

    // get the query string
    if (!args[0]->IsString()) {
      TRI_V8_THROW_TYPE_ERROR("expecting string for <queryString>");
    }
    std::string const queryString(TRI_ObjectToString(isolate, args[0]));

    // bind parameters
    std::shared_ptr<VPackBuilder> bindVars;

    if (args.Length() > 1) {
      if (!args[1]->IsUndefined() && !args[1]->IsNull() &&
          !args[1]->IsObject()) {
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
      // we have options! yikes!
      if (!args[2]->IsObject()) {
        TRI_V8_THROW_TYPE_ERROR("expecting object for <options>");
      }

      TRI_V8ToVPack(isolate, options, args[2], false);
    }
    size_t batchSize = VelocyPackHelper::getNumericValue<size_t>(
        options.slice(), "batchSize", 1000);

    bool isRetriable =
        VelocyPackHelper::getBooleanValue(options.slice(), "allowRetry", false);

    TRI_vocbase_t* vocbase = v8g->_vocbase;
    TRI_ASSERT(vocbase != nullptr);
    auto* cursors = vocbase->cursorRepository();  // create a cursor
    double ttl = std::numeric_limits<double>::max();
    auto origin = transaction::OperationOriginAQL{"running AQL query"};

    auto q = aql::Query::create(
        transaction::V8Context::createWhenRequired(*vocbase, origin, true),
        aql::QueryString(queryString), std::move(bindVars),
        aql::QueryOptions(options.slice()));

    // specify ID 0 so it uses the external V8 context
    Cursor* cc =
        cursors->createQueryStream(std::move(q), batchSize, ttl, isRetriable)
            .waitAndGet();
    // a soft shutdown will throw here!

    arangodb::ScopeGuard releaseCursorGuard([&]() noexcept { cc->release(); });
    // args.Holder() is supposedly better than args.This()
    auto self =
        std::make_unique<V8Cursor>(isolate, args.Holder(), *vocbase, cc->id());
    V8Cursor* v8Cursor = self.release();  // args.Holder() owns the pointer
    Result r = v8Cursor->fetchData(cc);
    if (r.fail()) {
      // Try to free the cursor from cursor repository.
      // We are in NEW so the caller has no chance to do this operation
      // as neither the cursor nor the id is known.
      cursors->release(cc);
      // cursors->release does cc->release() for us.
      releaseCursorGuard.cancel();
      TRI_V8_THROW_EXCEPTION(r);
    } else {
      TRI_V8_RETURN(args.This());
    }
    // do not delete self, its owned by V8 now

    TRI_V8_RETURN(args.This());
    TRI_V8_TRY_CATCH_END
  }

  ////////////////////////////////////////////////////////////////////////////////
  /// @brief ArangoQueryStreamCursor.prototype.toArray = ...
  ////////////////////////////////////////////////////////////////////////////////

  static void toArray(v8::FunctionCallbackInfo<v8::Value> const& args) {
    TRI_V8_TRY_CATCH_BEGIN(isolate);
    v8::HandleScope scope(isolate);
    auto context = TRI_IGETC;

    V8Cursor* self = V8Cursor::unwrap(args.Holder());
    if (self == nullptr) {
      TRI_V8_RETURN_UNDEFINED();
    }

    v8::Handle<v8::Array> resArray = v8::Array::New(isolate);

    // iterate over result and return it
    uint32_t j = 0;
    while (self->maybeFetchBatch(isolate)) {
      if (!self->_dataIterator) {
        break;
      }

      if (V8PlatformFeature::isOutOfMemory(isolate)) {
        TRI_V8_SET_EXCEPTION_MEMORY();
        break;
      }

      while (self->_dataIterator->valid()) {
        VPackSlice s = self->_dataIterator->value();
        resArray->Set(context, j++, TRI_VPackToV8(isolate, s, &self->_options))
            .FromMaybe(false);
        ++(*self->_dataIterator);
      }
      // reset so that the next one can fetch again
      self->_dataIterator.reset();
    }
    TRI_V8_RETURN(resArray);

    TRI_V8_TRY_CATCH_END
  }

  ////////////////////////////////////////////////////////////////////////////////
  /// @brief ArangoQueryStreamCursor.prototype.getExtra = ...
  ////////////////////////////////////////////////////////////////////////////////

  static void getExtra(v8::FunctionCallbackInfo<v8::Value> const& args) {
    TRI_V8_TRY_CATCH_BEGIN(isolate);
    v8::HandleScope scope(isolate);

    V8Cursor* self = V8Cursor::unwrap(args.Holder());
    if (self == nullptr) {
      TRI_V8_RETURN(v8::Undefined(isolate));
    }

    // we always need to fetch
    if (!self->maybeFetchBatch(isolate)) {  // sets exception
      return;
    }

    if (self->_extraSlice.isObject()) {
      TRI_V8_RETURN(TRI_VPackToV8(isolate, self->_extraSlice));
    }

    TRI_V8_THROW_EXCEPTION_MESSAGE(
        TRI_ERROR_BAD_PARAMETER,
        "getExtra() is only valid after all data has been fetched");
    TRI_V8_TRY_CATCH_END
  }

  ////////////////////////////////////////////////////////////////////////////////
  /// @brief ArangoQueryStreamCursor.prototype.hasNext = ...
  ////////////////////////////////////////////////////////////////////////////////

  static void hasNext(v8::FunctionCallbackInfo<v8::Value> const& args) {
    TRI_V8_TRY_CATCH_BEGIN(isolate);
    v8::HandleScope scope(isolate);

    V8Cursor* self = V8Cursor::unwrap(args.Holder());
    if (self == nullptr) {
      TRI_V8_RETURN_UNDEFINED();
    }

    // we always need to fetch
    if (!self->maybeFetchBatch(isolate)) {  // sets exception
      return;
    }

    if (self->_dataIterator != nullptr) {
      TRI_V8_RETURN_TRUE();
    } else {
      TRI_V8_RETURN_FALSE();
    }

    TRI_V8_TRY_CATCH_END
  }

  ////////////////////////////////////////////////////////////////////////////////
  /// @brief ArangoQueryStreamCursor.prototype.next = ...
  ////////////////////////////////////////////////////////////////////////////////

  static void next(v8::FunctionCallbackInfo<v8::Value> const& args) {
    TRI_V8_TRY_CATCH_BEGIN(isolate);
    v8::HandleScope scope(isolate);

    V8Cursor* self = V8Cursor::unwrap(args.Holder());
    if (self == nullptr) {
      TRI_V8_RETURN_UNDEFINED();
    }

    // we always need to fetch
    if (!self->maybeFetchBatch(isolate)) {  // sets exception
      return;
    }

    if (self->_dataIterator) {  // got a current batch
      TRI_ASSERT(self->_dataIterator->valid());

      VPackSlice s = self->_dataIterator->value();
      v8::Local<v8::Value> val = TRI_VPackToV8(isolate, s, &self->_options);

      ++(*self->_dataIterator);
      // reset so that the next one can fetch again
      if (!self->_dataIterator->valid()) {
        self->_dataIterator.reset();
      }
      TRI_V8_RETURN(val);
    }

    TRI_V8_RETURN_UNDEFINED();
    TRI_V8_TRY_CATCH_END
  }

  ////////////////////////////////////////////////////////////////////////////////
  /// @brief ArangoQueryStreamCursor.prototype.count = ...
  ////////////////////////////////////////////////////////////////////////////////

  static void count(v8::FunctionCallbackInfo<v8::Value> const& args) {
    TRI_V8_TRY_CATCH_BEGIN(isolate);
    v8::HandleScope scope(isolate);
    TRI_V8_RETURN_UNDEFINED();  // always undefined
    TRI_V8_TRY_CATCH_END
  }

  ////////////////////////////////////////////////////////////////////////////////
  /// @brief explicitly discard cursor, mostly relevant for testing
  ////////////////////////////////////////////////////////////////////////////////

  static void dispose(v8::FunctionCallbackInfo<v8::Value> const& args) {
    TRI_V8_TRY_CATCH_BEGIN(isolate);
    V8Cursor* self = V8Cursor::unwrap(args.Holder());
    if (self != nullptr) {
      TRI_GET_GLOBALS();
      CursorRepository* cursors = v8g->_vocbase->cursorRepository();
      cursors->remove(self->_cursorId);
      self->_hasMore = false;
      self->_dataSlice = VPackSlice::noneSlice();
      self->_extraSlice = VPackSlice::noneSlice();
      self->_dataIterator.reset();
      self->_tmpResult.clear();
    }
    TRI_V8_TRY_CATCH_END
  }

  ////////////////////////////////////////////////////////////////////////////////
  /// @brief ArangoQueryStreamCursor.prototype.id = ...
  ////////////////////////////////////////////////////////////////////////////////

  static void id(v8::FunctionCallbackInfo<v8::Value> const& args) {
    v8::Isolate* isolate = args.GetIsolate();
    v8::HandleScope scope(isolate);
    V8Cursor* self = V8Cursor::unwrap(args.Holder());
    if (self == nullptr) {
      TRI_V8_RETURN(v8::Undefined(isolate));
    }
    TRI_V8_RETURN(TRI_V8UInt64String<TRI_voc_tick_t>(isolate, self->_cursorId));
  }

 private:
  /// called when GC deletes the value
  static void weakCallback(const v8::WeakCallbackInfo<V8Cursor>& data) {
    auto obj = data.GetParameter();
    delete obj;
  }

 private:
  /// @brief persistent handle for V8 object
  v8::Persistent<v8::Object> _handle;
  /// @brief isolate
  v8::Isolate* _isolate;
  /// @brief temporary result buffer
  VPackBuilder _tmpResult;
  /// @brief id of cursor
  CursorId _cursorId;

  /// cache has more variable
  bool _hasMore = true;
  VPackSlice _dataSlice = VPackSlice::noneSlice();
  /// cached extras which might be attached to the stream
  VPackSlice _extraSlice = VPackSlice::noneSlice();
  /// @brief pointer to the current result
  std::unique_ptr<VPackArrayIterator> _dataIterator;

  CollectionNameResolver _resolver;
  std::shared_ptr<VPackCustomTypeHandler> _cte;
  VPackOptions _options = VPackOptions::Defaults;
};

// .............................................................................
// generate the general cursor template
// .............................................................................

void TRI_InitV8cursor(v8::Handle<v8::Context> context, TRI_v8_global_t* v8g) {
  v8::Isolate* isolate = v8::Isolate::GetCurrent();

  // cursor functions. not intended to be used by end users
  // these cursor functions are the APIs implemented in js/actions/api-simple.js
  TRI_AddGlobalFunctionVocbase(isolate,
                               TRI_V8_ASCII_STRING(isolate, "CREATE_CURSOR"),
                               JS_CreateCursor, true);
  TRI_AddGlobalFunctionVocbase(isolate,
                               TRI_V8_ASCII_STRING(isolate, "JSON_CURSOR"),
                               JS_JsonCursor, true);

  // streaming query cursor class, intended to be used via
  // ArangoStatement.execute
  v8::Handle<v8::ObjectTemplate> rt;
  v8::Handle<v8::FunctionTemplate> ft;

  ft = v8::FunctionTemplate::New(isolate, V8Cursor::New);
  ft->SetClassName(TRI_V8_ASCII_STRING(isolate, "ArangoQueryStreamCursor"));

  rt = ft->InstanceTemplate();
  rt->SetInternalFieldCount(1);

  ft->PrototypeTemplate()->Set(
      TRI_V8_ASCII_STRING(isolate, "isArangoResultSet"), v8::True(isolate));
  TRI_V8_AddProtoMethod(isolate, ft, TRI_V8_ASCII_STRING(isolate, "toArray"),
                        V8Cursor::toArray);
  TRI_V8_AddProtoMethod(isolate, ft, TRI_V8_ASCII_STRING(isolate, "getExtra"),
                        V8Cursor::getExtra);
  TRI_V8_AddProtoMethod(isolate, ft, TRI_V8_ASCII_STRING(isolate, "hasNext"),
                        V8Cursor::hasNext);
  TRI_V8_AddProtoMethod(isolate, ft, TRI_V8_ASCII_STRING(isolate, "next"),
                        V8Cursor::next);
  TRI_V8_AddProtoMethod(isolate, ft, TRI_V8_ASCII_STRING(isolate, "count"),
                        V8Cursor::count);
  TRI_V8_AddProtoMethod(isolate, ft, TRI_V8_ASCII_STRING(isolate, "dispose"),
                        V8Cursor::dispose);
  TRI_V8_AddProtoMethod(isolate, ft, TRI_V8_ASCII_STRING(isolate, "id"),
                        V8Cursor::id);

  v8g->StreamQueryCursorTempl.Reset(isolate, ft);
  v8::MaybeLocal<v8::Function> ctor = ft->GetFunction(context);
  if (ctor.IsEmpty()) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL,
                                   "error creating v8 stream cursor");
  }
  // ft->SetClassName(TRI_V8_ASCII_STRING(isolate,
  // "ArangoStreamQueryCursorCtor"));
  TRI_AddGlobalFunctionVocbase(
      isolate, TRI_V8_ASCII_STRING(isolate, "ArangoQueryStreamCursor"),
      ctor.ToLocalChecked(), true);
}
