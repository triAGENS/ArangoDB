////////////////////////////////////////////////////////////////////////////////
/// @brief transaction subsystem
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
/// @author Copyright 2012-2013, triAGENS GmbH, Cologne, Germany
////////////////////////////////////////////////////////////////////////////////

#ifndef TRIAGENS_VOC_BASE_TRANSACTION_H
#define TRIAGENS_VOC_BASE_TRANSACTION_H 1

#include "BasicsC/common.h"

#include "BasicsC/associative.h"
#include "BasicsC/hashes.h"
#include "BasicsC/locks.h"
#include "BasicsC/vector.h"

#include "VocBase/datafile.h"
#include "VocBase/voc-types.h"

#ifdef __cplusplus
extern "C" {
#endif

// -----------------------------------------------------------------------------
// --SECTION--                                              forward declarations
// -----------------------------------------------------------------------------

struct TRI_doc_mptr_s;
struct TRI_vocbase_s;
struct TRI_vocbase_col_s;

// -----------------------------------------------------------------------------
// --SECTION--                                                 TRANSACTION TYPES
// -----------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// --SECTION--                                                    public defines
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @addtogroup VocBase
/// @{
////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
/// @brief top level of a transaction
////////////////////////////////////////////////////////////////////////////////

#define TRI_TRANSACTION_TOP_LEVEL 0

////////////////////////////////////////////////////////////////////////////////
/// @brief time (in µs) that is spent waiting for a lock
////////////////////////////////////////////////////////////////////////////////

#define TRI_TRANSACTION_DEFAULT_LOCK_TIMEOUT 30000000ULL

////////////////////////////////////////////////////////////////////////////////
/// @brief sleep time (in µs) while waiting for lock acquisition
////////////////////////////////////////////////////////////////////////////////

#define TRI_TRANSACTION_DEFAULT_SLEEP_DURATION 10000ULL

////////////////////////////////////////////////////////////////////////////////
/// @}
////////////////////////////////////////////////////////////////////////////////

// -----------------------------------------------------------------------------
// --SECTION--                                                      public types
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @addtogroup VocBase
/// @{
////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
/// @brief transaction type
////////////////////////////////////////////////////////////////////////////////

typedef enum {
  TRI_TRANSACTION_READ  = 1,
  TRI_TRANSACTION_WRITE = 2
}
TRI_transaction_type_e;

////////////////////////////////////////////////////////////////////////////////
/// @brief transaction statuses
////////////////////////////////////////////////////////////////////////////////

typedef enum {
  TRI_TRANSACTION_UNDEFINED    = 0,
  TRI_TRANSACTION_CREATED      = 1,
  TRI_TRANSACTION_RUNNING      = 2,
  TRI_TRANSACTION_COMMITTED    = 3,
  TRI_TRANSACTION_ABORTED      = 4,
  TRI_TRANSACTION_FAILED       = 5
}
TRI_transaction_status_e;

////////////////////////////////////////////////////////////////////////////////
/// @}
////////////////////////////////////////////////////////////////////////////////

// -----------------------------------------------------------------------------
// --SECTION--                                                  TRANSACTION LIST
// -----------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// --SECTION--                                                      public types
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @addtogroup VocBase
/// @{
////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
/// @}
////////////////////////////////////////////////////////////////////////////////

// -----------------------------------------------------------------------------
// --SECTION--                                               TRANSACTION CONTEXT
// -----------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// --SECTION--                                                      public types
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @addtogroup VocBase
/// @{
////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
/// @brief global transaction context typedef
////////////////////////////////////////////////////////////////////////////////

typedef struct TRI_transaction_context_s {
#if 0  
  TRI_read_write_lock_t     _collectionsLock;
  TRI_associative_pointer_t _collections;
#endif

  struct TRI_vocbase_s*     _vocbase;     
}
TRI_transaction_context_t;



typedef struct TRI_transaction_global_stats_s {
  TRI_voc_tid_t  _lastStartedReader;
  TRI_voc_tid_t  _lastFinishedReader;

  TRI_voc_tid_t  _lastStartedWriter;
  TRI_voc_tid_t  _lastAbortedWriter;
  TRI_voc_tid_t  _lastFinishedWriter;
}
TRI_transaction_global_stats_t;

////////////////////////////////////////////////////////////////////////////////
/// @brief get the stats for a global instance of a collection
////////////////////////////////////////////////////////////////////////////////

#if 0

typedef struct TRI_transaction_collection_stats_s {
  TRI_voc_tid_t  _lastStartedReader;
  TRI_voc_tid_t  _lastFinishedReader;

  TRI_voc_tid_t  _lastStartedWriter;
  TRI_voc_tid_t  _lastAbortedWriter;
  TRI_voc_tid_t  _lastFinishedWriter;
}
TRI_transaction_collection_stats_t;

#endif

////////////////////////////////////////////////////////////////////////////////
/// @brief global instance of a collection
////////////////////////////////////////////////////////////////////////////////

#if 0

typedef struct TRI_transaction_collection_global_s {
  TRI_voc_cid_t         _cid;
  TRI_read_write_lock_t _lock;

  TRI_transaction_collection_stats_t _stats;
}
TRI_transaction_collection_global_t;

#endif

////////////////////////////////////////////////////////////////////////////////
/// @}
////////////////////////////////////////////////////////////////////////////////

// -----------------------------------------------------------------------------
// --SECTION--                                        constructors / destructors
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @addtogroup VocBase
/// @{
////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
/// @brief create the global transaction context
////////////////////////////////////////////////////////////////////////////////

TRI_transaction_context_t* TRI_CreateTransactionContext (struct TRI_vocbase_s* const);

////////////////////////////////////////////////////////////////////////////////
/// @brief free the global transaction context
////////////////////////////////////////////////////////////////////////////////

void TRI_FreeTransactionContext (TRI_transaction_context_t* const);

////////////////////////////////////////////////////////////////////////////////
/// @}
////////////////////////////////////////////////////////////////////////////////

// -----------------------------------------------------------------------------
// --SECTION--                                                  public functions
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @addtogroup VocBase
/// @{
////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
/// @brief free all data associated with a specific collection
/// this function must be called for all collections that are dropped
////////////////////////////////////////////////////////////////////////////////

void TRI_RemoveCollectionTransactionContext (TRI_transaction_context_t*,
                                             const TRI_voc_cid_t);

////////////////////////////////////////////////////////////////////////////////
/// @brief populates a struct with transaction statistics for a collections
////////////////////////////////////////////////////////////////////////////////

#if 0

int TRI_StatsCollectionTransactionContext (TRI_transaction_context_t*,
                                           const TRI_voc_cid_t,
                                           TRI_transaction_collection_stats_t*);

#endif

////////////////////////////////////////////////////////////////////////////////
/// @}
////////////////////////////////////////////////////////////////////////////////

// -----------------------------------------------------------------------------
// --SECTION--                                                       TRANSACTION
// -----------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// --SECTION--                                                      public types
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @addtogroup VocBase
/// @{
////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
/// @brief typedef for transaction hints
////////////////////////////////////////////////////////////////////////////////

typedef uint32_t TRI_transaction_hint_t;

////////////////////////////////////////////////////////////////////////////////
/// @brief hints that can be used for transactions
////////////////////////////////////////////////////////////////////////////////

typedef enum {
  TRI_TRANSACTION_HINT_NONE              = 0,
  TRI_TRANSACTION_HINT_SINGLE_OPERATION  = 1,
  TRI_TRANSACTION_HINT_LOCK_ENTIRELY     = 2,
  TRI_TRANSACTION_HINT_LOCK_NEVER        = 4,
  TRI_TRANSACTION_HINT_READ_ONLY         = 8,
  TRI_TRANSACTION_HINT_SINGLE_COLLECTION = 16
}
TRI_transaction_hint_e;

////////////////////////////////////////////////////////////////////////////////
/// @brief transaction typedef
////////////////////////////////////////////////////////////////////////////////

typedef struct TRI_transaction_s {
  TRI_transaction_context_t* _context;        // global context object
  TRI_voc_tid_t              _id;
  TRI_transaction_type_e     _type;           // access type (read|write)
  TRI_transaction_status_e   _status;         // current status
  TRI_vector_pointer_t       _collections;    // list of participating collections
  TRI_transaction_hint_t     _hints;          // hints;
  int                        _nestingLevel;
  uint64_t                   _timeout;        // timeout for lock acquisition
  bool                       _hasOperations;  // whether or not there are write operations in the trx
  bool                       _replicate;      // replicate this transaction?
  bool                       _waitForSync;    // whether or not the collection had a synchronous op
}
TRI_transaction_t;

////////////////////////////////////////////////////////////////////////////////
/// @brief collection used in a transaction
////////////////////////////////////////////////////////////////////////////////

typedef struct TRI_transaction_collection_s {
  TRI_transaction_t*                   _transaction;       // the transaction
  TRI_voc_cid_t                        _cid;               // collection id
  TRI_transaction_type_e               _accessType;        // access type (read|write)
  int                                  _nestingLevel;      // the transaction level that added this collection
  struct TRI_vocbase_col_s*            _collection;        // vocbase collection pointer
#if 0
  TRI_transaction_collection_global_t* _globalInstance;    // pointer to the global instance
#endif
  TRI_vector_t*                        _operations;        // buffered CRUD operations
  TRI_voc_tick_t                       _originalRevision;  // collection revision at trx start
  bool                                 _locked;            // collection lock flag
  bool                                 _compactionLocked;  // was the compaction lock grabbed for the collection?
  bool                                 _waitForSync;       // whether or not the collection has waitForSync
}
TRI_transaction_collection_t;

////////////////////////////////////////////////////////////////////////////////
/// @}
////////////////////////////////////////////////////////////////////////////////

// -----------------------------------------------------------------------------
// --SECTION--                                        constructors / destructors
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @addtogroup VocBase
/// @{
////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
/// @brief create a new transaction container
////////////////////////////////////////////////////////////////////////////////

TRI_transaction_t* TRI_CreateTransaction (TRI_transaction_context_t* const,
                                          bool,
                                          double, 
                                          bool);

////////////////////////////////////////////////////////////////////////////////
/// @brief free a transaction container
////////////////////////////////////////////////////////////////////////////////

void TRI_FreeTransaction (TRI_transaction_t* const);

////////////////////////////////////////////////////////////////////////////////
/// @}
////////////////////////////////////////////////////////////////////////////////

// -----------------------------------------------------------------------------
// --SECTION--                                                  public functions
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @addtogroup VocBase
/// @{
////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
/// @brief increase the number of writes done for a collection
////////////////////////////////////////////////////////////////////////////////

void TRI_IncreaseWritesCollectionTransaction (TRI_transaction_collection_t*,
                                              bool);

////////////////////////////////////////////////////////////////////////////////
/// @brief return the collection from a transaction
////////////////////////////////////////////////////////////////////////////////

bool TRI_WasSynchronousCollectionTransaction (TRI_transaction_t const*,
                                              const TRI_voc_cid_t);

////////////////////////////////////////////////////////////////////////////////
/// @brief return the collection from a transaction
////////////////////////////////////////////////////////////////////////////////

TRI_transaction_collection_t* TRI_GetCollectionTransaction (TRI_transaction_t const*,
                                                            const TRI_voc_cid_t,
                                                            const TRI_transaction_type_e);

////////////////////////////////////////////////////////////////////////////////
/// @brief add a collection to a transaction
////////////////////////////////////////////////////////////////////////////////

int TRI_AddCollectionTransaction (TRI_transaction_t* const,
                                  const TRI_voc_cid_t,
                                  const TRI_transaction_type_e, 
                                  const int);

////////////////////////////////////////////////////////////////////////////////
/// @brief request a lock for a collection
////////////////////////////////////////////////////////////////////////////////

int TRI_LockCollectionTransaction (TRI_transaction_collection_t*,
                                   const TRI_transaction_type_e,
                                   const int);

////////////////////////////////////////////////////////////////////////////////
/// @brief request an unlock for a collection
////////////////////////////////////////////////////////////////////////////////

int TRI_UnlockCollectionTransaction (TRI_transaction_collection_t*,
                                     const TRI_transaction_type_e, 
                                     const int);

////////////////////////////////////////////////////////////////////////////////
/// @brief check whether a collection is locked in a transaction
////////////////////////////////////////////////////////////////////////////////

bool TRI_IsLockedCollectionTransaction (TRI_transaction_collection_t*,
                                        const TRI_transaction_type_e,
                                        const int);

////////////////////////////////////////////////////////////////////////////////
/// @brief as the id of a failed transaction to a vector
////////////////////////////////////////////////////////////////////////////////

int TRI_AddIdFailedTransaction (TRI_vector_t*,
                                TRI_voc_tid_t);

////////////////////////////////////////////////////////////////////////////////
/// @brief add a marker to a transaction collection
////////////////////////////////////////////////////////////////////////////////

int TRI_AddOperationCollectionTransaction (TRI_transaction_collection_t*,
                                           TRI_voc_document_operation_e,
                                           struct TRI_doc_mptr_s*,
                                           struct TRI_doc_mptr_s*,
                                           struct TRI_doc_mptr_s*,
                                           TRI_df_marker_t*,
                                           TRI_voc_size_t,
                                           TRI_voc_rid_t,
                                           bool,
                                           bool*);

////////////////////////////////////////////////////////////////////////////////
/// @brief get a transaction's id
////////////////////////////////////////////////////////////////////////////////

TRI_voc_tid_t TRI_GetIdTransaction (const TRI_transaction_t*);

////////////////////////////////////////////////////////////////////////////////
/// @brief get a transaction's id for writing into a marker
/// this will return 0 if the operation is standalone
////////////////////////////////////////////////////////////////////////////////

TRI_voc_tid_t TRI_GetMarkerIdTransaction (const TRI_transaction_t*);

////////////////////////////////////////////////////////////////////////////////
/// @brief begin a transaction
////////////////////////////////////////////////////////////////////////////////

int TRI_BeginTransaction (TRI_transaction_t* const, 
                          TRI_transaction_hint_t,
                          const int);

////////////////////////////////////////////////////////////////////////////////
/// @brief commit a transaction
////////////////////////////////////////////////////////////////////////////////

int TRI_CommitTransaction (TRI_transaction_t* const,
                           const int);

////////////////////////////////////////////////////////////////////////////////
/// @brief abort a transaction
////////////////////////////////////////////////////////////////////////////////

int TRI_AbortTransaction (TRI_transaction_t* const,
                          const int);

////////////////////////////////////////////////////////////////////////////////
/// @}
////////////////////////////////////////////////////////////////////////////////

// -----------------------------------------------------------------------------
// --SECTION--                                               TRANSACTION HELPERS
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @addtogroup VocBase
/// @{
////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
/// @brief execute a single operation wrapped in a transaction
/// the actual operation can be specified using a callback function
////////////////////////////////////////////////////////////////////////////////

int TRI_ExecuteSingleOperationTransaction (struct TRI_vocbase_s*,
                                           const char*,
                                           TRI_transaction_type_e,
                                           int (*callback)(TRI_transaction_collection_t*, void*),
                                           void*,
                                           bool);

////////////////////////////////////////////////////////////////////////////////
/// @}
////////////////////////////////////////////////////////////////////////////////

// -----------------------------------------------------------------------------
// --SECTION--                                               TRANSACTION MARKERS
// -----------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// --SECTION--                                                      public types
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @addtogroup VocBase
/// @{
////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
/// @brief begin transaction marker
////////////////////////////////////////////////////////////////////////////////

typedef struct TRI_doc_begin_transaction_marker_s {
  TRI_df_marker_t base;

  TRI_voc_tid_t   _tid;
  uint32_t        _numCollections;
#ifdef TRI_PADDING_32
  char            _padding_begin_marker[4];
#endif
}
TRI_doc_begin_transaction_marker_t;

////////////////////////////////////////////////////////////////////////////////
/// @brief commit transaction marker
////////////////////////////////////////////////////////////////////////////////

typedef struct TRI_doc_commit_transaction_marker_s {
  TRI_df_marker_t base;

  TRI_voc_tid_t   _tid;
}
TRI_doc_commit_transaction_marker_t;

////////////////////////////////////////////////////////////////////////////////
/// @brief abort transaction marker
////////////////////////////////////////////////////////////////////////////////

typedef struct TRI_doc_abort_transaction_marker_s {
  TRI_df_marker_t base;

  TRI_voc_tid_t   _tid;
}
TRI_doc_abort_transaction_marker_t;

////////////////////////////////////////////////////////////////////////////////
/// @brief prepare transaction marker
////////////////////////////////////////////////////////////////////////////////

typedef struct TRI_doc_prepare_transaction_marker_s {
  TRI_df_marker_t base;

  TRI_voc_tid_t   _tid;
}
TRI_doc_prepare_transaction_marker_t;

////////////////////////////////////////////////////////////////////////////////
/// @}
////////////////////////////////////////////////////////////////////////////////

// -----------------------------------------------------------------------------
// --SECTION--                                                  public functions
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @addtogroup VocBase
/// @{
////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
/// @brief create a "begin" marker
////////////////////////////////////////////////////////////////////////////////

int TRI_CreateMarkerBeginTransaction (TRI_transaction_t*,
                                      struct TRI_doc_begin_transaction_marker_s**,
                                      uint32_t);

////////////////////////////////////////////////////////////////////////////////
/// @brief create a "commit" marker
////////////////////////////////////////////////////////////////////////////////

int TRI_CreateMarkerCommitTransaction (TRI_transaction_t*,
                                       struct TRI_doc_commit_transaction_marker_s**);

////////////////////////////////////////////////////////////////////////////////
/// @brief create an "abort" marker
////////////////////////////////////////////////////////////////////////////////

int TRI_CreateMarkerAbortTransaction (TRI_transaction_t*,
                                      struct TRI_doc_abort_transaction_marker_s**);

////////////////////////////////////////////////////////////////////////////////
/// @brief create a "prepare" marker
////////////////////////////////////////////////////////////////////////////////

int TRI_CreateMarkerPrepareTransaction (TRI_transaction_t*,
                                        struct TRI_doc_prepare_transaction_marker_s**);

                                        
////////////////////////////////////////////////////////////////////////////////
/// @brief returns one or more figures associated with transactions
////////////////////////////////////////////////////////////////////////////////
                          
int TRI_GetGlobalTransactionFigures (TRI_transaction_global_stats_t*); 
                          
////////////////////////////////////////////////////////////////////////////////
/// @}
////////////////////////////////////////////////////////////////////////////////

#ifdef __cplusplus
}
#endif

#endif

// Local Variables:
// mode: outline-minor
// outline-regexp: "/// @brief\\|/// {@inheritDoc}\\|/// @addtogroup\\|/// @page\\|// --SECTION--\\|/// @\\}"
