////////////////////////////////////////////////////////////////////////////////
/// @brief index base class
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

#ifndef ARANGODB_INDEXES_INDEX_H
#define ARANGODB_INDEXES_INDEX_H 1

#include "Basics/Common.h"
#include "Basics/AttributeNameParser.h"
#include "Basics/JsonHelper.h"
#include "Basics/logging.h"
#include "VocBase/document-collection.h"
#include "VocBase/shaped-json.h"
#include "VocBase/vocbase.h"
#include "VocBase/VocShaper.h"
#include "VocBase/voc-types.h"

// -----------------------------------------------------------------------------
// --SECTION--                                              forward declarations
// -----------------------------------------------------------------------------

struct TRI_doc_mptr_t;
struct TRI_document_collection_t;
struct TRI_shaped_json_s;
struct TRI_transaction_collection_s;

////////////////////////////////////////////////////////////////////////////////
/// @brief index query parameter
////////////////////////////////////////////////////////////////////////////////

typedef struct TRI_index_search_value_s {
  size_t _length;
  struct TRI_shaped_json_s* _values;
}
TRI_index_search_value_t;


// -----------------------------------------------------------------------------
// --SECTION--                                              struct index_element
// -----------------------------------------------------------------------------


////////////////////////////////////////////////////////////////////////////////
/// @brief Unified index element. Do not directly construct it.
////////////////////////////////////////////////////////////////////////////////

struct TRI_index_element_t {
  private:
    TRI_doc_mptr_t* _document;

     // Do not use new for this struct, use ...
     TRI_index_element_t () {
     };

     ~TRI_index_element_t () = delete;

  public: 

////////////////////////////////////////////////////////////////////////////////
/// @brief Get a pointer to the Document's masterpointer.
////////////////////////////////////////////////////////////////////////////////

    TRI_doc_mptr_t* document () const {
      return _document;
    }
     
////////////////////////////////////////////////////////////////////////////////
/// @brief Set the pointer to the Document's masterpointer.
////////////////////////////////////////////////////////////////////////////////

    void document (TRI_doc_mptr_t* doc) {
      _document = doc;
    }

////////////////////////////////////////////////////////////////////////////////
/// @brief Get a pointer to sub objects
////////////////////////////////////////////////////////////////////////////////

    TRI_shaped_sub_t* subObjects () const {
      return reinterpret_cast<TRI_shaped_sub_t*>((char*) &_document + sizeof(TRI_doc_mptr_t*));
    }

////////////////////////////////////////////////////////////////////////////////
/// @brief Allocate a new index Element
////////////////////////////////////////////////////////////////////////////////

    static TRI_index_element_t* allocate (size_t numSubs, bool set) {
      void* space = TRI_Allocate(
        TRI_UNKNOWN_MEM_ZONE, sizeof(TRI_doc_mptr_t*) + (sizeof(TRI_shaped_sub_t) * numSubs), set
      );
      return new (space) TRI_index_element_t();
    }

////////////////////////////////////////////////////////////////////////////////
/// @brief Free the index element.
////////////////////////////////////////////////////////////////////////////////

    static void free (TRI_index_element_t* el) {
      TRI_ASSERT_EXPENSIVE(el != nullptr);
      TRI_ASSERT_EXPENSIVE(el->document() != nullptr);
      TRI_ASSERT_EXPENSIVE(el->subObjects() != nullptr);

      TRI_Free(TRI_UNKNOWN_MEM_ZONE, el);
    }

};

// -----------------------------------------------------------------------------
// --SECTION--                                                       class Index
// -----------------------------------------------------------------------------

namespace triagens {
  namespace arango {

    class Index {

// -----------------------------------------------------------------------------
// --SECTION--                                        constructors / destructors
// -----------------------------------------------------------------------------

      public:

        Index () = delete;
        Index (Index const&) = delete;
        Index& operator= (Index const&) = delete;

        Index (TRI_idx_iid_t,
               struct TRI_document_collection_t*,
               std::vector<std::vector<triagens::basics::AttributeName> >const&);

        virtual ~Index ();

      public:

////////////////////////////////////////////////////////////////////////////////
/// @brief index types
////////////////////////////////////////////////////////////////////////////////

        enum IndexType {
          TRI_IDX_TYPE_UNKNOWN = 0,
          TRI_IDX_TYPE_PRIMARY_INDEX,
          TRI_IDX_TYPE_GEO1_INDEX,
          TRI_IDX_TYPE_GEO2_INDEX,
          TRI_IDX_TYPE_HASH_INDEX,
          TRI_IDX_TYPE_EDGE_INDEX,
          TRI_IDX_TYPE_FULLTEXT_INDEX,
          TRI_IDX_TYPE_PRIORITY_QUEUE_INDEX, // DEPRECATED and not functional anymore
          TRI_IDX_TYPE_SKIPLIST_INDEX,
          TRI_IDX_TYPE_BITARRAY_INDEX,       // DEPRECATED and not functional anymore
          TRI_IDX_TYPE_CAP_CONSTRAINT
        };

// -----------------------------------------------------------------------------
// --SECTION--                                                    public methods
// -----------------------------------------------------------------------------

      public:

////////////////////////////////////////////////////////////////////////////////
/// @brief return the index id
////////////////////////////////////////////////////////////////////////////////
        
        inline TRI_idx_iid_t id () const {
          return _iid;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief return the index fields
////////////////////////////////////////////////////////////////////////////////

        inline std::vector<std::vector<triagens::basics::AttributeName>> const& fields () const {
          return _fields;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief return a contextual string for logging
////////////////////////////////////////////////////////////////////////////////

        std::string context () const;

////////////////////////////////////////////////////////////////////////////////
/// @brief return the name of the index
////////////////////////////////////////////////////////////////////////////////
        
        char const* typeName () const {
          return typeName(type());
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief return the index type based on a type name
////////////////////////////////////////////////////////////////////////////////

        static IndexType type (char const*);

////////////////////////////////////////////////////////////////////////////////
/// @brief return the name of an index type
////////////////////////////////////////////////////////////////////////////////

        static char const* typeName (IndexType);

////////////////////////////////////////////////////////////////////////////////
/// @brief validate an index id
////////////////////////////////////////////////////////////////////////////////

        static bool validateId (char const*);

////////////////////////////////////////////////////////////////////////////////
/// @brief validate an index handle (collection name + / + index id)
////////////////////////////////////////////////////////////////////////////////

        static bool validateHandle (char const*,
                                    size_t*);

////////////////////////////////////////////////////////////////////////////////
/// @brief generate a new index id
////////////////////////////////////////////////////////////////////////////////

        static TRI_idx_iid_t generateId ();

////////////////////////////////////////////////////////////////////////////////
/// @brief index comparator, used by the coordinator to detect if two index
/// contents are the same
////////////////////////////////////////////////////////////////////////////////

        static bool Compare (TRI_json_t const* lhs,
                             TRI_json_t const* rhs);

        virtual IndexType type () const = 0;
        virtual bool hasSelectivityEstimate () const = 0;
        virtual double selectivityEstimate () const;
        virtual size_t memory () const = 0;
        virtual triagens::basics::Json toJson (TRI_memory_zone_t*) const;
        virtual bool dumpFields () const = 0;
  
        virtual int insert (struct TRI_doc_mptr_t const*, bool) = 0;
        virtual int remove (struct TRI_doc_mptr_t const*, bool) = 0;
        virtual int postInsert (struct TRI_transaction_collection_s*, struct TRI_doc_mptr_t const*);
        virtual int batchInsert (std::vector<struct TRI_doc_mptr_t const*> const*, size_t);

        // a garbage collection function for the index
        virtual int cleanup ();

        // give index a hint about the expected size
        virtual int sizeHint (size_t);

        virtual bool hasBatchInsert () const;

        friend std::ostream& operator<< (std::ostream&, Index const*);
        friend std::ostream& operator<< (std::ostream&, Index const&);

// -----------------------------------------------------------------------------
// --SECTION--                                               protected variables
// -----------------------------------------------------------------------------

      protected:

        TRI_idx_iid_t const                                                    _iid;

        struct TRI_document_collection_t*                                      _collection;

        std::vector<std::vector<triagens::basics::AttributeName>> const        _fields;
               


// -----------------------------------------------------------------------------
// --SECTION--                                                 protected methods
// -----------------------------------------------------------------------------

      public:

////////////////////////////////////////////////////////////////////////////////
/// @brief helper function to insert a document into any index type
////////////////////////////////////////////////////////////////////////////////

        int fillElement(std::function<TRI_index_element_t* ()> allocate,
                        std::vector<TRI_index_element_t*>& elements,
                        TRI_doc_mptr_t const* document,
                        std::vector<TRI_shape_pid_t> const& paths,
                        bool const sparse);


        template<typename Idx_Element>
        // int fillElement(std::function<Idx_Element* ()> allocator,
        int fillElement2(Idx_Element* element,
                        TRI_shaped_sub_t* subObjects,
                        TRI_doc_mptr_t const* document,
                        std::vector<TRI_shape_pid_t> const& paths,
                        bool const sparse) {
          TRI_ASSERT(document != nullptr);
          TRI_ASSERT_EXPENSIVE(document->getDataPtr() != nullptr);   // ONLY IN INDEX, PROTECTED by RUNTIME

          TRI_shaped_json_t shapedJson;

          TRI_EXTRACT_SHAPED_JSON_MARKER(shapedJson, document->getDataPtr());  // ONLY IN INDEX, PROTECTED by RUNTIME

          if (shapedJson._sid == TRI_SHAPE_ILLEGAL) {
            LOG_WARNING("encountered invalid marker with shape id 0");

            return TRI_ERROR_INTERNAL;
          }

          int res = TRI_ERROR_NO_ERROR;

          element->_document = const_cast<TRI_doc_mptr_t*>(document);
          char const* ptr = element->_document->getShapedJsonPtr();  // ONLY IN INDEX, PROTECTED by RUNTIME

          size_t const n = paths.size();

          for (size_t j = 0; j < n; ++j) {
            TRI_shape_pid_t path = paths[j];

            // ..........................................................................
            // Determine if document has that particular shape
            // ..........................................................................

            TRI_shape_access_t const* acc = _collection->getShaper()->findAccessor(shapedJson._sid, path);  // ONLY IN INDEX, PROTECTED by RUNTIME

            if (acc == nullptr || acc->_resultSid == TRI_SHAPE_ILLEGAL) {
              // OK, the document does not contain the attributed needed by 
              // the index, are we sparse?
              subObjects[j]._sid = BasicShapes::TRI_SHAPE_SID_NULL;

              res = TRI_ERROR_ARANGO_INDEX_DOCUMENT_ATTRIBUTE_MISSING;

              if (sparse) {
                // no need to continue
                return res;
              }
              continue;
            }

            // ..........................................................................
            // Extract the field
            // ..........................................................................

            TRI_shaped_json_t shapedObject;
            if (! TRI_ExecuteShapeAccessor(acc, &shapedJson, &shapedObject)) {
              return TRI_ERROR_INTERNAL;
            }

            if (shapedObject._sid == BasicShapes::TRI_SHAPE_SID_NULL) {
              res = TRI_ERROR_ARANGO_INDEX_DOCUMENT_ATTRIBUTE_MISSING;

              if (sparse) {
                // no need to continue
                return res;
              }
            }

            // .........................................................................
            // Store the field
            // .........................................................................

            TRI_FillShapedSub(&subObjects[j], &shapedObject, ptr);
          }

          return res;
        }
    };

  }
}

#endif

// -----------------------------------------------------------------------------
// --SECTION--                                                       END-OF-FILE
// -----------------------------------------------------------------------------

// Local Variables:
// mode: outline-minor
// outline-regexp: "/// @brief\\|/// {@inheritDoc}\\|/// @page\\|// --SECTION--\\|/// @\\}"
// End:
