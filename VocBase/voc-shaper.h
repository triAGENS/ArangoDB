////////////////////////////////////////////////////////////////////////////////
/// @brief json shaper used to compute the shape of an json object
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
/// @author Martin Schoenert
/// @author Copyright 2006-2011, triAGENS GmbH, Cologne, Germany
////////////////////////////////////////////////////////////////////////////////

#ifndef TRIAGENS_DURHAM_VOC_BASE_VOC_SHAPER_H
#define TRIAGENS_DURHAM_VOC_BASE_VOC_SHAPER_H 1

#include <Basics/Common.h>

#include <VocBase/datafile.h>
#include <VocBase/blob-collection.h>

#include "ShapedJson/json-shaper.h"
#include "ShapedJson/shaped-json.h"

#ifdef __cplusplus
extern "C" {
#endif

// -----------------------------------------------------------------------------
// --SECTION--                                                      public types
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @addtogroup VocBase
/// @{
////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
/// @brief datafile attribute marker
////////////////////////////////////////////////////////////////////////////////

typedef struct TRI_df_attribute_marker_s {
  TRI_df_marker_t base;

  TRI_shape_aid_t _aid;
  TRI_shape_size_t _size;

  // char name[]
}
TRI_df_attribute_marker_t;

////////////////////////////////////////////////////////////////////////////////
/// @brief datafile shape marker
////////////////////////////////////////////////////////////////////////////////

typedef struct TRI_df_shape_marker_s {
  TRI_df_marker_t base;
}
TRI_df_shape_marker_t;

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
/// @brief returns the underlying collection
////////////////////////////////////////////////////////////////////////////////

TRI_blob_collection_t* TRI_CollectionVocShaper (TRI_shaper_t* shaper);

////////////////////////////////////////////////////////////////////////////////
/// @brief creates persistent shaper
////////////////////////////////////////////////////////////////////////////////

TRI_shaper_t* TRI_CreateVocShaper (char const* path, char const* name);

////////////////////////////////////////////////////////////////////////////////
/// @brief opens a persistent shaper
////////////////////////////////////////////////////////////////////////////////

TRI_shaper_t* TRI_OpenVocShaper (char const* filename);

////////////////////////////////////////////////////////////////////////////////
/// @brief closes a persistent shaper
////////////////////////////////////////////////////////////////////////////////

int TRI_CloseVocShaper (TRI_shaper_t* s);

////////////////////////////////////////////////////////////////////////////////
/// @brief destroys an persistent shaper, but does not free the pointer
////////////////////////////////////////////////////////////////////////////////

void TRI_DestroyVocShaper (TRI_shaper_t*);

////////////////////////////////////////////////////////////////////////////////
/// @brief destroys an persistent shaper and frees the pointer
////////////////////////////////////////////////////////////////////////////////

void TRI_FreeVocShaper (TRI_shaper_t*);

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
