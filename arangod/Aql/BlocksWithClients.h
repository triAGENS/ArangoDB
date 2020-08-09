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
/// @author Max Neunhoeffer
////////////////////////////////////////////////////////////////////////////////

#ifndef ARANGOD_AQL_CLUSTER_BLOCKS_H
#define ARANGOD_AQL_CLUSTER_BLOCKS_H 1

#include "Aql/ClusterNodes.h"
#include "Aql/ExecutionBlock.h"
#include "Aql/ExecutionState.h"
#include "Aql/RegisterInfos.h"
#include "Basics/Result.h"

#include <velocypack/Builder.h>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace arangodb {

namespace transaction {
class Methods;
}

namespace aql {
class AqlItemBlock;
struct Collection;
class ExecutionEngine;
class ExecutionNode;
class SkipResult;

class ClientsExecutorInfos {
 public:
  explicit ClientsExecutorInfos(std::vector<std::string> clientIds);

  ClientsExecutorInfos(ClientsExecutorInfos&&) = default;
  ClientsExecutorInfos(ClientsExecutorInfos const&) = delete;
  ~ClientsExecutorInfos() = default;

  auto nrClients() const noexcept -> size_t;
  auto clientIds() const noexcept -> std::vector<std::string> const&;

 private:
  std::vector<std::string> const _clientIds;
};

class BlocksWithClients {
 public:
  virtual ~BlocksWithClients() = default;

  /// @brief getSomeForShard
  /// @deprecated
  virtual std::pair<ExecutionState, SharedAqlItemBlockPtr> getSomeForShard(
      size_t atMost, std::string const& shardId) = 0;

  /// @brief skipSomeForShard
  /// @deprecated
  virtual std::pair<ExecutionState, size_t> skipSomeForShard(size_t atMost,
                                                             std::string const& shardId) = 0;

  /**
   * @brief Execute for client.
   *  Like execute, but bound to the dataset, that needs to be send to the given client ID
   *
   * @param stack The AqlCallStack
   * @param clientId The requesting client Id.
   * @return std::tuple<ExecutionState, size_t, SharedAqlItemBlockPtr>
   */
  virtual auto executeForClient(AqlCallStack const& stack, AqlCallList clientCall, std::string const& clientId)
      -> std::tuple<ExecutionState, SkipResult, SharedAqlItemBlockPtr> = 0;
};

/**
 * @brief Implementation of an ExecutionBlock that has multiple clients
 *        Data is distributed to those clients, it might be all (Scatter)
 *        or a selected part of it (Distribute).
 *        How data is distributed is defined by the template parameter.
 *
 * @tparam ClientBlockData needs to be able to hold the data to be distributed
 *         to a single client.
 *         It needs to implement the following methods:
 *         canProduce(size_t limit) -> bool stating it has enough information to fill limit many rows (or more)
 *
 */

template <class Executor>
class BlocksWithClientsImpl : public ExecutionBlock, public BlocksWithClients {
  using ExecutorInfos = typename Executor::Infos;

 public:
  BlocksWithClientsImpl(ExecutionEngine* engine, ExecutionNode const* ep,
                        RegisterInfos registerInfos, typename Executor::Infos executorInfos);

  ~BlocksWithClientsImpl() override = default;

 public:
  /// @brief initializeCursor
  auto initializeCursor(InputAqlItemRow const& input)
      -> std::pair<ExecutionState, Result> override;

  /// @brief shutdown
  std::pair<ExecutionState, Result> shutdown(int) override;

  /// @brief execute: shouldn't be used, use executeForClient
  std::tuple<ExecutionState, SkipResult, SharedAqlItemBlockPtr> execute(AqlCallStack const& stack, AqlCallList clientCall) override;

  /**
   * @brief Execute for client.
   *  Like execute, but bound to the dataset, that needs to be send to the given client ID
   *
   * @param stack The AqlCallStack
   * @param clientId The requesting client Id.
   * @return std::tuple<ExecutionState, size_t, SharedAqlItemBlockPtr>
   */
  auto executeForClient(AqlCallStack const& stack, AqlCallList clientCall, std::string const& clientId)
      -> std::tuple<ExecutionState, SkipResult, SharedAqlItemBlockPtr> override;

 private:
  /**
   * @brief Actual implementation of Execute.
   *
   * @param stack The AqlCallStack
   * @param clientId The requesting client Id.
   * @return std::tuple<ExecutionState, size_t, SharedAqlItemBlockPtr>
   */
  auto executeWithoutTraceForClient(AqlCallStack const& stack, AqlCallList clientCall, std::string const& clientId)
      -> std::tuple<ExecutionState, SkipResult, SharedAqlItemBlockPtr>;

  /**
   * @brief Load more data from upstream and distribute it into _clientBlockData
   *
   */
  auto fetchMore(AqlCallStack const& stack) -> ExecutionState;

  /// @brief getSomeForShard
  /// @deprecated
  std::pair<ExecutionState, SharedAqlItemBlockPtr> getSomeForShard(size_t atMost,
                                                                   std::string const& shardId) override;

  /// @brief skipSomeForShard
  /// @deprecated
  std::pair<ExecutionState, size_t> skipSomeForShard(size_t atMost,
                                                     std::string const& shardId) override;

 protected:
  /// @brief getClientId: get the number <clientId> (used internally)
  /// corresponding to <shardId>
  size_t getClientId(std::string const& shardId) const;

  /// @brief _shardIdMap: map from shardIds to clientNrs
  /// @deprecated
  std::unordered_map<std::string, size_t> _shardIdMap;

  /// @brief _nrClients: total number of clients
  size_t _nrClients;

  /// @brief type of distribution that this nodes follows.
  ScatterNode::ScatterType _type;

 private:
  RegisterInfos _registerInfos;

  /**
   * @brief This is the working party of this implementation
   *        the template class needs to implement the logic
   *        to produce a single row from the upstream information.
   */
  ExecutorInfos _executorInfos;

  Executor _executor;

  /// @brief A map of clientId to the data this client should receive.
  ///        This map will be filled as the execution progresses.
  std::unordered_map<std::string, typename Executor::ClientBlockData> _clientBlockData;

  bool _wasShutdown;
};

}  // namespace aql
}  // namespace arangodb

#endif
