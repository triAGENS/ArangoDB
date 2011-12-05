////////////////////////////////////////////////////////////////////////////////
/// @brief vector implementation
///
/// @file
///
/// DISCLAIMER
///
/// Copyright 2010-2011 triagens GmbH, Cologne, Germany
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
/// @author Copyright 2011, triagens GmbH, Cologne, Germany
////////////////////////////////////////////////////////////////////////////////

#ifndef TRIAGENS_PHILADELPHIA_BASICS_VECTOR_H
#define TRIAGENS_PHILADELPHIA_BASICS_VECTOR_H 1

#include <Basics/Common.h>

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
/// @brief pod vector
////////////////////////////////////////////////////////////////////////////////

typedef struct TRI_vector_s {
  size_t _elementSize;
  char * _buffer;
  size_t _length;
  size_t _capacity;
}
TRI_vector_t;

////////////////////////////////////////////////////////////////////////////////
/// @brief pointer vector
////////////////////////////////////////////////////////////////////////////////

typedef struct TRI_vector_pointer_s {
  void** _buffer;
  size_t _length;
  size_t _capacity;
}
TRI_vector_pointer_t;

////////////////////////////////////////////////////////////////////////////////
/// @brief string vector
///
/// Destroying a string vector will also free all strings stored inside the
/// vector.
////////////////////////////////////////////////////////////////////////////////

typedef struct TRI_vector_string_s {
  char** _buffer;
  size_t _length;
  size_t _capacity;
}
TRI_vector_string_t;

////////////////////////////////////////////////////////////////////////////////
/// @}
////////////////////////////////////////////////////////////////////////////////

// -----------------------------------------------------------------------------
// --SECTION--                                                       POD VECTORS
// -----------------------------------------------------------------------------

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

void TRI_InitVector (TRI_vector_t*, size_t elementSize);

////////////////////////////////////////////////////////////////////////////////
/// @brief destroys a vector, but does not free the pointer
////////////////////////////////////////////////////////////////////////////////

void TRI_DestroyVector (TRI_vector_t*);

////////////////////////////////////////////////////////////////////////////////
/// @brief destroys a vector and frees the pointer
////////////////////////////////////////////////////////////////////////////////

void TRI_FreeVector (TRI_vector_t*);

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
/// @brief copies a vector
////////////////////////////////////////////////////////////////////////////////

TRI_vector_t* TRI_CopyVector (TRI_vector_t*);

////////////////////////////////////////////////////////////////////////////////
/// @brief returns true if the vector is empty
////////////////////////////////////////////////////////////////////////////////

bool TRI_EmptyVector (TRI_vector_t const*);

////////////////////////////////////////////////////////////////////////////////
/// @brief clears the vector
////////////////////////////////////////////////////////////////////////////////

void TRI_ClearVector (TRI_vector_t*);

////////////////////////////////////////////////////////////////////////////////
/// @brief resizes the vector
////////////////////////////////////////////////////////////////////////////////

void TRI_ResizeVector (TRI_vector_t*, size_t n);

////////////////////////////////////////////////////////////////////////////////
/// @brief adds and element at the end
////////////////////////////////////////////////////////////////////////////////

void TRI_PushBackVector (TRI_vector_t*, void const* element);

////////////////////////////////////////////////////////////////////////////////
/// @brief removes an element
////////////////////////////////////////////////////////////////////////////////

void TRI_RemoveVector (TRI_vector_t*, size_t n);

////////////////////////////////////////////////////////////////////////////////
/// @brief returns the element at a given position
////////////////////////////////////////////////////////////////////////////////

void* TRI_AtVector (TRI_vector_t const*, size_t);

////////////////////////////////////////////////////////////////////////////////
/// @brief sets an element at a given position
////////////////////////////////////////////////////////////////////////////////

void TRI_SetVector (TRI_vector_t* vector, size_t, void const* element);

////////////////////////////////////////////////////////////////////////////////
/// @brief returns the begining
////////////////////////////////////////////////////////////////////////////////

void* TRI_BeginVector (TRI_vector_t*);

////////////////////////////////////////////////////////////////////////////////
/// @brief returns the end
////////////////////////////////////////////////////////////////////////////////

void* TRI_EndVector (TRI_vector_t*);

////////////////////////////////////////////////////////////////////////////////
/// @}
////////////////////////////////////////////////////////////////////////////////

// -----------------------------------------------------------------------------
// --SECTION--                                                   POINTER VECTORS
// -----------------------------------------------------------------------------

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

void TRI_InitVectorPointer (TRI_vector_pointer_t*);

////////////////////////////////////////////////////////////////////////////////
/// @brief destroys a vector, but does not free the pointer
////////////////////////////////////////////////////////////////////////////////

void TRI_DestroyVectorPointer (TRI_vector_pointer_t*);

////////////////////////////////////////////////////////////////////////////////
/// @brief destroys a vector and frees the pointer
////////////////////////////////////////////////////////////////////////////////

void TRI_FreeVectorPointer (TRI_vector_pointer_t*);

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
/// @brief copies a vector
////////////////////////////////////////////////////////////////////////////////

TRI_vector_pointer_t* TRI_CopyVectorPointer (TRI_vector_pointer_t*);

////////////////////////////////////////////////////////////////////////////////
/// @brief copies all pointers from a vector
////////////////////////////////////////////////////////////////////////////////

void TRI_CopyDataVectorPointer (TRI_vector_pointer_t* dst, TRI_vector_pointer_t* src);

////////////////////////////////////////////////////////////////////////////////
/// @brief returns true if the vector is empty
////////////////////////////////////////////////////////////////////////////////

bool TRI_EmptyVectorPointer (TRI_vector_pointer_t*);

////////////////////////////////////////////////////////////////////////////////
/// @brief clears the vector
////////////////////////////////////////////////////////////////////////////////

void TRI_ClearVectorPointer (TRI_vector_pointer_t*);

////////////////////////////////////////////////////////////////////////////////
/// @brief resizes the vector
////////////////////////////////////////////////////////////////////////////////

void TRI_ResizeVectorPointer (TRI_vector_pointer_t*, size_t n);

////////////////////////////////////////////////////////////////////////////////
/// @brief adds and element at the end
////////////////////////////////////////////////////////////////////////////////

void TRI_PushBackVectorPointer (TRI_vector_pointer_t*, void* element);

////////////////////////////////////////////////////////////////////////////////
/// @brief removes an element, returns this element
////////////////////////////////////////////////////////////////////////////////

void* TRI_RemoveVectorPointer (TRI_vector_pointer_t*, size_t n);

////////////////////////////////////////////////////////////////////////////////
/// @}
////////////////////////////////////////////////////////////////////////////////

// -----------------------------------------------------------------------------
// --SECTION--                                                    STRING VECTORS
// -----------------------------------------------------------------------------

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

void TRI_InitVectorString (TRI_vector_string_t*);

////////////////////////////////////////////////////////////////////////////////
/// @brief destroys a vector and all strings, but does not free the pointer
////////////////////////////////////////////////////////////////////////////////

void TRI_DestroyVectorString (TRI_vector_string_t*);

////////////////////////////////////////////////////////////////////////////////
/// @brief destroys a vector and frees the string
////////////////////////////////////////////////////////////////////////////////

void TRI_FreeVectorString (TRI_vector_string_t*);

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

TRI_vector_string_t* TRI_CopyVectorString (TRI_vector_string_t*);

////////////////////////////////////////////////////////////////////////////////
/// @brief returns true if the vector is empty
////////////////////////////////////////////////////////////////////////////////

bool TRI_EmptyVectorString (TRI_vector_string_t*);

////////////////////////////////////////////////////////////////////////////////
/// @brief clears the vector
////////////////////////////////////////////////////////////////////////////////

void TRI_ClearVectorString (TRI_vector_string_t*);

////////////////////////////////////////////////////////////////////////////////
/// @brief resizes the vector
////////////////////////////////////////////////////////////////////////////////

void TRI_ResizeVectorString (TRI_vector_string_t*, size_t n);

////////////////////////////////////////////////////////////////////////////////
/// @brief adds and element at the end
///
/// Note that the vector claims owenship of element.
////////////////////////////////////////////////////////////////////////////////

void TRI_PushBackVectorString (TRI_vector_string_t*, char* element);

////////////////////////////////////////////////////////////////////////////////
/// @brief removes an element, frees this element
////////////////////////////////////////////////////////////////////////////////

void TRI_RemoveVectorString (TRI_vector_string_t*, size_t n);

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
