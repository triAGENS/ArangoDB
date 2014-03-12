////////////////////////////////////////////////////////////////////////////////
/// @brief vector implementation
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
/// @author Dr. Frank Celler
/// @author Copyright 2011-2013, triAGENS GmbH, Cologne, Germany
////////////////////////////////////////////////////////////////////////////////

#ifndef TRIAGENS_BASICS_C_VECTOR_H
#define TRIAGENS_BASICS_C_VECTOR_H 1

#include "BasicsC/common.h"

#ifdef __cplusplus
extern "C" {
#endif

// -----------------------------------------------------------------------------
// --SECTION--                                                       POD VECTORS
// -----------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// --SECTION--                                                      public types
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @addtogroup Collections
/// @{
////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
/// @brief pod vector
////////////////////////////////////////////////////////////////////////////////

typedef struct TRI_vector_s {
  TRI_memory_zone_t* _memoryZone;
  size_t _elementSize;
  char * _buffer;
  size_t _length;
  size_t _capacity;
}
TRI_vector_t;

////////////////////////////////////////////////////////////////////////////////
/// @}
////////////////////////////////////////////////////////////////////////////////

// -----------------------------------------------------------------------------
// --SECTION--                                      constructors and destructors
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @addtogroup Collections
/// @{
////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
/// @brief initialises a vector
////////////////////////////////////////////////////////////////////////////////

void TRI_InitVector (TRI_vector_t*, TRI_memory_zone_t*, size_t elementSize);

////////////////////////////////////////////////////////////////////////////////
/// @brief initialises a vector, with user-definable settings
////////////////////////////////////////////////////////////////////////////////

int TRI_InitVector2 (TRI_vector_t*,
                     TRI_memory_zone_t*,
                     size_t elementSize,
                     size_t initialCapacity);

////////////////////////////////////////////////////////////////////////////////
/// @brief destroys a vector, but does not free the pointer
////////////////////////////////////////////////////////////////////////////////

void TRI_DestroyVector (TRI_vector_t*);

////////////////////////////////////////////////////////////////////////////////
/// @brief destroys a vector and frees the pointer
////////////////////////////////////////////////////////////////////////////////

void TRI_FreeVector (TRI_memory_zone_t* zone, TRI_vector_t*);

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
/// @brief ensures a vector has space for extraCapacity more items
////////////////////////////////////////////////////////////////////////////////

int TRI_ReserveVector (TRI_vector_t*, 
                       size_t);

////////////////////////////////////////////////////////////////////////////////
/// @brief copies a vector
////////////////////////////////////////////////////////////////////////////////

TRI_vector_t* TRI_CopyVector (TRI_memory_zone_t*, 
                              TRI_vector_t const*);

////////////////////////////////////////////////////////////////////////////////
/// @brief copy data from one vector into another
////////////////////////////////////////////////////////////////////////////////

int TRI_CopyDataVector (TRI_vector_t*, 
                        TRI_vector_t const*);

////////////////////////////////////////////////////////////////////////////////
/// @brief returns true if the vector is empty
////////////////////////////////////////////////////////////////////////////////

bool TRI_EmptyVector (TRI_vector_t const*);

////////////////////////////////////////////////////////////////////////////////
/// @brief returns length of vector
////////////////////////////////////////////////////////////////////////////////

size_t TRI_LengthVector (TRI_vector_t const*);

////////////////////////////////////////////////////////////////////////////////
/// @brief clears the vector
////////////////////////////////////////////////////////////////////////////////

void TRI_ClearVector (TRI_vector_t*);

////////////////////////////////////////////////////////////////////////////////
/// @brief resizes the vector
////////////////////////////////////////////////////////////////////////////////

int TRI_ResizeVector (TRI_vector_t*, size_t);

////////////////////////////////////////////////////////////////////////////////
/// @brief adds and element at the end
////////////////////////////////////////////////////////////////////////////////

int TRI_PushBackVector (TRI_vector_t*, void const* element);

////////////////////////////////////////////////////////////////////////////////
/// @brief removes an element
////////////////////////////////////////////////////////////////////////////////

void TRI_RemoveVector (TRI_vector_t*, size_t n);

////////////////////////////////////////////////////////////////////////////////
/// @brief returns the element at a given position
////////////////////////////////////////////////////////////////////////////////

void* TRI_AtVector (TRI_vector_t const*, size_t);

////////////////////////////////////////////////////////////////////////////////
/// @brief inserts an element at a given position
////////////////////////////////////////////////////////////////////////////////

int TRI_InsertVector (TRI_vector_t* vector, void const* element, size_t position);

////////////////////////////////////////////////////////////////////////////////
/// @brief sets an element at a given position
////////////////////////////////////////////////////////////////////////////////

void TRI_SetVector (TRI_vector_t* vector, size_t, void const* element);

////////////////////////////////////////////////////////////////////////////////
/// @brief returns the beginning
////////////////////////////////////////////////////////////////////////////////

void* TRI_BeginVector (TRI_vector_t*);

////////////////////////////////////////////////////////////////////////////////
/// @brief returns the end (pointer after the last element)
////////////////////////////////////////////////////////////////////////////////

void* TRI_EndVector (TRI_vector_t*);

////////////////////////////////////////////////////////////////////////////////
/// @}
////////////////////////////////////////////////////////////////////////////////

// -----------------------------------------------------------------------------
// --SECTION--                                                   POINTER VECTORS
// -----------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// --SECTION--                                                      public types
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @addtogroup Collections
/// @{
////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
/// @brief pointer vector
////////////////////////////////////////////////////////////////////////////////

typedef struct TRI_vector_pointer_s {
  TRI_memory_zone_t* _memoryZone;
  void** _buffer;
  size_t _length;
  size_t _capacity;
}
TRI_vector_pointer_t;

////////////////////////////////////////////////////////////////////////////////
/// @}
////////////////////////////////////////////////////////////////////////////////

// -----------------------------------------------------------------------------
// --SECTION--                                      constructors and destructors
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @addtogroup Collections
/// @{
////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
/// @brief initialises a vector
////////////////////////////////////////////////////////////////////////////////

void TRI_InitVectorPointer (TRI_vector_pointer_t*, TRI_memory_zone_t*);

////////////////////////////////////////////////////////////////////////////////
/// @brief initialises a vector, with user-definable settings
////////////////////////////////////////////////////////////////////////////////

int TRI_InitVectorPointer2 (TRI_vector_pointer_t*,
                            TRI_memory_zone_t*,
                            size_t);

////////////////////////////////////////////////////////////////////////////////
/// @brief destroys a vector, but does not free the pointer
////////////////////////////////////////////////////////////////////////////////

void TRI_DestroyVectorPointer (TRI_vector_pointer_t*);

////////////////////////////////////////////////////////////////////////////////
/// @brief destroys a vector and frees the pointer
////////////////////////////////////////////////////////////////////////////////

void TRI_FreeVectorPointer (TRI_memory_zone_t*, TRI_vector_pointer_t*);

////////////////////////////////////////////////////////////////////////////////
/// @brief destroys a vector and frees the pointer and the content
////////////////////////////////////////////////////////////////////////////////

void TRI_FreeContentVectorPointer (TRI_memory_zone_t*,
                                   TRI_vector_pointer_t*);

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
/// @brief ensures a vector has space for more items
////////////////////////////////////////////////////////////////////////////////

int TRI_ReserveVectorPointer (TRI_vector_pointer_t*, 
                              size_t);

////////////////////////////////////////////////////////////////////////////////
/// @brief copies a vector
////////////////////////////////////////////////////////////////////////////////

TRI_vector_pointer_t* TRI_CopyVectorPointer (TRI_memory_zone_t*, 
                                             TRI_vector_pointer_t const*);

////////////////////////////////////////////////////////////////////////////////
/// @brief copies all pointers from a vector
////////////////////////////////////////////////////////////////////////////////

int TRI_CopyDataVectorPointer (TRI_vector_pointer_t*, 
                               TRI_vector_pointer_t const*);

////////////////////////////////////////////////////////////////////////////////
/// @brief returns true if the vector is empty
////////////////////////////////////////////////////////////////////////////////

bool TRI_EmptyVectorPointer (TRI_vector_pointer_t*);

////////////////////////////////////////////////////////////////////////////////
/// @brief returns length of vector
////////////////////////////////////////////////////////////////////////////////

size_t TRI_LengthVectorPointer (TRI_vector_pointer_t const*);

////////////////////////////////////////////////////////////////////////////////
/// @brief clears the vector
////////////////////////////////////////////////////////////////////////////////

void TRI_ClearVectorPointer (TRI_vector_pointer_t*);

////////////////////////////////////////////////////////////////////////////////
/// @brief resizes the vector
////////////////////////////////////////////////////////////////////////////////

int TRI_ResizeVectorPointer (TRI_vector_pointer_t*, size_t);

////////////////////////////////////////////////////////////////////////////////
/// @brief adds an element at the end
////////////////////////////////////////////////////////////////////////////////

int TRI_PushBackVectorPointer (TRI_vector_pointer_t*, void*);

////////////////////////////////////////////////////////////////////////////////
/// @brief adds an element at position n
////////////////////////////////////////////////////////////////////////////////

int TRI_InsertVectorPointer (TRI_vector_pointer_t*, void*, size_t);

////////////////////////////////////////////////////////////////////////////////
/// @brief removes an element, returns this element
////////////////////////////////////////////////////////////////////////////////

void* TRI_RemoveVectorPointer (TRI_vector_pointer_t*, size_t);

////////////////////////////////////////////////////////////////////////////////
/// @brief returns the element at a given position
////////////////////////////////////////////////////////////////////////////////

void* TRI_AtVectorPointer (TRI_vector_pointer_t const*, size_t);

////////////////////////////////////////////////////////////////////////////////
/// @}
////////////////////////////////////////////////////////////////////////////////

// -----------------------------------------------------------------------------
// --SECTION--                                                    STRING VECTORS
// -----------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// --SECTION--                                                      public types
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @addtogroup Collections
/// @{
////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
/// @brief string vector
///
/// Destroying a string vector will also free all strings stored inside the
/// vector.
////////////////////////////////////////////////////////////////////////////////

typedef struct TRI_vector_string_s {
  TRI_memory_zone_t* _memoryZone;
  char** _buffer;
  size_t _length;
  size_t _capacity;
}
TRI_vector_string_t;

////////////////////////////////////////////////////////////////////////////////
/// @}
////////////////////////////////////////////////////////////////////////////////

// -----------------------------------------------------------------------------
// --SECTION--                                      constructors and destructors
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @addtogroup Collections
/// @{
////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
/// @brief initialises a string vector
////////////////////////////////////////////////////////////////////////////////

void TRI_InitVectorString (TRI_vector_string_t*, TRI_memory_zone_t*);

////////////////////////////////////////////////////////////////////////////////
/// @brief initialises a string vector, with user-definable settings
////////////////////////////////////////////////////////////////////////////////

int TRI_InitVectorString2 (TRI_vector_string_t*,
                           TRI_memory_zone_t*,
                           size_t);

////////////////////////////////////////////////////////////////////////////////
/// @brief destroys a vector and all strings, but does not free the pointer
////////////////////////////////////////////////////////////////////////////////

void TRI_DestroyVectorString (TRI_vector_string_t*);

////////////////////////////////////////////////////////////////////////////////
/// @brief destroys a vector and frees the string
////////////////////////////////////////////////////////////////////////////////

void TRI_FreeVectorString (TRI_memory_zone_t*, TRI_vector_string_t*);

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
/// @brief copies a vector and all its strings
////////////////////////////////////////////////////////////////////////////////

TRI_vector_string_t* TRI_CopyVectorString (TRI_memory_zone_t*,
                                           TRI_vector_string_t const*);

////////////////////////////////////////////////////////////////////////////////
/// @brief copies all pointers from a vector
////////////////////////////////////////////////////////////////////////////////

int TRI_CopyDataVectorString (TRI_memory_zone_t*,
                              TRI_vector_string_t* dst,
                              TRI_vector_string_t const* src);

////////////////////////////////////////////////////////////////////////////////
/// @brief copies data from a TRI_vector_pointer_t into a string vector
////////////////////////////////////////////////////////////////////////////////

int TRI_CopyDataFromVectorPointerVectorString (TRI_memory_zone_t*,
                                               TRI_vector_string_t*,
                                               TRI_vector_pointer_t const*);

////////////////////////////////////////////////////////////////////////////////
/// @brief returns true if the vector is empty
////////////////////////////////////////////////////////////////////////////////

bool TRI_EmptyVectorString (TRI_vector_string_t*);

////////////////////////////////////////////////////////////////////////////////
/// @brief returns length of vector
////////////////////////////////////////////////////////////////////////////////

size_t TRI_LengthVectorString (TRI_vector_string_t const*);

////////////////////////////////////////////////////////////////////////////////
/// @brief clears the vector
////////////////////////////////////////////////////////////////////////////////

void TRI_ClearVectorString (TRI_vector_string_t*);

////////////////////////////////////////////////////////////////////////////////
/// @brief resizes the vector
////////////////////////////////////////////////////////////////////////////////

int TRI_ResizeVectorString (TRI_vector_string_t*, size_t n);

////////////////////////////////////////////////////////////////////////////////
/// @brief adds an element at the end
///
/// Note that the vector claims owenship of element.
////////////////////////////////////////////////////////////////////////////////

int TRI_PushBackVectorString (TRI_vector_string_t*, char* element);

////////////////////////////////////////////////////////////////////////////////
/// @brief adds an element at position n
///
/// Note that the vector claims owenship of element.
////////////////////////////////////////////////////////////////////////////////

int TRI_InsertVectorString (TRI_vector_string_t*, char* element, size_t n);

////////////////////////////////////////////////////////////////////////////////
/// @brief removes an element, frees this element
////////////////////////////////////////////////////////////////////////////////

void TRI_RemoveVectorString (TRI_vector_string_t*, size_t n);

////////////////////////////////////////////////////////////////////////////////
/// @brief returns the element at a given position
////////////////////////////////////////////////////////////////////////////////

char* TRI_AtVectorString (TRI_vector_string_t const*, size_t);

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
// End:
