////////////////////////////////////////////////////////////////////////////////
/// @brief replication logger
///
/// @file
///
/// DISCLAIMER
///
/// Copyright 2004-2013 triAGENS GmbH, Cologne, Germany
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
/// Copyright holder is triAGENS GmbH, Cologne, Germany
///
/// @author Jan Steemann
/// @author Copyright 2011-2013, triAGENS GmbH, Cologne, Germany
////////////////////////////////////////////////////////////////////////////////

#include "replication-logger.h"

#include "BasicsC/conversions.h"
#include "BasicsC/files.h"
#include "BasicsC/json.h"
#include "BasicsC/logging.h"
#include "BasicsC/string-buffer.h"
#include "BasicsC/tri-strings.h"

#include "VocBase/collection.h"
#include "VocBase/datafile.h"
#include "VocBase/document-collection.h"
#include "VocBase/server.h"
#include "VocBase/transaction.h"
#include "VocBase/vocbase.h"
#include "VocBase/voc-shaper.h"

// -----------------------------------------------------------------------------
// --SECTION--                                                REPLICATION LOGGER
// -----------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// --SECTION--                                                   CLIENT HANDLING
// -----------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// --SECTION--                                                     private types
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief struct to hold a client action
////////////////////////////////////////////////////////////////////////////////

typedef struct logger_client_s {
  TRI_server_id_t _serverId;
  TRI_voc_tick_t  _lastServedTick;
  char            _stamp[24];
}
logger_client_t;

// -----------------------------------------------------------------------------
// --SECTION--                                                 private functions
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief hashes a client id
////////////////////////////////////////////////////////////////////////////////

static uint64_t HashKeyClient (TRI_associative_pointer_t* array, 
                               void const* key) {
  TRI_server_id_t const* k = static_cast<TRI_server_id_t const*>(key);

  return (uint64_t) *k;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief hashes a client struct
////////////////////////////////////////////////////////////////////////////////

static uint64_t HashElementClient (TRI_associative_pointer_t* array, 
                                   void const* element) {
  logger_client_t const* e = static_cast<logger_client_t const*>(element);
  
  return (uint64_t) e->_serverId;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief compares a client key and a client struct
////////////////////////////////////////////////////////////////////////////////

static bool IsEqualKeyClient (TRI_associative_pointer_t* array, 
                              void const* key, 
                              void const* element) {
  TRI_server_id_t const* k = static_cast<TRI_server_id_t const*>(key);
  logger_client_t const* e = static_cast<logger_client_t const*>(element);

  return *k == e->_serverId;
}
      
////////////////////////////////////////////////////////////////////////////////
/// @brief free a single registered client
////////////////////////////////////////////////////////////////////////////////

static void FreeClient (logger_client_t* client) {
  TRI_Free(TRI_UNKNOWN_MEM_ZONE, client);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief free registered clients
////////////////////////////////////////////////////////////////////////////////

static void FreeClients (TRI_replication_logger_t* logger) {
  uint32_t i, n;
  
  n = logger->_clients._nrAlloc;

  for (i = 0; i < n; ++i) {
    logger_client_t* client = static_cast<logger_client_t*>(logger->_clients._table[i]);

    if (client != NULL) {
      FreeClient(client);
    }
  }
}

// -----------------------------------------------------------------------------
// --SECTION--                                                           LOGGING
// -----------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// --SECTION--                                                   private defines
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief shortcut function
////////////////////////////////////////////////////////////////////////////////

#define FAIL_IFNOT(func, buffer, val)                                     \
  if (func(buffer, val) != TRI_ERROR_NO_ERROR) {                          \
    return false;                                                         \
  }

////////////////////////////////////////////////////////////////////////////////
/// @brief create a string-buffer function name
////////////////////////////////////////////////////////////////////////////////

#define APPEND_FUNC(name) TRI_ ## name ## StringBuffer

////////////////////////////////////////////////////////////////////////////////
/// @brief append a character to a string-buffer or fail
////////////////////////////////////////////////////////////////////////////////

#define APPEND_CHAR(buffer, c)      FAIL_IFNOT(APPEND_FUNC(AppendChar), buffer, c)

////////////////////////////////////////////////////////////////////////////////
/// @brief append a string to a string-buffer or fail
////////////////////////////////////////////////////////////////////////////////

#define APPEND_STRING(buffer, str)  FAIL_IFNOT(APPEND_FUNC(AppendString), buffer, str)

////////////////////////////////////////////////////////////////////////////////
/// @brief append uint64 to a string-buffer or fail
////////////////////////////////////////////////////////////////////////////////

#define APPEND_UINT64(buffer, val)  FAIL_IFNOT(APPEND_FUNC(AppendUInt64), buffer, val)

////////////////////////////////////////////////////////////////////////////////
/// @brief append json to a string-buffer or fail
////////////////////////////////////////////////////////////////////////////////

#define APPEND_JSON(buffer, json)   FAIL_IFNOT(TRI_StringifyJson, buffer, json)

// -----------------------------------------------------------------------------
// --SECTION--                                                 private variables
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief number of pre-allocated string buffers for logging
////////////////////////////////////////////////////////////////////////////////

static size_t NumBuffers = 16;

////////////////////////////////////////////////////////////////////////////////
/// @brief pre-allocated size for each log buffer
////////////////////////////////////////////////////////////////////////////////

static size_t BufferSize = 256;

// -----------------------------------------------------------------------------
// --SECTION--                                                 private functions
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief free the logger's cap constraint
/// the function must called under the statusLock
////////////////////////////////////////////////////////////////////////////////
    
static void FreeCap (TRI_replication_logger_t* logger) {
  TRI_ASSERT(logger != NULL);

  if (logger->_cap != NULL) {
  
    TRI_ASSERT(logger->_trxCollection != NULL);
    TRI_ASSERT(logger->_trxCollection->_collection != NULL);

    TRI_document_collection_t* document = logger->_trxCollection->_collection->_collection;
    TRI_ASSERT(document != NULL);

    // TODO: this wont work anymore - remove function altogether
    TRI_DropIndexDocumentCollection(document,
                                    logger->_cap->_iid,
                                    TRI_GetIdServer());

    logger->_cap = NULL;
  }
}

////////////////////////////////////////////////////////////////////////////////
/// @brief create a cap constraint for the logger
/// the function must called under the statusLock
////////////////////////////////////////////////////////////////////////////////

static bool CreateCap (TRI_replication_logger_t* logger) {
  TRI_index_t* idx;
  size_t maxEvents;
  int64_t maxEventsSize;

  if (logger->_configuration._maxEvents == 0ULL &&
      logger->_configuration._maxEventsSize == 0ULL) {
    return true;
  }

  TRI_document_collection_t* document = logger->_trxCollection->_collection->_collection;
  TRI_ASSERT(document != NULL);

  TRI_ASSERT(logger->_configuration._maxEvents > 0ULL ||
         logger->_configuration._maxEventsSize > 0ULL);

  LOG_TRACE("creating cap constraint for replication logger. maxEvents: %llu, maxEventsSize: %llu",
            (unsigned long long) logger->_configuration._maxEvents,
            (unsigned long long) logger->_configuration._maxEventsSize);
  
  // need to convert to (possibly) lower types
  if (logger->_configuration._maxEvents > (uint64_t) SIZE_MAX) {
    maxEvents = SIZE_MAX;
  }
  else {
    maxEvents = (size_t) logger->_configuration._maxEvents;
  }

  if (logger->_configuration._maxEventsSize > (uint64_t) INT64_MAX) {
    maxEventsSize = INT64_MAX;
  }
  else {
    maxEventsSize = (int64_t) logger->_configuration._maxEventsSize;
  }

  idx = TRI_EnsureCapConstraintDocumentCollection(document,
                                                  0,
                                                  maxEvents,
                                                  maxEventsSize,
                                                  NULL,
                                                  TRI_GetIdServer());

  if (idx == NULL) {
    LOG_WARNING("creating cap constraint for '%s' failed", TRI_COL_NAME_REPLICATION);
    return false;
  }
    
  logger->_cap = idx;

  return true;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief get a buffer to write an event in
////////////////////////////////////////////////////////////////////////////////

static TRI_string_buffer_t* GetBuffer (TRI_replication_logger_t* logger) {
  size_t n;
   
  TRI_ASSERT(logger != NULL);
  TRI_string_buffer_t* buffer = NULL;

  // locked section
  // ---------------------------------------
  TRI_LockSpin(&logger->_bufferLock);

  n = logger->_buffers._length;

  if (n > 0) {
    buffer = static_cast<TRI_string_buffer_t*>(TRI_RemoveVectorPointer(&logger->_buffers, (size_t) (n - 1)));
  }

  TRI_UnlockSpin(&logger->_bufferLock);
  // ---------------------------------------
  // locked section end

  TRI_ASSERT(buffer != NULL);

  return buffer;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief returns a buffer to the list of available buffers
////////////////////////////////////////////////////////////////////////////////

static void ReturnBuffer (TRI_replication_logger_t* logger,
                          TRI_string_buffer_t* buffer) {
  TRI_ASSERT(logger != NULL);
  TRI_ASSERT(buffer != NULL);

  // make the buffer usable again
  if (buffer->_buffer == NULL) {
    TRI_InitSizedStringBuffer(buffer, TRI_CORE_MEM_ZONE, BufferSize);
  }
  else {
    TRI_ResetStringBuffer(buffer);
  }

  // locked section
  // ---------------------------------------
  TRI_LockSpin(&logger->_bufferLock);

  TRI_PushBackVectorPointer(&logger->_buffers, buffer);
  TRI_ASSERT(logger->_buffers._length <= NumBuffers);
  
  TRI_UnlockSpin(&logger->_bufferLock);
  // ---------------------------------------
  // locked section end
}

////////////////////////////////////////////////////////////////////////////////
/// @brief logs a replication event contained in the buffer
/// the function will always free the buffer passed
////////////////////////////////////////////////////////////////////////////////

static int LogEvent (TRI_replication_logger_t* logger,
                     TRI_voc_tid_t tid,
                     bool isStandaloneOperation,
                     TRI_replication_operation_e type,
                     TRI_string_buffer_t* buffer) {
  TRI_memory_zone_t* zone;
  TRI_shaped_json_t* shaped;
  TRI_json_t json;
  TRI_doc_mptr_copy_t mptr;
  size_t len;
  int res;
  bool forceSync;
  bool withTid;
  bool lock;

  TRI_ASSERT(logger != NULL);
  TRI_ASSERT(buffer != NULL);

  len = TRI_LengthStringBuffer(buffer);

  if (len < 1) {
    // buffer is empty
    ReturnBuffer(logger, buffer);

    return TRI_ERROR_NO_ERROR;
  }
  
  // create fake transaction to prevent assertion error. TODO: FIXME
  triagens::arango::TransactionBase fake(true);

  // do we have a transaction id?
  withTid = (tid > 0);

  // this type of operation will be synced. all other operations will not be synced.
  forceSync = (type == REPLICATION_STOP);
 
  TRI_InitArray2Json(TRI_CORE_MEM_ZONE, &json, withTid ? 3 : 2);

  // add "type" attribute
  {
    TRI_json_t typeAttribute;
    TRI_InitNumberJson(&typeAttribute, (double) type);

    TRI_Insert4ArrayJson(TRI_CORE_MEM_ZONE, 
                         &json, 
                         (char*) "type", 
                         4, // strlen("type")
                         &typeAttribute,
                         true);
  }

  // "tid" attribute
  if (withTid) {
    TRI_json_t tidAttribute;
    TRI_InitStringJson(&tidAttribute, TRI_StringUInt64(tid));

    TRI_Insert4ArrayJson(TRI_CORE_MEM_ZONE, 
                         &json, 
                         (char*) "tid", 
                         3, // strlen("tid")
                         &tidAttribute,
                         true);
  }

  // "data" attribute
  {
    TRI_json_t dataAttribute;
    // pass the string-buffer buffer pointer to the JSON
    TRI_InitStringReference2Json(&dataAttribute, TRI_BeginStringBuffer(buffer), TRI_LengthStringBuffer(buffer));

    TRI_Insert4ArrayJson(TRI_CORE_MEM_ZONE, 
                         &json, 
                         (char*) "data", 
                         4, // strlen("data")
                         &dataAttribute,
                         true);
  }

  LOG_TRACE("logging replication event, type: %d, tid: %llu, sync: %d, data: %s", 
            (int) type,
            (unsigned long long) tid,
            (int) forceSync,
            TRI_BeginStringBuffer(buffer));

  lock = isStandaloneOperation;
  
  TRI_document_collection_t* document = logger->_trxCollection->_collection->_collection;
  zone = document->getShaper()->_memoryZone;  // ONLY IN INDEX, PROTECTED by RUNTIME
  shaped = TRI_ShapedJsonJson(document->getShaper(), &json, true, ! lock);  // ONLY IN INDEX, PROTECTED by RUNTIME
  TRI_DestroyJson(TRI_CORE_MEM_ZONE, &json);
  
  ReturnBuffer(logger, buffer);

  if (shaped == NULL) {
    return TRI_ERROR_ARANGO_SHAPER_FAILED;
  }

  res = TRI_InsertShapedJsonDocumentCollection(logger->_trxCollection, 
                        NULL, 
                        0,
                        &mptr, 
                        shaped, 
                        NULL, 
                        lock, 
                        forceSync, 
                        false);

  TRI_FreeShapedJson(zone, shaped);

  if (res != TRI_ERROR_NO_ERROR) {
    return res;
  }

  // TRI_ASSERT the write was successful
  TRI_ASSERT(mptr.getDataPtr() != NULL);  // PROTECTED by trx in logger
  
  // update the last tick that we've logged
  TRI_LockSpin(&logger->_idLock);

  logger->_state._lastLogTick = ((TRI_df_marker_t*) mptr.getDataPtr())->_tick;  // PROTECTED by trx in logger
  logger->_state._totalEvents++;

  TRI_UnlockSpin(&logger->_idLock);

  return TRI_ERROR_NO_ERROR;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief stringify a "replication" operation with a tick
////////////////////////////////////////////////////////////////////////////////

static bool StringifyTickReplication (TRI_string_buffer_t* buffer,
                                      TRI_voc_tick_t tick) {
  if (buffer == NULL) {
    return false;
  }

  APPEND_STRING(buffer, "{\"lastTick\":\"");
  APPEND_UINT64(buffer, (uint64_t) tick);
  APPEND_STRING(buffer, "\"}");

  return true;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief get current state from the replication logger
/// note: must hold the lock when calling this
////////////////////////////////////////////////////////////////////////////////

static int GetStateReplicationLogger (TRI_replication_logger_t* logger,
                                      TRI_replication_logger_state_t* dst) {
  TRI_ASSERT(logger->_state._active);

  TRI_LockSpin(&logger->_idLock);
  memcpy(dst, &logger->_state, sizeof(TRI_replication_logger_state_t));
  TRI_UnlockSpin(&logger->_idLock);

  return TRI_ERROR_NO_ERROR;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief start the replication logger
/// note: must hold the lock when calling this
////////////////////////////////////////////////////////////////////////////////

static int StartReplicationLogger (TRI_replication_logger_t* logger) {
  TRI_transaction_t* trx;
  TRI_vocbase_col_t* collection;
  TRI_vocbase_t* vocbase;
  TRI_transaction_hint_t hint;
  TRI_string_buffer_t* buffer;
  TRI_voc_cid_t cid;
  int res;
  
  if (logger->_state._active) {
    return TRI_ERROR_INTERNAL;
  }

  TRI_ASSERT(logger->_trx == NULL);
  TRI_ASSERT(logger->_trxCollection == NULL);
  TRI_ASSERT(logger->_state._lastLogTick == 0);

  vocbase = logger->_vocbase;
  collection = TRI_LookupCollectionByNameVocBase(vocbase, TRI_COL_NAME_REPLICATION);

  if (collection == NULL) {
    // try to create _replication collection on the fly
    TRI_col_info_t parameter;
    
    TRI_InitCollectionInfo(vocbase, 
                           &parameter, 
                           TRI_COL_NAME_REPLICATION,
                           (TRI_col_type_e) TRI_COL_TYPE_DOCUMENT, 
                           (TRI_voc_size_t) vocbase->_settings.defaultMaximalSize, 
                           NULL);
    
    parameter._isSystem = true;

    collection = TRI_CreateCollectionVocBase(vocbase, &parameter, 0, TRI_GetIdServer());
    TRI_FreeCollectionInfoOptions(&parameter);
    
    if (collection != NULL) {
      LOG_INFO("created collection '" TRI_COL_NAME_REPLICATION "'");
    }
  }

  if (collection == NULL) {
    LOG_ERROR("could not open collection '" TRI_COL_NAME_REPLICATION "'");

    return TRI_ERROR_ARANGO_COLLECTION_NOT_FOUND;
  }

  cid = collection->_cid;
  // create fake transaction to prevent assertion error. TODO: FIXME
  triagens::arango::TransactionBase fake(true);

  trx = TRI_CreateTransaction(vocbase, TRI_GetIdServer(), false, 0.0, false);

  if (trx == NULL) {
    return TRI_ERROR_OUT_OF_MEMORY;
  }

  res = TRI_AddCollectionTransaction(trx, cid, TRI_TRANSACTION_WRITE, TRI_TRANSACTION_TOP_LEVEL);

  if (res != TRI_ERROR_NO_ERROR) {
    TRI_FreeTransaction(trx);

    return TRI_ERROR_INTERNAL;
  }

  // the SINGLE_OPERATION hint is actually a hack:
  // the logger does not write just one operation, but it is used to prevent locking the collection
  // for the entire duration of the transaction
  hint = (TRI_transaction_hint_t) TRI_TRANSACTION_HINT_SINGLE_OPERATION;
  res = TRI_BeginTransaction(trx, hint, TRI_TRANSACTION_TOP_LEVEL);

  if (res != TRI_ERROR_NO_ERROR) {
    TRI_FreeTransaction(trx);
    
    return TRI_ERROR_INTERNAL;
  }

  logger->_trxCollection = TRI_GetCollectionTransaction(trx, cid, TRI_TRANSACTION_WRITE);
  logger->_trx = trx;

  TRI_ASSERT(logger->_trxCollection != NULL);
  TRI_ASSERT(logger->_trxCollection->_collection != NULL);
  TRI_ASSERT(logger->_trxCollection->_collection->_collection != NULL);
  TRI_ASSERT(logger->_state._active == false);

  TRI_ASSERT(logger->_cap == NULL);
  // create cap constraint? 
  if (logger->_configuration._maxEvents > 0ULL || 
      logger->_configuration._maxEventsSize > 0ULL) { 

    CreateCap(logger);
  }

  logger->_state._lastLogTick = ((TRI_collection_t*) collection->_collection)->_info._revision;
  logger->_state._active      = true;
  
  LOG_INFO("started replication logger for database '%s', last tick: %llu", 
           logger->_databaseName,
           (unsigned long long) logger->_state._lastLogTick);
  
  buffer = GetBuffer(logger);
  
  if (! StringifyTickReplication(buffer, logger->_state._lastLogTick)) {
    ReturnBuffer(logger, buffer);

    return TRI_ERROR_OUT_OF_MEMORY;
  }
 
  res = LogEvent(logger, 0, true, REPLICATION_START, buffer); 

  return res;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief stop the replication logger
/// note: must hold the lock when calling this
////////////////////////////////////////////////////////////////////////////////

static int StopReplicationLogger (TRI_replication_logger_t* logger) {
  TRI_string_buffer_t* buffer;
  TRI_voc_tick_t lastTick;
  int res;

  if (! logger->_state._active) {
    return TRI_ERROR_INTERNAL;
  }

  TRI_LockSpin(&logger->_idLock);
  lastTick = logger->_state._lastLogTick;
  TRI_UnlockSpin(&logger->_idLock);

  TRI_ASSERT(logger->_trx != NULL);
  TRI_ASSERT(logger->_trxCollection != NULL);

  buffer = GetBuffer(logger);
  
  if (! StringifyTickReplication(buffer, lastTick)) {
    ReturnBuffer(logger, buffer);

    return TRI_ERROR_OUT_OF_MEMORY;
  }
  
  res = LogEvent(logger, 0, true, REPLICATION_STOP, buffer); 
  
  // destroy cap constraint
  FreeCap(logger);

  TRI_CommitTransaction(logger->_trx, 0);
  
  TRI_FreeTransaction(logger->_trx);
  
  LOG_INFO("stopped replication logger for database '%s', last tick: %llu", 
           logger->_databaseName,
           (unsigned long long) lastTick);

  logger->_trx                = NULL;
  logger->_trxCollection      = NULL;
  logger->_state._lastLogTick = 0;
  logger->_state._active      = false;

  return res;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief get the state of the _replication collection for a non-running
/// replication logger
/// note: must hold the lock when calling this
////////////////////////////////////////////////////////////////////////////////

static int GetStateInactive (TRI_replication_logger_t* logger,
                             TRI_replication_logger_state_t* dst) {
  TRI_vocbase_t* vocbase;
  TRI_vocbase_col_t* col;

  vocbase = logger->_vocbase;

  if (vocbase->_type == TRI_VOCBASE_TYPE_COORDINATOR) {
    dst->_lastLogTick  = 0;
    dst->_totalEvents  = 0;
    dst->_active       = false;
    return TRI_ERROR_NO_ERROR;
  }

  TRI_vocbase_col_status_e status; 
  col = TRI_UseCollectionByNameVocBase(vocbase, TRI_COL_NAME_REPLICATION, status);

  if (col == NULL || col->_collection == NULL) {
    LOG_ERROR("could not open collection '" TRI_COL_NAME_REPLICATION "'");

    return TRI_ERROR_ARANGO_COLLECTION_NOT_FOUND;
  }

  TRI_document_collection_t* document = col->_collection;

  dst->_lastLogTick  = document->_info._revision;
  dst->_totalEvents  = 0;
  dst->_active       = false;

  TRI_ReleaseCollectionVocBase(vocbase, col);
  
  TRI_LockSpin(&logger->_idLock);
  dst->_totalEvents  = logger->_state._totalEvents;
  TRI_UnlockSpin(&logger->_idLock);

  return TRI_ERROR_NO_ERROR;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief free all allocated buffers
////////////////////////////////////////////////////////////////////////////////

static void FreeBuffers (TRI_replication_logger_t* logger) {
  size_t i, n;

  LOG_TRACE("freeing buffers");

  n = logger->_buffers._length;

  for (i = 0; i < n; ++i) {
    TRI_string_buffer_t* buffer = (TRI_string_buffer_t*) TRI_AtVectorPointer(&logger->_buffers, i);

    TRI_ASSERT(buffer != NULL);
    TRI_FreeStringBuffer(TRI_CORE_MEM_ZONE, buffer);
  }

  TRI_DestroyVectorPointer(&logger->_buffers);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief initialise buffers
////////////////////////////////////////////////////////////////////////////////

static int InitBuffers (TRI_replication_logger_t* logger) {
  size_t i;
  int res;

  TRI_ASSERT(NumBuffers > 0);
  
  LOG_TRACE("initialising buffers");

  res = TRI_InitVectorPointer2(&logger->_buffers, TRI_CORE_MEM_ZONE, NumBuffers); 

  if (res != TRI_ERROR_NO_ERROR) {
    return res;
  }

  for (i = 0; i < NumBuffers; ++i) {
    TRI_string_buffer_t* buffer = TRI_CreateSizedStringBuffer(TRI_CORE_MEM_ZONE, BufferSize);

    if (buffer == NULL) {
      FreeBuffers(logger);

      return TRI_ERROR_OUT_OF_MEMORY;
    }

    TRI_PushBackVectorPointer(&logger->_buffers, buffer);
  }

  TRI_ASSERT(logger->_buffers._length == NumBuffers);

  return TRI_ERROR_NO_ERROR;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief get the filename of the replication logger configuration file
////////////////////////////////////////////////////////////////////////////////

static char* GetConfigurationFilename (TRI_vocbase_t* vocbase) {
  return TRI_Concatenate2File(vocbase->_path, "REPLICATION-LOGGER-CONFIG");
}

// -----------------------------------------------------------------------------
// --SECTION--                                        constructors / destructors
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief create a replication logger
////////////////////////////////////////////////////////////////////////////////

TRI_replication_logger_t* TRI_CreateReplicationLogger (TRI_vocbase_t* vocbase) {
  TRI_replication_logger_t* logger = static_cast<TRI_replication_logger_t*>(TRI_Allocate(TRI_CORE_MEM_ZONE, sizeof(TRI_replication_logger_t), false));

  if (logger == NULL) {
    return NULL;
  }
 
  // init string buffers 
  int res = InitBuffers(logger);

  if (res != TRI_ERROR_NO_ERROR) {
    // out of memory
    TRI_Free(TRI_CORE_MEM_ZONE, logger);

    return NULL;
  }
  
  res = TRI_InitAssociativePointer(&logger->_clients,
                                   TRI_UNKNOWN_MEM_ZONE,
                                   HashKeyClient,
                                   HashElementClient,
                                   IsEqualKeyClient,
                                   NULL);

  if (res != TRI_ERROR_NO_ERROR) {
    // out of memory
    FreeBuffers(logger);
    TRI_Free(TRI_CORE_MEM_ZONE, logger);

    return NULL;
  }

  TRI_InitReadWriteLock(&logger->_statusLock);
  TRI_InitReadWriteLock(&logger->_clientsLock);
  TRI_InitSpin(&logger->_idLock);
  TRI_InitSpin(&logger->_bufferLock);

  logger->_vocbase                         = vocbase;
  logger->_trx                             = NULL;
  logger->_trxCollection                   = NULL;
  logger->_cap                             = NULL;

  logger->_state._lastLogTick              = 0;
  logger->_state._totalEvents              = 0;
  logger->_state._active                   = false;
  logger->_configuration._logRemoteChanges = false;
  logger->_configuration._maxEvents        = (uint64_t) TRI_REPLICATION_LOGGER_EVENTS_DEFAULT;
  logger->_configuration._maxEventsSize    = (uint64_t) TRI_REPLICATION_LOGGER_SIZE_DEFAULT;
  logger->_configuration._autoStart        = false; 

  logger->_localServerId                   = TRI_GetIdServer();
  logger->_databaseName                    = TRI_DuplicateStringZ(TRI_CORE_MEM_ZONE, vocbase->_name);

  TRI_ASSERT(logger->_databaseName != NULL);
  
  // check if there is a configuration file to load
  char* filename = GetConfigurationFilename(vocbase);

  if (filename != NULL) {
    LOG_TRACE("looking for replication logger configuration in '%s'", filename);

    if (TRI_ExistsFile(filename)) {
      TRI_json_t* json;

      LOG_TRACE("loading replication logger configuration from '%s'", filename);

      json = TRI_JsonFile(TRI_CORE_MEM_ZONE, filename, NULL);

      if (TRI_IsArrayJson(json)) {
        TRI_json_t const* value;

        value =  TRI_LookupArrayJson(json, "autoStart");

        if (value != NULL && value->_type == TRI_JSON_BOOLEAN) {
          logger->_configuration._autoStart = value->_value._boolean;
        }

        value =  TRI_LookupArrayJson(json, "logRemoteChanges");

        if (value != NULL && value->_type == TRI_JSON_BOOLEAN) {
          logger->_configuration._logRemoteChanges = value->_value._boolean;
        }

        value =  TRI_LookupArrayJson(json, "maxEvents");

        if (value != NULL && value->_type == TRI_JSON_NUMBER) {
          logger->_configuration._maxEvents = (uint64_t) value->_value._number;
        }

        value =  TRI_LookupArrayJson(json, "maxEventsSize");

        if (value != NULL && value->_type == TRI_JSON_NUMBER) {
          logger->_configuration._maxEventsSize = (uint64_t) value->_value._number;
        }
      }

      if (json != NULL) {
        TRI_FreeJson(TRI_CORE_MEM_ZONE, json);
      }
    }

    TRI_FreeString(TRI_CORE_MEM_ZONE, filename);
  }

  return logger;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief destroy a replication logger
////////////////////////////////////////////////////////////////////////////////

void TRI_DestroyReplicationLogger (TRI_replication_logger_t* logger) {
  TRI_StopReplicationLogger(logger);

  FreeBuffers(logger);
 
  FreeClients(logger);
  TRI_DestroyAssociativePointer(&logger->_clients);

  TRI_FreeString(TRI_CORE_MEM_ZONE, logger->_databaseName);
  TRI_DestroySpin(&logger->_bufferLock);
  TRI_DestroySpin(&logger->_idLock);
  TRI_DestroyReadWriteLock(&logger->_clientsLock);
  TRI_DestroyReadWriteLock(&logger->_statusLock);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief free a replication logger
////////////////////////////////////////////////////////////////////////////////

void TRI_FreeReplicationLogger (TRI_replication_logger_t* logger) {
  TRI_DestroyReplicationLogger(logger);
  TRI_Free(TRI_CORE_MEM_ZONE, logger);
}

// -----------------------------------------------------------------------------
// --SECTION--                                                  public functions
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief get a JSON representation of the replication logger configuration
////////////////////////////////////////////////////////////////////////////////

TRI_json_t* TRI_JsonConfigurationReplicationLogger (TRI_replication_logger_configuration_t const* config) {
  TRI_json_t* json;

  json = TRI_CreateArray2Json(TRI_CORE_MEM_ZONE, 3);

  if (json == NULL) {
    return NULL;
  }
  
  TRI_Insert3ArrayJson(TRI_CORE_MEM_ZONE,
                       json,
                       "autoStart",
                       TRI_CreateBooleanJson(TRI_CORE_MEM_ZONE, config->_autoStart));

  TRI_Insert3ArrayJson(TRI_CORE_MEM_ZONE,
                       json,
                       "logRemoteChanges",
                       TRI_CreateBooleanJson(TRI_CORE_MEM_ZONE, config->_logRemoteChanges));
  
  TRI_Insert3ArrayJson(TRI_CORE_MEM_ZONE,
                       json,
                       "maxEvents",
                       TRI_CreateNumberJson(TRI_CORE_MEM_ZONE, (double) config->_maxEvents));
  
  TRI_Insert3ArrayJson(TRI_CORE_MEM_ZONE,
                       json,
                       "maxEventsSize",
                       TRI_CreateNumberJson(TRI_CORE_MEM_ZONE, (double) config->_maxEventsSize));

  return json;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief configure the replication logger
////////////////////////////////////////////////////////////////////////////////

int TRI_ConfigureReplicationLogger (TRI_replication_logger_t* logger,
                                    TRI_replication_logger_configuration_t const* config) {
  uint64_t oldMaxEvents;
  uint64_t oldMaxEventsSize;
  char* filename;
  int res;
  
  if (logger->_vocbase->_type == TRI_VOCBASE_TYPE_COORDINATOR) {
    return TRI_ERROR_CLUSTER_UNSUPPORTED;
  }

  res = TRI_ERROR_NO_ERROR;

  if (config->_maxEventsSize != 0 || config->_maxEvents != 0) {
    if (config->_maxEvents > 0 && config->_maxEvents < TRI_REPLICATION_LOGGER_EVENTS_MIN) {
      return TRI_ERROR_REPLICATION_INVALID_LOGGER_CONFIGURATION;
    }
  
    if (config->_maxEventsSize > 0 && config->_maxEventsSize < TRI_REPLICATION_LOGGER_SIZE_MIN) {
      return TRI_ERROR_REPLICATION_INVALID_LOGGER_CONFIGURATION;
    }
  }

  // valid

  TRI_WriteLockReadWriteLock(&logger->_statusLock);
  oldMaxEvents     = logger->_configuration._maxEvents;
  oldMaxEventsSize = logger->_configuration._maxEventsSize;

  if (config->_maxEvents != oldMaxEvents ||
      config->_maxEventsSize != oldMaxEventsSize) {
    // configuration change. free existing cap
    if (logger->_state._active) {
      FreeCap(logger);
    }

    // set new limits and re-create cap if necessary
    logger->_configuration._maxEvents     = config->_maxEvents;
    logger->_configuration._maxEventsSize = config->_maxEventsSize;

    TRI_ASSERT(logger->_cap == NULL);

    if (logger->_state._active) {
      CreateCap(logger);
    }
  }

  logger->_configuration._logRemoteChanges = config->_logRemoteChanges;
  logger->_configuration._autoStart = config->_autoStart;

  // now save configuration to file
  filename = GetConfigurationFilename(logger->_vocbase);

  if (filename != NULL) {
    TRI_json_t* json = TRI_JsonConfigurationReplicationLogger(&logger->_configuration);

    if (json != NULL) {
      TRI_SaveJson(filename, json, true); 
      TRI_FreeJson(TRI_CORE_MEM_ZONE, json);
    }

    TRI_Free(TRI_CORE_MEM_ZONE, filename);
  }

  TRI_WriteUnlockReadWriteLock(&logger->_statusLock);

  return res;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief copy a logger configuration
////////////////////////////////////////////////////////////////////////////////

void TRI_CopyConfigurationReplicationLogger (TRI_replication_logger_configuration_t const* src,
                                             TRI_replication_logger_configuration_t* dst) {
  memcpy(dst, src, sizeof(TRI_replication_logger_configuration_t));
}

////////////////////////////////////////////////////////////////////////////////
/// @brief return the list of clients as a JSON array
////////////////////////////////////////////////////////////////////////////////

TRI_json_t* TRI_JsonClientsReplicationLogger (TRI_replication_logger_t* logger) {
  TRI_json_t* json;
  uint32_t i, n;

  json = TRI_CreateListJson(TRI_CORE_MEM_ZONE);

  if (json == NULL) {
    return NULL;
  }

  TRI_ReadLockReadWriteLock(&logger->_clientsLock);

  n = logger->_clients._nrAlloc;

  for (i = 0; i < n; ++i) {
    logger_client_t* client = static_cast<logger_client_t*>(logger->_clients._table[i]);

    if (client != NULL) {
      TRI_json_t* element = TRI_CreateArrayJson(TRI_CORE_MEM_ZONE);

      if (element != NULL) {
        char* value;
        
        value = TRI_StringUInt64(client->_serverId);

        if (value != NULL) {
          TRI_Insert3ArrayJson(TRI_CORE_MEM_ZONE, element, "serverId", TRI_CreateStringJson(TRI_CORE_MEM_ZONE, value));
        }
        
        value = TRI_StringUInt64(client->_lastServedTick);

        if (value != NULL) {
          TRI_Insert3ArrayJson(TRI_CORE_MEM_ZONE, element, "lastServedTick", TRI_CreateStringJson(TRI_CORE_MEM_ZONE, value));
        }

        TRI_Insert3ArrayJson(TRI_CORE_MEM_ZONE, element, "time", TRI_CreateStringCopyJson(TRI_CORE_MEM_ZONE, client->_stamp));

        TRI_PushBack3ListJson(TRI_CORE_MEM_ZONE, json, element);
      }
    }
  }

  TRI_ReadUnlockReadWriteLock(&logger->_clientsLock);

  return json;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief insert an applier action into an action list
////////////////////////////////////////////////////////////////////////////////

void TRI_UpdateClientReplicationLogger (TRI_replication_logger_t* logger,
                                        TRI_server_id_t serverId,
                                        TRI_voc_tick_t lastServedTick) {

  logger_client_t* client = static_cast<logger_client_t*>(TRI_Allocate(TRI_UNKNOWN_MEM_ZONE, sizeof(logger_client_t), false));

  if (client == NULL) {
    return;
  }

  TRI_GetTimeStampReplication(client->_stamp, sizeof(client->_stamp) - 1);

  client->_serverId       = serverId;
  client->_lastServedTick = lastServedTick;

  TRI_WriteLockReadWriteLock(&logger->_clientsLock);

  void* found = TRI_RemoveKeyAssociativePointer(&logger->_clients, &client->_serverId);

  if (found != NULL) {
    FreeClient((logger_client_t*) found); 
  }

  found = TRI_InsertKeyAssociativePointer(&logger->_clients, &client->_serverId, (void*) client, false);
  TRI_ASSERT(found == NULL);

  TRI_WriteUnlockReadWriteLock(&logger->_clientsLock);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief start the replication logger
////////////////////////////////////////////////////////////////////////////////

int TRI_StartReplicationLogger (TRI_replication_logger_t* logger) {
  int res;
  
  if (logger->_vocbase->_type == TRI_VOCBASE_TYPE_COORDINATOR) {
    return TRI_ERROR_CLUSTER_UNSUPPORTED;
  }
  
  res = TRI_ERROR_NO_ERROR;

  TRI_WriteLockReadWriteLock(&logger->_statusLock);

  if (! logger->_state._active) {
    res = StartReplicationLogger(logger);
  }

  TRI_WriteUnlockReadWriteLock(&logger->_statusLock);

  return res;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief stop the replication logger
////////////////////////////////////////////////////////////////////////////////

int TRI_StopReplicationLogger (TRI_replication_logger_t* logger) {
  int res;
  
  if (logger->_vocbase->_type == TRI_VOCBASE_TYPE_COORDINATOR) {
    return TRI_ERROR_CLUSTER_UNSUPPORTED;
  }

  res = TRI_ERROR_NO_ERROR;
  
  TRI_WriteLockReadWriteLock(&logger->_statusLock);

  if (logger->_state._active) {
    res = StopReplicationLogger(logger);
  }

  TRI_WriteUnlockReadWriteLock(&logger->_statusLock);

  return res;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief get the current replication logger state
////////////////////////////////////////////////////////////////////////////////

int TRI_StateReplicationLogger (TRI_replication_logger_t* logger,
                                TRI_replication_logger_state_t* state) {
  int res;

  TRI_ReadLockReadWriteLock(&logger->_statusLock);

  if (logger->_state._active) {
    // use state from logger
    res = GetStateReplicationLogger(logger, state);
  }
  else {
    // read first/last directly from collection
    res = GetStateInactive(logger, state);
  }

  TRI_ReadUnlockReadWriteLock(&logger->_statusLock);

  return res;
} 

////////////////////////////////////////////////////////////////////////////////
/// @brief get a JSON representation of a logger state
////////////////////////////////////////////////////////////////////////////////
  
TRI_json_t* TRI_JsonStateReplicationLogger (TRI_replication_logger_state_t const* state) {
  TRI_json_t* json; 
  char* value;
  char timeString[24];

  json = TRI_CreateArray2Json(TRI_CORE_MEM_ZONE, 3);

  if (json == NULL) {
    return NULL;
  }

  // add replication state
  TRI_Insert3ArrayJson(TRI_CORE_MEM_ZONE, json, "running", TRI_CreateBooleanJson(TRI_CORE_MEM_ZONE, state->_active));
  
  value = TRI_StringUInt64(state->_lastLogTick);
  TRI_Insert3ArrayJson(TRI_CORE_MEM_ZONE, json, "lastLogTick", TRI_CreateStringJson(TRI_CORE_MEM_ZONE, value));
  
  TRI_Insert3ArrayJson(TRI_CORE_MEM_ZONE, json, "totalEvents", TRI_CreateNumberJson(TRI_CORE_MEM_ZONE, (double) state->_totalEvents));

  TRI_GetTimeStampReplication(timeString, sizeof(timeString) - 1);
  TRI_Insert3ArrayJson(TRI_CORE_MEM_ZONE, json, "time", TRI_CreateStringCopyJson(TRI_CORE_MEM_ZONE, timeString));
  
  return json;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief return a JSON representation of the replication logger
////////////////////////////////////////////////////////////////////////////////

TRI_json_t* TRI_JsonReplicationLogger (TRI_replication_logger_t* logger) {
  TRI_replication_logger_state_t state;
  TRI_json_t* json;
  TRI_json_t* server;
  TRI_json_t* clients;
  int res;
  
  res = TRI_StateReplicationLogger(logger, &state);
  
  if (res != TRI_ERROR_NO_ERROR) {
    return NULL;
  }

  json = TRI_CreateArrayJson(TRI_CORE_MEM_ZONE);

  if (json == NULL) {
    return NULL;
  }
    
  TRI_Insert3ArrayJson(TRI_CORE_MEM_ZONE, json, "state", TRI_JsonStateReplicationLogger(&state)); 
    
  // add server info
  server = TRI_CreateArrayJson(TRI_CORE_MEM_ZONE);

  if (server != NULL) {
    TRI_server_id_t serverId;

    TRI_Insert3ArrayJson(TRI_CORE_MEM_ZONE, server, "version", TRI_CreateStringCopyJson(TRI_CORE_MEM_ZONE, TRI_VERSION));

    serverId = TRI_GetIdServer();  
    TRI_Insert3ArrayJson(TRI_CORE_MEM_ZONE, server, "serverId", TRI_CreateStringJson(TRI_CORE_MEM_ZONE, TRI_StringUInt64(serverId)));

    TRI_Insert3ArrayJson(TRI_CORE_MEM_ZONE, json, "server", server);
  }
    
  clients = TRI_JsonClientsReplicationLogger(logger);

  if (clients != NULL) {
    TRI_Insert3ArrayJson(TRI_CORE_MEM_ZONE, json, "clients", clients);
  }

  return json;
}

// Local Variables:
// mode: outline-minor
// outline-regexp: "/// @brief\\|/// {@inheritDoc}\\|/// @addtogroup\\|/// @page\\|// --SECTION--\\|/// @\\}"
// End:
