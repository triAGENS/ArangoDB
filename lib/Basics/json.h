////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2016 ArangoDB GmbH, Cologne, Germany
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

#ifndef LIB_BASICS_JSON_H
#define LIB_BASICS_JSON_H 1

#include "Basics/Common.h"

#include "Basics/associative.h"
#include "Basics/vector.h"

struct TRI_string_buffer_s;

////////////////////////////////////////////////////////////////////////////////
/// @brief json type
////////////////////////////////////////////////////////////////////////////////

typedef enum {
  TRI_JSON_UNUSED = 0,
  TRI_JSON_NULL = 1,
  TRI_JSON_BOOLEAN = 2,
  TRI_JSON_NUMBER = 3,
  TRI_JSON_STRING = 4,
  TRI_JSON_STRING_REFERENCE = 5,
  TRI_JSON_ARRAY = 6,
  TRI_JSON_OBJECT = 7
} TRI_json_type_e;

////////////////////////////////////////////////////////////////////////////////
/// @brief json object
////////////////////////////////////////////////////////////////////////////////

typedef struct TRI_json_t {
  TRI_json_type_e _type;

  union {
    bool _boolean;
    double _number;
    TRI_blob_t _string;
    TRI_vector_t _objects;
  } _value;
} TRI_json_t;

////////////////////////////////////////////////////////////////////////////////
/// @brief creates a null object
////////////////////////////////////////////////////////////////////////////////

TRI_json_t* TRI_CreateNullJson(TRI_memory_zone_t*);

////////////////////////////////////////////////////////////////////////////////
/// @brief initializes a null object
////////////////////////////////////////////////////////////////////////////////

void TRI_InitNullJson(TRI_json_t*);

////////////////////////////////////////////////////////////////////////////////
/// @brief creates a boolean object
////////////////////////////////////////////////////////////////////////////////

TRI_json_t* TRI_CreateBooleanJson(TRI_memory_zone_t*, bool);

////////////////////////////////////////////////////////////////////////////////
/// @brief initializes a boolean object
////////////////////////////////////////////////////////////////////////////////

void TRI_InitBooleanJson(TRI_json_t*, bool);

////////////////////////////////////////////////////////////////////////////////
/// @brief creates a number object
////////////////////////////////////////////////////////////////////////////////

TRI_json_t* TRI_CreateNumberJson(TRI_memory_zone_t*, double);

////////////////////////////////////////////////////////////////////////////////
/// @brief initializes a number object
////////////////////////////////////////////////////////////////////////////////

void TRI_InitNumberJson(TRI_json_t*, double);

////////////////////////////////////////////////////////////////////////////////
/// @brief creates a string object with given length
////////////////////////////////////////////////////////////////////////////////

TRI_json_t* TRI_CreateStringJson(TRI_memory_zone_t*, char* value,
                                 size_t length);

////////////////////////////////////////////////////////////////////////////////
/// @brief creates a string object with given length, copying the string
////////////////////////////////////////////////////////////////////////////////

TRI_json_t* TRI_CreateStringCopyJson(TRI_memory_zone_t*, char const*, size_t);

////////////////////////////////////////////////////////////////////////////////
/// @brief creates a string object
////////////////////////////////////////////////////////////////////////////////

void TRI_InitStringJson(TRI_json_t*, char*, size_t);

////////////////////////////////////////////////////////////////////////////////
/// @brief initializes a string object
////////////////////////////////////////////////////////////////////////////////

int TRI_InitStringCopyJson(TRI_memory_zone_t*, TRI_json_t*, char const*,
                           size_t);

////////////////////////////////////////////////////////////////////////////////
/// @brief creates a string reference object with given length
////////////////////////////////////////////////////////////////////////////////

TRI_json_t* TRI_CreateStringReferenceJson(TRI_memory_zone_t*, char const* value,
                                          size_t length);

////////////////////////////////////////////////////////////////////////////////
/// @brief creates a string reference object
////////////////////////////////////////////////////////////////////////////////

void TRI_InitStringReferenceJson(TRI_json_t*, char const*, size_t);

////////////////////////////////////////////////////////////////////////////////
/// @brief creates an array, with the specified initial size
////////////////////////////////////////////////////////////////////////////////

TRI_json_t* TRI_CreateArrayJson(TRI_memory_zone_t*, size_t = 0);

////////////////////////////////////////////////////////////////////////////////
/// @brief initializes an array with a given size
////////////////////////////////////////////////////////////////////////////////

void TRI_InitArrayJson(TRI_memory_zone_t*, TRI_json_t*, size_t = 0);

////////////////////////////////////////////////////////////////////////////////
/// @brief creates an object with a given size
////////////////////////////////////////////////////////////////////////////////

TRI_json_t* TRI_CreateObjectJson(TRI_memory_zone_t*, size_t = 0);

////////////////////////////////////////////////////////////////////////////////
/// @brief initializes an object, using a specific initial size
////////////////////////////////////////////////////////////////////////////////

void TRI_InitObjectJson(TRI_memory_zone_t*, TRI_json_t*, size_t = 0);

////////////////////////////////////////////////////////////////////////////////
/// @brief destroys a json object, but does not free the pointer
////////////////////////////////////////////////////////////////////////////////

void TRI_DestroyJson(TRI_memory_zone_t*, TRI_json_t*);

////////////////////////////////////////////////////////////////////////////////
/// @brief destroys a json object and frees the pointer
////////////////////////////////////////////////////////////////////////////////

void TRI_FreeJson(TRI_memory_zone_t*, TRI_json_t*);

////////////////////////////////////////////////////////////////////////////////
/// @brief returns a user printable string
////////////////////////////////////////////////////////////////////////////////

char const* TRI_GetTypeStringJson(TRI_json_t const* object);

////////////////////////////////////////////////////////////////////////////////
/// @brief determines the length of an array json
////////////////////////////////////////////////////////////////////////////////

size_t TRI_LengthArrayJson(TRI_json_t const*);

////////////////////////////////////////////////////////////////////////////////
/// @brief determines the length of the json's objects vector
////////////////////////////////////////////////////////////////////////////////

size_t TRI_LengthVectorJson(TRI_json_t const*);

////////////////////////////////////////////////////////////////////////////////
/// @brief determines whether the JSON passed is of type object
////////////////////////////////////////////////////////////////////////////////

static inline bool TRI_IsObjectJson(TRI_json_t const* json) {
  return json != nullptr && json->_type == TRI_JSON_OBJECT;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief determines whether the JSON passed is of type array
////////////////////////////////////////////////////////////////////////////////

static inline bool TRI_IsArrayJson(TRI_json_t const* json) {
  return json != nullptr && json->_type == TRI_JSON_ARRAY;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief determines whether the JSON passed is of type number
////////////////////////////////////////////////////////////////////////////////

static inline bool TRI_IsNumberJson(TRI_json_t const* json) {
  return json != nullptr && json->_type == TRI_JSON_NUMBER;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief determines whether the JSON passed is of type boolean
////////////////////////////////////////////////////////////////////////////////

static inline bool TRI_IsBooleanJson(TRI_json_t const* json) {
  return json != nullptr && json->_type == TRI_JSON_BOOLEAN;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief determines whether the JSON passed is of type null
////////////////////////////////////////////////////////////////////////////////

static inline bool TRI_IsNullJson(TRI_json_t const* json) {
  return json != nullptr && json->_type == TRI_JSON_NULL;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief determines whether the JSON passed is of type string
////////////////////////////////////////////////////////////////////////////////

bool TRI_IsStringJson(TRI_json_t const* json);

////////////////////////////////////////////////////////////////////////////////
/// @brief adds a new sub-object to an array, copying it
////////////////////////////////////////////////////////////////////////////////

void TRI_PushBackArrayJson(TRI_memory_zone_t*, TRI_json_t* array,
                           TRI_json_t const* object);

////////////////////////////////////////////////////////////////////////////////
/// @brief adds a new sub-object to an array, not copying it
////////////////////////////////////////////////////////////////////////////////

int TRI_PushBack2ArrayJson(TRI_json_t* array, TRI_json_t const* object);

////////////////////////////////////////////////////////////////////////////////
/// @brief adds a new sub-object, not copying it but freeing the pointer
////////////////////////////////////////////////////////////////////////////////

int TRI_PushBack3ArrayJson(TRI_memory_zone_t*, TRI_json_t* array,
                           TRI_json_t* object);

////////////////////////////////////////////////////////////////////////////////
/// @brief looks up a value in a json array
////////////////////////////////////////////////////////////////////////////////

TRI_json_t* TRI_LookupArrayJson(TRI_json_t const*, size_t);

////////////////////////////////////////////////////////////////////////////////
/// @brief deletes an element from a json array
////////////////////////////////////////////////////////////////////////////////

bool TRI_DeleteArrayJson(TRI_memory_zone_t* zone, TRI_json_t* object,
                         size_t index);

////////////////////////////////////////////////////////////////////////////////
/// @brief adds a new attribute to an object, using copy
////////////////////////////////////////////////////////////////////////////////

void TRI_InsertObjectJson(TRI_memory_zone_t*, TRI_json_t* object,
                          char const* name, TRI_json_t const* subobject);

////////////////////////////////////////////////////////////////////////////////
/// @brief adds a new attribute  to an object, not copying it
////////////////////////////////////////////////////////////////////////////////

void TRI_Insert2ObjectJson(TRI_memory_zone_t*, TRI_json_t* object,
                           char const* name, TRI_json_t const* subobject);

////////////////////////////////////////////////////////////////////////////////
/// @brief adds a new attribute, not copying it but freeing the pointer
////////////////////////////////////////////////////////////////////////////////

void TRI_Insert3ObjectJson(TRI_memory_zone_t*, TRI_json_t* object,
                           char const* name, TRI_json_t* subobject);

////////////////////////////////////////////////////////////////////////////////
/// @brief looks up an attribute in a json object
////////////////////////////////////////////////////////////////////////////////

TRI_json_t* TRI_LookupObjectJson(TRI_json_t const* object, char const* name);

////////////////////////////////////////////////////////////////////////////////
/// @brief deletes an element from a json object
////////////////////////////////////////////////////////////////////////////////

bool TRI_DeleteObjectJson(TRI_memory_zone_t* zone, TRI_json_t* object,
                          char const* name);

////////////////////////////////////////////////////////////////////////////////
/// @brief replaces an element in a json object
////////////////////////////////////////////////////////////////////////////////

bool TRI_ReplaceObjectJson(TRI_memory_zone_t* zone, TRI_json_t* object,
                           char const* name, TRI_json_t const* replacement);

////////////////////////////////////////////////////////////////////////////////
/// @brief stringifies a json object
////////////////////////////////////////////////////////////////////////////////

int TRI_StringifyJson(struct TRI_string_buffer_s*, TRI_json_t const* object);

////////////////////////////////////////////////////////////////////////////////
/// @brief stringifies a json object skiping the outer braces
////////////////////////////////////////////////////////////////////////////////

int TRI_Stringify2Json(struct TRI_string_buffer_s*, TRI_json_t const* object);

////////////////////////////////////////////////////////////////////////////////
/// @brief prints a json object
////////////////////////////////////////////////////////////////////////////////

bool TRI_PrintJson(int fd, TRI_json_t const*, bool);

////////////////////////////////////////////////////////////////////////////////
/// @brief saves a json object
////////////////////////////////////////////////////////////////////////////////

bool TRI_SaveJson(char const*, TRI_json_t const*, bool);

////////////////////////////////////////////////////////////////////////////////
/// @brief copies a json object into a given buffer
////////////////////////////////////////////////////////////////////////////////

int TRI_CopyToJson(TRI_memory_zone_t*, TRI_json_t* dst, TRI_json_t const* src);

////////////////////////////////////////////////////////////////////////////////
/// @brief copies a json object
////////////////////////////////////////////////////////////////////////////////

TRI_json_t* TRI_CopyJson(TRI_memory_zone_t*, TRI_json_t const*);

////////////////////////////////////////////////////////////////////////////////
/// @brief parses a json string
////////////////////////////////////////////////////////////////////////////////

TRI_json_t* TRI_JsonString(TRI_memory_zone_t*, char const* text);

////////////////////////////////////////////////////////////////////////////////
/// @brief parses a json string and returns error message
////////////////////////////////////////////////////////////////////////////////

TRI_json_t* TRI_Json2String(TRI_memory_zone_t*, char const* text, char** error);

////////////////////////////////////////////////////////////////////////////////
/// @brief parses a json file and returns error message
////////////////////////////////////////////////////////////////////////////////

TRI_json_t* TRI_JsonFile(TRI_memory_zone_t*, char const* path, char** error);

////////////////////////////////////////////////////////////////////////////////
/// @brief converts a json object into a number
////////////////////////////////////////////////////////////////////////////////

int64_t TRI_ToInt64Json(TRI_json_t const*);

////////////////////////////////////////////////////////////////////////////////
/// @brief converts a json object into a number, the second parameter
///        indicates if the conversion was successful. If not it will
///        return 0.0 and caller can react to fail or not.
////////////////////////////////////////////////////////////////////////////////

double TRI_ToDoubleJson(TRI_json_t const*, bool&);

////////////////////////////////////////////////////////////////////////////////
/// @brief default deleter for TRI_json_t
/// this can be used to put a TRI_json_t with TRI_UNKNOWN_MEM_ZONE into an
/// std::unique_ptr
////////////////////////////////////////////////////////////////////////////////

namespace std {
template <>
class default_delete<TRI_json_t> {
 public:
  void operator()(TRI_json_t* json) {
    if (json != nullptr) {
      TRI_FreeJson(TRI_UNKNOWN_MEM_ZONE, json);
    }
  }
};
}

#endif
