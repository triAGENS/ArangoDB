////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2019 ArangoDB GmbH, Cologne, Germany
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

#ifndef ARANGOD_AQL_DISTRIBUTE_CLIENT_BLOCK_H
#define ARANGOD_AQL_DISTRIBUTE_CLIENT_BLOCK_H

#include "Aql/SharedAqlItemBlockPtr.h"
#include "Aql/SkipResult.h"

#include <deque>

namespace arangodb {
namespace aql {

struct AqlCall;
class AqlCallList;
class AqlCallStack;
enum class ExecutionState;
class ExecutionBlock;
class ExecutionEngine;
class ExecutionNode;
class RegisterInfos;

class DistributeClientBlock {
 private:
  class QueueEntry {
   public:
    QueueEntry(SkipResult const& skipped, SharedAqlItemBlockPtr block,
               std::vector<size_t> chosen);

    auto numRows() const -> size_t;

    auto skipResult() const -> SkipResult const&;

    auto block() const -> SharedAqlItemBlockPtr const&;

    auto chosen() const -> std::vector<size_t> const&;

   private:
    SkipResult _skip;
    SharedAqlItemBlockPtr _block;
    std::vector<size_t> _chosen;
  };

 public:
  DistributeClientBlock(ExecutionEngine& engine, ExecutionNode const* node,
                        RegisterInfos const& registerInfos);

  auto clear() -> void;
  auto addBlock(SkipResult const& skipResult, SharedAqlItemBlockPtr block,
                std::vector<size_t> usedIndexes) -> void;

  auto hasDataFor(AqlCall const& call) -> bool;

  auto execute(AqlCallStack const& callStack, AqlCallList clientCall, ExecutionState upstreamState)
      -> std::tuple<ExecutionState, SkipResult, SharedAqlItemBlockPtr>;

 private:
  /**
   * @brief This call will join as many blocks as available from the queue
   *        and return them in a SingleBlock. We then use the IdExecutor
   *        to hand out the data contained in these blocks
   *        We do on purpose not give any kind of guarantees on the sizing
   * of this block to be flexible with the implementation, and find a good
   *        trade-off between blocksize and block copy operations.
   *
   * @return SharedAqlItemBlockPtr a joined block from the queue.
   *         SkipResult the skip information matching to this block
   */
  auto popJoinedBlock() -> std::tuple<SharedAqlItemBlockPtr, SkipResult>;

 private:
  AqlItemBlockManager& _blockManager;
  RegisterInfos const& registerInfos;

  std::deque<QueueEntry> _queue;

  // This is unique_ptr to get away with everything being forward declared...
  std::unique_ptr<ExecutionBlock> _executor;
  bool _executorHasMore = false;
};

}  // namespace aql
}  // namespace arangodb

#endif  // ARANGOD_AQL_DISTRIBUTE_CLIENT_BLOCK_H
