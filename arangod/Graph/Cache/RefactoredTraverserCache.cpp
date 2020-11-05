////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2020 ArangoDB GmbH, Cologne, Germany
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
////////////////////////////////////////////////////////////////////////////////

#include "RefactoredTraverserCache.h"

#include "Aql/AqlValue.h"
#include "Aql/Query.h"
#include "Basics/StringHeap.h"
#include "Basics/VelocyPackHelper.h"
#include "Cluster/ServerState.h"
#include "Graph/BaseOptions.h"
#include "Graph/EdgeDocumentToken.h"
#include "Logger/LogMacros.h"
#include "Logger/Logger.h"
#include "Logger/LoggerStream.h"
#include "Transaction/Methods.h"
#include "VocBase/LogicalCollection.h"
#include "VocBase/ManagedDocumentResult.h"

#include <velocypack/Builder.h>
#include <velocypack/HashedStringRef.h>
#include <velocypack/Slice.h>
#include <velocypack/StringRef.h>
#include <velocypack/velocypack-aliases.h>

using namespace arangodb;
using namespace arangodb::graph;

RefactoredTraverserCache::RefactoredTraverserCache(arangodb::transaction::Methods* trx,
                                                   aql::QueryContext* query)
    : _query(query),
      _trx(trx),
      _insertedDocuments(0),
      _filteredDocuments(0),
      _stringHeap(4096) /* arbitrary block-size may be adjusted for performance */
{}

RefactoredTraverserCache::~RefactoredTraverserCache() = default;

void RefactoredTraverserCache::clear() {
  _stringHeap.clear();
  _persistedStrings.clear();
  _mmdr.clear();
}

VPackSlice RefactoredTraverserCache::lookupToken(EdgeDocumentToken const& idToken) {
  TRI_ASSERT(!ServerState::instance()->isCoordinator());
  auto col = _trx->vocbase().lookupCollection(idToken.cid());

  if (col == nullptr) {
    // collection gone... should not happen
    LOG_TOPIC("3b2ba", ERR, arangodb::Logger::GRAPHS)
        << "Could not extract indexed edge document. collection not found";
    TRI_ASSERT(col != nullptr);  // for maintainer mode
    return arangodb::velocypack::Slice::nullSlice();
  }

  if (!col->readDocument(_trx, idToken.localDocumentId(), _mmdr)) {
    // We already had this token, inconsistent state. Return NULL in Production
    LOG_TOPIC("3acb3", ERR, arangodb::Logger::GRAPHS)
        << "Could not extract indexed edge document, return 'null' instead. "
        << "This is most likely a caching issue. Try: 'db." << col->name()
        << ".unload(); db." << col->name() << ".load()' in arangosh to fix this.";
    TRI_ASSERT(false);  // for maintainer mode
    return arangodb::velocypack::Slice::nullSlice();
  }

  return VPackSlice(_mmdr.vpack());
}

VPackSlice RefactoredTraverserCache::lookupVertexInCollection(arangodb::velocypack::StringRef id) {
  // TODO: Missing produceVertices check
  /*if (!_baseOptions->produceVertices()) {
    // this traversal does not produce any vertices
    return arangodb::velocypack::Slice::nullSlice();
  }*/

  // TRI_ASSERT(!ServerState::instance()->isCoordinator()); // TODO: if we're just setting this up for singleserver we can enable that assert
  size_t pos = id.find('/');
  if (pos == std::string::npos || pos + 1 == id.size()) {
    // Invalid input. If we get here somehow we managed to store invalid
    // _from/_to values or the traverser did a let an illegal start through
    TRI_ASSERT(false);  // for maintainer mode
    return arangodb::velocypack::Slice::nullSlice();
  }

  std::string collectionName = id.substr(0, pos).toString();

  /* TODO: See todo above, we could just get rid of this in standalone (e.g. just being used in SingleServerProdiver as Cache)
  auto const& map = _baseOptions->collectionToShard();
  if (!map.empty()) {
    auto found = map.find(collectionName);
    if (found != map.end()) {
      collectionName = found->second;
    }
  }*/

  try {
    Result res = _trx->documentFastPathLocal(collectionName, id.substr(pos + 1), _mmdr);
    if (res.ok()) {
      ++_insertedDocuments;
      return VPackSlice(_mmdr.vpack());
    }

    if (!res.is(TRI_ERROR_ARANGO_DOCUMENT_NOT_FOUND)) {
      // ok we are in a rather bad state. Better throw and abort.
      THROW_ARANGO_EXCEPTION(res);
    }
  } catch (basics::Exception const& ex) {
    // TODO: see todo above, same issue
    if (ServerState::instance()->isDBServer()) {
      // on a DB server, we could have got here only in the OneShard case.
      // in this case turn the rather misleading "collection or view not found"
      // error into a nicer "collection not known to traversal, please add WITH"
      // message, so users know what to do
      if (ex.code() == TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND) {
        THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_QUERY_COLLECTION_LOCK_FAILED,
                                       "collection not known to traversal: '" + collectionName +
                                           "'. please add 'WITH " + collectionName +
                                           "' as the first line in your AQL");
      }
    }

    throw;
  }

  ++_insertedDocuments;

  // Register a warning. It is okay though but helps the user
  std::string msg = "vertex '" + id.toString() + "' not found";
  _query->warnings().registerWarning(TRI_ERROR_ARANGO_DOCUMENT_NOT_FOUND, msg.c_str()); // TODO CHECK: Query only needed for warnings?!
  // This is expected, we may have dangling edges. Interpret as NULL
  return arangodb::velocypack::Slice::nullSlice();
}

void RefactoredTraverserCache::insertEdgeIntoResult(EdgeDocumentToken const& idToken,
                                                    VPackBuilder& builder) {
  TRI_ASSERT(!ServerState::instance()->isCoordinator());
  builder.add(lookupToken(idToken));
}

void RefactoredTraverserCache::insertVertexIntoResult(arangodb::velocypack::StringRef idString,
                                                      VPackBuilder& builder) {
  builder.add(lookupVertexInCollection(idString));
}

aql::AqlValue RefactoredTraverserCache::fetchEdgeAqlResult(EdgeDocumentToken const& idToken) {
  TRI_ASSERT(!ServerState::instance()->isCoordinator());
  return aql::AqlValue(lookupToken(idToken));
}

aql::AqlValue RefactoredTraverserCache::fetchVertexAqlResult(arangodb::velocypack::StringRef idString) {
  return aql::AqlValue(lookupVertexInCollection(idString));
}

arangodb::velocypack::StringRef RefactoredTraverserCache::persistString(arangodb::velocypack::StringRef idString) {
  arangodb::velocypack::HashedStringRef hsr(idString.data(),
                                            static_cast<uint32_t>(idString.size()));

  auto it = _persistedStrings.find(hsr);
  if (it != _persistedStrings.end()) {
    return (*it).stringRef();
  }
  auto res = _stringHeap.registerString(hsr);
  _persistedStrings.emplace(res);
  // convert to simple StringRef here, as caller is not prepared to receive a HashedStringRef
  return res.stringRef();
}

arangodb::velocypack::HashedStringRef RefactoredTraverserCache::persistString(
    arangodb::velocypack::HashedStringRef idString) {
  auto it = _persistedStrings.find(idString);
  if (it != _persistedStrings.end()) {
    return *it;
  }
  auto res = _stringHeap.registerString(idString);
  _persistedStrings.emplace(res);
  return res;
}
