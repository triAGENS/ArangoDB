////////////////////////////////////////////////////////////////////////////////
/// @brief associative array implementation
///
/// @file
///
/// DISCLAIMER
///
/// Copyright 2004-2012 triagens GmbH, Cologne, Germany
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
/// @author Dr. Frank Celler
/// @author Martin Schoenert
/// @author Copyright 2006-2012, triAGENS GmbH, Cologne, Germany
////////////////////////////////////////////////////////////////////////////////

#ifndef TRIAGENS_HASHINDEX_HASH_ARRAY_H
#define TRIAGENS_HASHINDEX_HASH_ARRAY_H 1

#include "BasicsC/common.h"
#include <BasicsC/vector.h>

#ifdef __cplusplus
extern "C" {
#endif

// -----------------------------------------------------------------------------
// --SECTION--                                                      public types
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @addtogroup Collections
/// @{
////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
/// @brief associative array
////////////////////////////////////////////////////////////////////////////////

typedef struct TRI_hasharray_s {
  uint64_t (*hashKey) (struct TRI_hasharray_s*, void*);
  uint64_t (*hashElement) (struct TRI_hasharray_s*, void*);

  void (*clearElement) (struct TRI_hasharray_s*, void*);

  bool (*isEmptyElement) (struct TRI_hasharray_s*, void*);
  bool (*isEqualKeyElement) (struct TRI_hasharray_s*, void*, void*);
  bool (*isEqualElementElement) (struct TRI_hasharray_s*, void*, void*);

  size_t _numFields; // the number of fields indexes
  uint64_t _elementSize;
  uint64_t _nrAlloc; // the size of the table
  uint64_t _nrUsed; // the number of used entries
  
  // _table might or might not be the same pointer as _data
  // if you want to handle the hash table memory, always use the _data pointer!
  // if you want to work with the hash table elements, always use the _table pointer!

  char* _data; // pointer to memory acquired for the hash table
  char* _table; // the table itself, aligned to a cache line boundary 

#ifdef TRI_INTERNAL_STATS
  uint64_t _nrFinds; // statistics: number of lookup calls
  uint64_t _nrAdds; // statistics: number of insert calls
  uint64_t _nrRems; // statistics: number of remove calls
  uint64_t _nrResizes; // statistics: number of resizes

  uint64_t _nrProbesF; // statistics: number of misses while looking up
  uint64_t _nrProbesA; // statistics: number of misses while inserting
  uint64_t _nrProbesD; // statistics: number of misses while removing
  uint64_t _nrProbesR; // statistics: number of misses while adding
#endif
}
TRI_hasharray_t;


////////////////////////////////////////////////////////////////////////////////
/// @}
////////////////////////////////////////////////////////////////////////////////

// -----------------------------------------------------------------------------
// --SECTION--                                                 ASSOCIATIVE ARRAY
// -----------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// --SECTION--                                      constructors and destructors
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @addtogroup Collections
/// @{
////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
/// @brief initialises an array
////////////////////////////////////////////////////////////////////////////////

bool TRI_InitHashArray (TRI_hasharray_t*,
                        size_t initialDocumentCount,
                        size_t numFields,
                        size_t elementSize,
                        uint64_t (*hashKey) (TRI_hasharray_t*, void*),
                        uint64_t (*hashElement) (TRI_hasharray_t*, void*),
                        void (*clearElement) (TRI_hasharray_t*, void*),
                        bool (*isEmptyElement) (TRI_hasharray_t*, void*),
                        bool (*isEqualKeyElement) (TRI_hasharray_t*, void*, void*),
                        bool (*isEqualElementElement) (TRI_hasharray_t*, void*, void*));

////////////////////////////////////////////////////////////////////////////////
/// @brief destroys an array, but does not free the pointer
////////////////////////////////////////////////////////////////////////////////

void TRI_DestroyHashArray (TRI_hasharray_t*);

////////////////////////////////////////////////////////////////////////////////
/// @brief destroys an array and frees the pointer
////////////////////////////////////////////////////////////////////////////////

void TRI_FreeHashArray (TRI_hasharray_t*);

////////////////////////////////////////////////////////////////////////////////
/// @}
////////////////////////////////////////////////////////////////////////////////

// -----------------------------------------------------------------------------
// --SECTION--                                                  public functions
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @addtogroup Collections
/// @{
////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
/// @brief lookups an element given a key
////////////////////////////////////////////////////////////////////////////////

void* TRI_LookupByKeyHashArray (TRI_hasharray_t*, void* key);

////////////////////////////////////////////////////////////////////////////////
/// @brief finds an element given a key, returns NULL if not found
////////////////////////////////////////////////////////////////////////////////

void* TRI_FindByKeyHashArray (TRI_hasharray_t*, void* key);

////////////////////////////////////////////////////////////////////////////////
/// @brief lookups an element given an element
////////////////////////////////////////////////////////////////////////////////

void* TRI_LookupByElementHashArray (TRI_hasharray_t*, void* element);

////////////////////////////////////////////////////////////////////////////////
/// @brief finds an element given an element, returns NULL if not found
////////////////////////////////////////////////////////////////////////////////

void* TRI_FindByElementHashArray (TRI_hasharray_t*, void* element);

////////////////////////////////////////////////////////////////////////////////
/// @brief adds an element to the array
////////////////////////////////////////////////////////////////////////////////

bool TRI_InsertElementHashArray (TRI_hasharray_t*, void* element, bool overwrite);

////////////////////////////////////////////////////////////////////////////////
/// @brief adds an key/element to the array
////////////////////////////////////////////////////////////////////////////////

bool TRI_InsertKeyHashArray (TRI_hasharray_t*, void* key, void* element, bool overwrite);

////////////////////////////////////////////////////////////////////////////////
/// @brief removes an element from the array
////////////////////////////////////////////////////////////////////////////////

bool TRI_RemoveElementHashArray (TRI_hasharray_t*, void* element);

////////////////////////////////////////////////////////////////////////////////
/// @brief removes an key/element to the array
////////////////////////////////////////////////////////////////////////////////

bool TRI_RemoveKeyHashArray (TRI_hasharray_t*, void* key);

////////////////////////////////////////////////////////////////////////////////
/// @}
////////////////////////////////////////////////////////////////////////////////

// -----------------------------------------------------------------------------
// --SECTION--                                                  MULTI HASH ARRAY
// -----------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// --SECTION--                                                  public functions
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @addtogroup Collections
/// @{
////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
/// @brief lookups an element given a key
////////////////////////////////////////////////////////////////////////////////

TRI_vector_pointer_t TRI_LookupByKeyHashArrayMulti (TRI_hasharray_t*, void* key);

////////////////////////////////////////////////////////////////////////////////
/// @brief lookups an element given an element
////////////////////////////////////////////////////////////////////////////////

TRI_vector_pointer_t TRI_LookupByElementHashArrayMulti (TRI_hasharray_t*, void* element);

////////////////////////////////////////////////////////////////////////////////
/// @brief adds an element to the array
////////////////////////////////////////////////////////////////////////////////

bool TRI_InsertElementHashArrayMulti (TRI_hasharray_t*, void* element, bool overwrite);

////////////////////////////////////////////////////////////////////////////////
/// @brief adds an key/element to the array
////////////////////////////////////////////////////////////////////////////////

bool TRI_InsertKeyHashArrayMulti (TRI_hasharray_t*, void* key, void* element, bool overwrite);

////////////////////////////////////////////////////////////////////////////////
/// @brief removes an element from the array
////////////////////////////////////////////////////////////////////////////////

bool TRI_RemoveElementHashArrayMulti (TRI_hasharray_t*, void* element);

////////////////////////////////////////////////////////////////////////////////
/// @brief removes an key/element to the array
////////////////////////////////////////////////////////////////////////////////

bool TRI_RemoveKeyHashArrayMulti (TRI_hasharray_t*, void* key);

////////////////////////////////////////////////////////////////////////////////
/// @}
////////////////////////////////////////////////////////////////////////////////

#ifdef __cplusplus
}
#endif

#endif

// Local Variables:
// mode: outline-minor
// outline-regexp: "^\\(/// @brief\\|/// {@inheritDoc}\\|/// @addtogroup\\|// --SECTION--\\|/// @\\}\\)"
// End:
