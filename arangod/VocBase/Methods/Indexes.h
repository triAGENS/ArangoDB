////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2024 ArangoDB GmbH, Cologne, Germany
/// Copyright 2004-2014 triAGENS GmbH, Cologne, Germany
///
/// Licensed under the Business Source License 1.1 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     https://github.com/arangodb/arangodb/blob/devel/LICENSE
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is ArangoDB GmbH, Cologne, Germany
///
/// @author Simon Grätzer
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include <velocypack/Builder.h>
#include <velocypack/Slice.h>

#include "Basics/Result.h"
#include "Futures/Future.h"
#include "Indexes/Index.h"
#include "Replication2/StateMachines/Document/CreateIndexReplicationCallback.h"
#include "Transaction/Hints.h"
#include "VocBase/Identifiers/IndexId.h"
#include "VocBase/voc-types.h"

#include <function2.hpp>

struct TRI_vocbase_t;

namespace arangodb {
namespace futures {
template<typename T>
class Future;
}
class LogicalCollection;
class CollectionNameResolver;
class SingleCollectionTransaction;
namespace methods {

/// Common code for ensureIndexes and api-index.js
struct Indexes {
  using ProgressTracker = std::function<arangodb::Result(double)>;

  static futures::Future<arangodb::Result> getIndex(
      LogicalCollection const& collection, velocypack::Slice indexId,
      velocypack::Builder& out, transaction::Methods* trx = nullptr);

  /// @brief get all indexes, skips view links
  static futures::Future<arangodb::Result> getAll(
      LogicalCollection const& collection,
      std::underlying_type<Index::Serialize>::type, bool withHidden,
      arangodb::velocypack::Builder&, transaction::Methods* trx = nullptr);

  static futures::Future<arangodb::Result> createIndex(
      LogicalCollection&, Index::IndexType, std::vector<std::string> const&,
      bool unique, bool sparse, bool estimates);

  static futures::Future<arangodb::Result> ensureIndex(
      LogicalCollection& collection, velocypack::Slice definition, bool create,
      velocypack::Builder& output, std::shared_ptr<ProgressTracker> f = nullptr,
      replication2::replicated_state::document::Replication2Callback
          replicationCb = nullptr);

  static futures::Future<arangodb::Result> drop(LogicalCollection& collection,
                                                velocypack::Slice indexArg);
  static futures::Future<arangodb::Result> drop(LogicalCollection& collection,
                                                IndexId indexId);

  template<typename IndexSpec>
  requires std::is_same_v<IndexSpec, IndexId> or
      std::is_same_v<IndexSpec, velocypack::Slice>
  static futures::Future<arangodb::Result> dropDBServer(
      LogicalCollection& collection, IndexSpec indexSpec);
  static futures::Future<arangodb::Result> dropCoordinator(
      LogicalCollection& collection, IndexId indexId);

  static std::unique_ptr<SingleCollectionTransaction> createTrxForDrop(
      LogicalCollection& collection);

  static arangodb::Result extractHandle(LogicalCollection const& collection,
                                        CollectionNameResolver const* resolver,
                                        velocypack::Slice const& val,
                                        IndexId& iid, std::string& name);

 private:
  static arangodb::Result ensureIndexCoordinator(
      LogicalCollection const& collection, velocypack::Slice indexDef,
      bool create, velocypack::Builder& resultBuilder);

#ifdef USE_ENTERPRISE
  static arangodb::Result ensureIndexCoordinatorEE(
      arangodb::LogicalCollection const& collection,
      arangodb::velocypack::Slice slice, bool create,
      arangodb::velocypack::Builder& resultBuilder);
  static arangodb::Result dropCoordinatorEE(
      arangodb::LogicalCollection const& collection, IndexId iid);
#endif
};
}  // namespace methods
}  // namespace arangodb
