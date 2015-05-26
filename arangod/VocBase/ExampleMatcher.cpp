////////////////////////////////////////////////////////////////////////////////
/// @brief Class to allow simple byExample matching of mptr.
///
/// @file
///
/// DISCLAIMER
///
/// Copyright 2014-2015 ArangoDB GmbH, Cologne, Germany
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
/// @author Michael Hackstein
/// @author Copyright 2014-2015, ArangoDB GmbH, Cologne, Germany
/// @author Copyright 2011-2013, triAGENS GmbH, Cologne, Germany
////////////////////////////////////////////////////////////////////////////////

#include "ExampleMatcher.h"
#include "voc-shaper.h"
#include "V8/v8-utils.h"
#include "V8/v8-conv.h"
#include "Basics/JsonHelper.h"

using namespace std;
using namespace triagens::arango;
using namespace triagens::basics;


////////////////////////////////////////////////////////////////////////////////
/// @brief cleans up the example object
////////////////////////////////////////////////////////////////////////////////

void ExampleMatcher::cleanup () {
  auto zone = _shaper->_memoryZone;
  // clean shaped json objects
  for (auto def : definitions) {
    for (auto it : def._values) {
      TRI_FreeShapedJson(zone, it);
    }
  }
}

////////////////////////////////////////////////////////////////////////////////
/// @brief Constructor using a v8::Object example
////////////////////////////////////////////////////////////////////////////////
 
ExampleMatcher::ExampleMatcher (
  v8::Isolate* isolate,
  v8::Handle<v8::Object> const example,
  TRI_shaper_t* shaper,
  std::string& errorMessage
) : _shaper(shaper) {
  v8::Handle<v8::Array> names = example->GetOwnPropertyNames();
  size_t n = names->Length();

  ExampleDefinition def;

  def._pids.reserve(n);
  def._values.reserve(n);

  try { 
    for (size_t i = 0;  i < n;  ++i) {
      v8::Handle<v8::Value> key = names->Get((uint32_t) i);
      v8::Handle<v8::Value> val = example->Get(key);
      TRI_Utf8ValueNFC keyStr(TRI_UNKNOWN_MEM_ZONE, key);
      if (*keyStr != nullptr) {
        auto pid = _shaper->lookupAttributePathByName(_shaper, *keyStr);

        if (pid == 0) {
          // no attribute path found. this means the result will be empty
          ExampleMatcher::cleanup();
          throw TRI_RESULT_ELEMENT_NOT_FOUND;
        }
        def._pids.push_back(pid);

        auto value = TRI_ShapedJsonV8Object(isolate, val, shaper, false);

        if (value == nullptr) {
          ExampleMatcher::cleanup();
          throw TRI_RESULT_ELEMENT_NOT_FOUND;
        }
        def._values.push_back(value);
      }
      else {
        ExampleMatcher::cleanup();
        errorMessage = "cannot convert attribute path to UTF8";
        throw TRI_ERROR_BAD_PARAMETER;
      }
    }
  } catch (bad_alloc& e) {
    ExampleMatcher::cleanup();
    throw;
  }
  definitions.emplace_back(move(def)); 
};

////////////////////////////////////////////////////////////////////////////////
/// @brief Constructor using an v8::Array of v8::Object examples
////////////////////////////////////////////////////////////////////////////////
 
ExampleMatcher::ExampleMatcher (
  v8::Isolate* isolate,
  v8::Handle<v8::Array> const examples,
  TRI_shaper_t* shaper,
  std::string& errorMessage
) : _shaper(shaper) {
  size_t exCount = examples->Length();
  for (size_t j = 0; j < exCount; ++j) {
    auto tmp = examples->Get((uint32_t) j);
    if (!tmp->IsObject() || tmp->IsArray()) {
      // Right now silently ignore this example
      continue;
    }
    v8::Handle<v8::Object> example = v8::Handle<v8::Object>::Cast(tmp);
    v8::Handle<v8::Array> names = example->GetOwnPropertyNames();
    size_t n = names->Length();

    ExampleDefinition def;

    def._pids.reserve(n);
    def._values.reserve(n);

    try { 
      for (size_t i = 0;  i < n;  ++i) {
        v8::Handle<v8::Value> key = names->Get((uint32_t) i);
        v8::Handle<v8::Value> val = example->Get(key);
        TRI_Utf8ValueNFC keyStr(TRI_UNKNOWN_MEM_ZONE, key);
        if (*keyStr != nullptr) {
          auto pid = _shaper->lookupAttributePathByName(_shaper, *keyStr);

          if (pid == 0) {
            // no attribute path found. this means the result will be empty
            ExampleMatcher::cleanup();
            throw TRI_RESULT_ELEMENT_NOT_FOUND;
          }
          def._pids.push_back(pid);

          auto value = TRI_ShapedJsonV8Object(isolate, val, shaper, false);

          if (value == nullptr) {
            ExampleMatcher::cleanup();
            throw TRI_RESULT_ELEMENT_NOT_FOUND;
          }
          def._values.push_back(value);
        }
        else {
          ExampleMatcher::cleanup();
          errorMessage = "cannot convert attribute path to UTF8";
          throw TRI_ERROR_BAD_PARAMETER;
        }
      }
    } catch (bad_alloc& e) {
      ExampleMatcher::cleanup();
      throw;
    }
    definitions.emplace_back(move(def)); 
  }
};






////////////////////////////////////////////////////////////////////////////////
/// @brief Constructor using a TRI_json_t object
////////////////////////////////////////////////////////////////////////////////

ExampleMatcher::ExampleMatcher (
  TRI_json_t* const example,
  TRI_shaper_t* shaper
) : _shaper(shaper) {
  // Just make sure we are talking about objects
  TRI_ASSERT(TRI_IsObjectJson(example));

  TRI_vector_t objects = example->_value._objects;

  ExampleDefinition def;

  // Trolololol std::vector in C... ;(
  size_t n = TRI_LengthVector(&objects); 

  def._pids.reserve(n);
  def._values.reserve(n);

  try { 
    for (size_t i = 0; i < n; i += 2) {
      TRI_json_t* keyobj = static_cast<TRI_json_t*>(TRI_AtVector(&objects, i));
      TRI_ASSERT(TRI_IsStringJson(keyobj));
      auto pid = _shaper->lookupAttributePathByName(_shaper, keyobj->_value._string.data);

      if (pid == 0) {
        // no attribute path found. this means the result will be empty
        ExampleMatcher::cleanup();
        throw TRI_RESULT_ELEMENT_NOT_FOUND;
      }

      def._pids.push_back(pid);
      TRI_json_t* jsonvalue = static_cast<TRI_json_t*>(TRI_AtVector(&objects, i+1));
      auto value = TRI_ShapedJsonJson(_shaper, jsonvalue, false);


      if (value == nullptr) {
        ExampleMatcher::cleanup();
        throw TRI_RESULT_ELEMENT_NOT_FOUND;
      }
      def._values.push_back(value);
    }
  } catch (bad_alloc& e) {
    ExampleMatcher::cleanup();
    throw;
  }
  definitions.emplace_back(move(def)); 
};


////////////////////////////////////////////////////////////////////////////////
/// @brief Checks if the given mptr matches the examples in this class
////////////////////////////////////////////////////////////////////////////////

bool ExampleMatcher::matches (TRI_doc_mptr_t const* mptr) const {
  if (mptr == nullptr) {
    return false;
  }
  for (auto def : definitions) {
    TRI_shaped_json_t document;
    TRI_shaped_json_t result;
    TRI_shape_t const* shape;

    TRI_EXTRACT_SHAPED_JSON_MARKER(document, mptr->getDataPtr());
    for (size_t i = 0;  i < def._values.size();  ++i) {
      TRI_shaped_json_t* example = def._values[i];

      bool ok = TRI_ExtractShapedJsonVocShaper(_shaper,
                                               &document,
                                               example->_sid,
                                               def._pids[i],
                                               &result,
                                               &shape);

      if (! ok || shape == nullptr) {
        goto nextExample;
      }

      if (result._data.length != example->_data.length) {
        // suppress excessive log spam
        // LOG_TRACE("expecting length %lu, got length %lu for path %lu",
        //           (unsigned long) result._data.length,
        //           (unsigned long) example->_data.length,
        //           (unsigned long) pids[i]);

        goto nextExample;
      }

      if (memcmp(result._data.data, example->_data.data, example->_data.length) != 0) {
        // suppress excessive log spam
        // LOG_TRACE("data mismatch at path %lu", (unsigned long) pids[i]);
        goto nextExample;
      }
    }
    // If you get here this example matches. Successful hit.
    return true;
    nextExample:
    // This Example does not match, try the next one. Goto requires a statement
    continue;
  }
  return false;
};
