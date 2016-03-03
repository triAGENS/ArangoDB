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

#ifndef LIB_BASICS_STRUCTURES_H
#define LIB_BASICS_STRUCTURES_H 1

#ifndef TRI_WITHIN_COMMON
#error use <Basics/Common.h>
#endif

////////////////////////////////////////////////////////////////////////////////
/// @brief type of date-time entries (seconds since 1970-01-01)
////////////////////////////////////////////////////////////////////////////////

typedef double TRI_datetime_t;

////////////////////////////////////////////////////////////////////////////////
/// @brief type of date entries (days since 1970-01-01)
////////////////////////////////////////////////////////////////////////////////

typedef int32_t TRI_date_t;

////////////////////////////////////////////////////////////////////////////////
/// @brief type of duration entries in seconds or seconds since 00:00:00
////////////////////////////////////////////////////////////////////////////////

typedef int32_t TRI_seconds_t;

////////////////////////////////////////////////////////////////////////////////
/// @brief global blob type
///
/// A blob contains a length and data. The size of the blob is limited to
/// 4 GByte.
////////////////////////////////////////////////////////////////////////////////

typedef struct TRI_blob_s {
  char* data;
  uint32_t length;
} TRI_blob_t;

////////////////////////////////////////////////////////////////////////////////
/// @brief destorys the data of blob, but does not free the pointer
////////////////////////////////////////////////////////////////////////////////

void TRI_DestroyBlob(TRI_memory_zone_t*, TRI_blob_t*);

////////////////////////////////////////////////////////////////////////////////
/// @brief copies a blob into given destination
////////////////////////////////////////////////////////////////////////////////

int TRI_CopyToBlob(TRI_memory_zone_t*, TRI_blob_t* dst, TRI_blob_t const* src);

////////////////////////////////////////////////////////////////////////////////
/// @brief assigns a blob value by reference into given destination
////////////////////////////////////////////////////////////////////////////////

int TRI_AssignToBlob(TRI_memory_zone_t* zone, TRI_blob_t* dst,
                     TRI_blob_t const* src);

#endif
