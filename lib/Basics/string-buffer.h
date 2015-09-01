////////////////////////////////////////////////////////////////////////////////
/// @brief a string buffer for sequential string concatenation
///
/// @file
///
/// DISCLAIMER
///
/// Copyright 2014 ArangoDB GmbH, Cologne, Germany
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
/// @author Copyright 2014, ArangoDB GmbH, Cologne, Germany
/// @author Copyright 2011-2013, triAGENS GmbH, Cologne, Germany
////////////////////////////////////////////////////////////////////////////////

#ifndef ARANGODB_BASICS_C_STRING__BUFFER_H
#define ARANGODB_BASICS_C_STRING__BUFFER_H 1

#include "Basics/Common.h"

// -----------------------------------------------------------------------------
// --SECTION--                                                      public types
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief string buffer with formatting routines
////////////////////////////////////////////////////////////////////////////////

typedef struct TRI_string_buffer_s {
  TRI_memory_zone_t* _memoryZone;
  char*              _buffer;
  char*              _current;
  size_t             _len;
}
TRI_string_buffer_t;

// -----------------------------------------------------------------------------
// --SECTION--                                      constructors and destructors
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief create a new string buffer and initialize it
////////////////////////////////////////////////////////////////////////////////

TRI_string_buffer_t* TRI_CreateStringBuffer (TRI_memory_zone_t*);

////////////////////////////////////////////////////////////////////////////////
/// @brief create a new string buffer and initialize it with a specific size
////////////////////////////////////////////////////////////////////////////////

TRI_string_buffer_t* TRI_CreateSizedStringBuffer (TRI_memory_zone_t*,
                                                  size_t);

////////////////////////////////////////////////////////////////////////////////
/// @brief initializes the string buffer
///
/// @warning You must call initialize before using the string buffer.
////////////////////////////////////////////////////////////////////////////////

void TRI_InitStringBuffer (TRI_string_buffer_t *, TRI_memory_zone_t*);

////////////////////////////////////////////////////////////////////////////////
/// @brief initializes the string buffer with a specific size
///
/// @warning You must call initialize before using the string buffer.
////////////////////////////////////////////////////////////////////////////////

void TRI_InitSizedStringBuffer (TRI_string_buffer_t *,
                                TRI_memory_zone_t*,
                                const size_t);

////////////////////////////////////////////////////////////////////////////////
/// @brief frees the string buffer
///
/// @warning You must call free or destroy after using the string buffer.
////////////////////////////////////////////////////////////////////////////////

void TRI_DestroyStringBuffer (TRI_string_buffer_t *);

////////////////////////////////////////////////////////////////////////////////
/// @brief frees the string buffer and cleans the buffer
///
/// @warning You must call free after or destroy using the string buffer.
////////////////////////////////////////////////////////////////////////////////

void TRI_AnnihilateStringBuffer (TRI_string_buffer_t *);

////////////////////////////////////////////////////////////////////////////////
/// @brief frees the string buffer and the pointer
////////////////////////////////////////////////////////////////////////////////

void TRI_FreeStringBuffer (TRI_memory_zone_t*, TRI_string_buffer_t *);

// -----------------------------------------------------------------------------
// --SECTION--                                                  public functions
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief compress the string buffer using deflate
////////////////////////////////////////////////////////////////////////////////

int TRI_DeflateStringBuffer (TRI_string_buffer_t*,
                             size_t);

////////////////////////////////////////////////////////////////////////////////
/// @brief ensure the string buffer has a specific capacity
////////////////////////////////////////////////////////////////////////////////

int TRI_ReserveStringBuffer (TRI_string_buffer_t*,
                             const size_t);

////////////////////////////////////////////////////////////////////////////////
/// @brief swaps content with another string buffer
////////////////////////////////////////////////////////////////////////////////

void TRI_SwapStringBuffer (TRI_string_buffer_t*,
                           TRI_string_buffer_t*);

////////////////////////////////////////////////////////////////////////////////
/// @brief returns pointer to the beginning of the character buffer
////////////////////////////////////////////////////////////////////////////////

char const * TRI_BeginStringBuffer (TRI_string_buffer_t const*);

////////////////////////////////////////////////////////////////////////////////
/// @brief returns pointer to the end of the character buffer
////////////////////////////////////////////////////////////////////////////////

char const * TRI_EndStringBuffer (TRI_string_buffer_t const*);

////////////////////////////////////////////////////////////////////////////////
/// @brief returns length of the character buffer
////////////////////////////////////////////////////////////////////////////////

size_t TRI_LengthStringBuffer (TRI_string_buffer_t const*);

////////////////////////////////////////////////////////////////////////////////
/// @brief increases length of the character buffer
////////////////////////////////////////////////////////////////////////////////

void TRI_IncreaseLengthStringBuffer (TRI_string_buffer_t*,
                                     size_t);

////////////////////////////////////////////////////////////////////////////////
/// @brief returns true if buffer is empty
////////////////////////////////////////////////////////////////////////////////

bool TRI_EmptyStringBuffer (TRI_string_buffer_t const*);

////////////////////////////////////////////////////////////////////////////////
/// @brief clears the buffer
////////////////////////////////////////////////////////////////////////////////

void TRI_ClearStringBuffer (TRI_string_buffer_t*);

////////////////////////////////////////////////////////////////////////////////
/// @brief resets the buffer (without clearing)
////////////////////////////////////////////////////////////////////////////////

void TRI_ResetStringBuffer (TRI_string_buffer_t*);

////////////////////////////////////////////////////////////////////////////////
/// @brief steals the buffer of a string buffer
////////////////////////////////////////////////////////////////////////////////

char* TRI_StealStringBuffer (TRI_string_buffer_t*);

////////////////////////////////////////////////////////////////////////////////
/// @brief copies the string buffer
////////////////////////////////////////////////////////////////////////////////

int TRI_CopyStringBuffer (TRI_string_buffer_t*,
                          TRI_string_buffer_t const*);

////////////////////////////////////////////////////////////////////////////////
/// @brief removes the first characters
////////////////////////////////////////////////////////////////////////////////

void TRI_EraseFrontStringBuffer (TRI_string_buffer_t*,
                                 size_t);

////////////////////////////////////////////////////////////////////////////////
/// @brief removes the first characters but does not clear the remaining
/// buffer space
////////////////////////////////////////////////////////////////////////////////

void TRI_MoveFrontStringBuffer (TRI_string_buffer_t*,
                                size_t);

////////////////////////////////////////////////////////////////////////////////
/// @brief replaces characters
////////////////////////////////////////////////////////////////////////////////

int TRI_ReplaceStringStringBuffer (TRI_string_buffer_t*,
                                   char const*,
                                   size_t);

// -----------------------------------------------------------------------------
// --SECTION--                                                  STRING APPENDERS
// -----------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// --SECTION--                                                  public functions
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief appends character
////////////////////////////////////////////////////////////////////////////////

int TRI_AppendCharStringBuffer (TRI_string_buffer_t* self, char chr);

////////////////////////////////////////////////////////////////////////////////
/// @brief appends characters
////////////////////////////////////////////////////////////////////////////////

int TRI_AppendStringStringBuffer (TRI_string_buffer_t* self, char const* str);

////////////////////////////////////////////////////////////////////////////////
/// @brief appends characters
////////////////////////////////////////////////////////////////////////////////

int TRI_AppendString2StringBuffer (TRI_string_buffer_t* self, char const* str, size_t len);

////////////////////////////////////////////////////////////////////////////////
/// @brief appends characters but url-encode the string
////////////////////////////////////////////////////////////////////////////////

int TRI_AppendUrlEncodedStringStringBuffer (TRI_string_buffer_t* self, char const* str);

////////////////////////////////////////////////////////////////////////////////
/// @brief appends characters but json-encode the null-terminated string
////////////////////////////////////////////////////////////////////////////////

int TRI_AppendJsonEncodedStringStringBuffer (TRI_string_buffer_t* self, char const* str, bool);

////////////////////////////////////////////////////////////////////////////////
/// @brief appends characters but json-encode the string
////////////////////////////////////////////////////////////////////////////////

int TRI_AppendJsonEncodedStringStringBuffer (TRI_string_buffer_t* self, char const* str, size_t, bool);

// -----------------------------------------------------------------------------
// --SECTION--                                                 INTEGER APPENDERS
// -----------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// --SECTION--                                                  public functions
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief appends integer with two digits
////////////////////////////////////////////////////////////////////////////////

int TRI_AppendInteger2StringBuffer (TRI_string_buffer_t * self, uint32_t attr);

////////////////////////////////////////////////////////////////////////////////
/// @brief appends integer with three digits
////////////////////////////////////////////////////////////////////////////////

int TRI_AppendInteger3StringBuffer (TRI_string_buffer_t * self, uint32_t attr);

////////////////////////////////////////////////////////////////////////////////
/// @brief appends integer with four digits
////////////////////////////////////////////////////////////////////////////////

int TRI_AppendInteger4StringBuffer (TRI_string_buffer_t * self, uint32_t attr);

////////////////////////////////////////////////////////////////////////////////
/// @brief appends integer with 8 bits
////////////////////////////////////////////////////////////////////////////////

int TRI_AppendInt8StringBuffer (TRI_string_buffer_t * self, int8_t attr);

////////////////////////////////////////////////////////////////////////////////
/// @brief appends unsigned integer with 8 bits
////////////////////////////////////////////////////////////////////////////////

int TRI_AppendUInt8StringBuffer (TRI_string_buffer_t * self, uint8_t attr);

////////////////////////////////////////////////////////////////////////////////
/// @brief appends integer with 16 bits
////////////////////////////////////////////////////////////////////////////////

int TRI_AppendInt16StringBuffer (TRI_string_buffer_t * self, int16_t attr);

////////////////////////////////////////////////////////////////////////////////
/// @brief appends unsigned integer with 32 bits
////////////////////////////////////////////////////////////////////////////////

int TRI_AppendUInt16StringBuffer (TRI_string_buffer_t * self, uint16_t attr);

////////////////////////////////////////////////////////////////////////////////
/// @brief appends integer with 32 bits
////////////////////////////////////////////////////////////////////////////////

int TRI_AppendInt32StringBuffer (TRI_string_buffer_t * self, int32_t attr);

////////////////////////////////////////////////////////////////////////////////
/// @brief appends unsigned integer with 32 bits
////////////////////////////////////////////////////////////////////////////////

int TRI_AppendUInt32StringBuffer (TRI_string_buffer_t * self, uint32_t attr);

////////////////////////////////////////////////////////////////////////////////
/// @brief appends integer with 64 bits
////////////////////////////////////////////////////////////////////////////////

int TRI_AppendInt64StringBuffer (TRI_string_buffer_t * self, int64_t attr);

////////////////////////////////////////////////////////////////////////////////
/// @brief appends unsigned integer with 64 bits
////////////////////////////////////////////////////////////////////////////////

int TRI_AppendUInt64StringBuffer (TRI_string_buffer_t * self, uint64_t attr);

// -----------------------------------------------------------------------------
// --SECTION--                                           INTEGER OCTAL APPENDERS
// -----------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// --SECTION--                                                  public functions
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief appends unsigned integer with 32 bits in octal
////////////////////////////////////////////////////////////////////////////////

int TRI_AppendUInt32OctalStringBuffer (TRI_string_buffer_t * self, uint32_t attr);

////////////////////////////////////////////////////////////////////////////////
/// @brief appends unsigned integer with 64 bits in octal
////////////////////////////////////////////////////////////////////////////////

int TRI_AppendUInt64OctalStringBuffer (TRI_string_buffer_t * self, uint64_t attr);

// -----------------------------------------------------------------------------
// --SECTION--                                             INTEGER HEX APPENDERS
// -----------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// --SECTION--                                                  public functions
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief appends unsigned integer with 32 bits in hex
////////////////////////////////////////////////////////////////////////////////

int TRI_AppendUInt32HexStringBuffer (TRI_string_buffer_t * self, uint32_t attr);

////////////////////////////////////////////////////////////////////////////////
/// @brief appends unsigned integer with 64 bits in hex
////////////////////////////////////////////////////////////////////////////////

int TRI_AppendUInt64HexStringBuffer (TRI_string_buffer_t * self, uint64_t attr);

// -----------------------------------------------------------------------------
// --SECTION--                                                   FLOAT APPENDERS
// -----------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// --SECTION--                                                  public functions
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief appends floating point number with 8 bits
////////////////////////////////////////////////////////////////////////////////

int TRI_AppendDoubleStringBuffer (TRI_string_buffer_t * self, double attr);

// -----------------------------------------------------------------------------
// --SECTION--                                           DATE AND TIME APPENDERS
// -----------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// --SECTION--                                                  public functions
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief appends time in standard format
////////////////////////////////////////////////////////////////////////////////

int TRI_AppendTimeStringBuffer (TRI_string_buffer_t * self, int32_t attr);

// -----------------------------------------------------------------------------
// --SECTION--                                                     CSV APPENDERS
// -----------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// --SECTION--                                                  public functions
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief appends csv 32-bit integer
////////////////////////////////////////////////////////////////////////////////

int TRI_AppendCsvInt32StringBuffer (TRI_string_buffer_t * self, int32_t i);

////////////////////////////////////////////////////////////////////////////////
/// @brief appends csv unisgned 32-bit integer
////////////////////////////////////////////////////////////////////////////////

int TRI_AppendCsvUInt32StringBuffer (TRI_string_buffer_t * self, uint32_t i);

////////////////////////////////////////////////////////////////////////////////
/// @brief appends csv 64-bit integer
////////////////////////////////////////////////////////////////////////////////

int TRI_AppendCsvInt64StringBuffer (TRI_string_buffer_t * self, int64_t i);

////////////////////////////////////////////////////////////////////////////////
/// @brief appends csv unsigned 64-bit integer
////////////////////////////////////////////////////////////////////////////////

int TRI_AppendCsvUInt64StringBuffer (TRI_string_buffer_t * self, uint64_t i);

////////////////////////////////////////////////////////////////////////////////
/// @brief appends csv double
////////////////////////////////////////////////////////////////////////////////

int TRI_AppendCsvDoubleStringBuffer (TRI_string_buffer_t * self, double d);

#endif

// -----------------------------------------------------------------------------
// --SECTION--                                                       END-OF-FILE
// -----------------------------------------------------------------------------

// Local Variables:
// mode: outline-minor
// outline-regexp: "/// @brief\\|/// {@inheritDoc}\\|/// @page\\|// --SECTION--\\|/// @\\}"
// End:
