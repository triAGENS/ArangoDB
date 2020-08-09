////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2018 ArangoDB GmbH, Cologne, Germany
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

#include "MultiDependencySingleRowFetcher.h"

#include "Aql/AqlItemBlock.h"
#include "Aql/DependencyProxy.h"
#include "Aql/ShadowAqlItemRow.h"
#include "Logger/LogMacros.h"
#include "Transaction/Context.h"
#include "Transaction/Methods.h"

#include <velocypack/Builder.h>
#include <velocypack/velocypack-aliases.h>

using namespace arangodb;
using namespace arangodb::aql;

MultiDependencySingleRowFetcher::UpstreamSkipReport::UpstreamSkipReport() {}

auto MultiDependencySingleRowFetcher::UpstreamSkipReport::isInitialized() const -> bool {
  return _isInitialized;
}

auto MultiDependencySingleRowFetcher::UpstreamSkipReport::initialize(size_t depth) -> void {
  _report.resize(depth, std::make_pair(0,0));
  _isInitialized = true;
}

auto MultiDependencySingleRowFetcher::UpstreamSkipReport::getSkipped(size_t subqueryDepth) const -> size_t {
  TRI_ASSERT(_isInitialized);
  TRI_ASSERT(subqueryDepth < _report.size());
  return _report[subqueryDepth].first;
}

auto MultiDependencySingleRowFetcher::UpstreamSkipReport::getFullCount(size_t subqueryDepth) const -> size_t {
  TRI_ASSERT(_isInitialized);
  TRI_ASSERT(subqueryDepth < _report.size());
  return _report[subqueryDepth].second;
}

auto MultiDependencySingleRowFetcher::UpstreamSkipReport::clearCounts(size_t subqueryDepth)
    -> void {
  for (size_t i = 0; i <= subqueryDepth; ++i) {
    // We can reset all counters of lower depth
    _report[i] = std::make_pair(0, 0);
  }
}

auto MultiDependencySingleRowFetcher::UpstreamSkipReport::setSkipped(size_t subqueryDepth, size_t skipped) -> void {
  TRI_ASSERT(subqueryDepth < _report.size());
  _report[subqueryDepth].first = skipped;
}

auto MultiDependencySingleRowFetcher::UpstreamSkipReport::setFullCount(size_t subqueryDepth,
                                                                       size_t skipped)
    -> void {
  TRI_ASSERT(subqueryDepth < _report.size());
  _report[subqueryDepth].second = skipped;
}

MultiDependencySingleRowFetcher::DependencyInfo::DependencyInfo()
    : _upstreamState{ExecutionState::HASMORE}, _currentBlock{nullptr}, _rowIndex{0} {}

MultiDependencySingleRowFetcher::MultiDependencySingleRowFetcher(
    DependencyProxy<BlockPassthrough::Disable>& executionBlock)
    : _dependencyProxy(&executionBlock) {}

MultiDependencySingleRowFetcher::MultiDependencySingleRowFetcher()
    : _dependencyProxy(nullptr) {}

RegisterCount MultiDependencySingleRowFetcher::getNrInputRegisters() const {
  return _dependencyProxy->getNrInputRegisters();
}

void MultiDependencySingleRowFetcher::initDependencies() {
  // Need to setup the dependencies, they are injected lazily.
  TRI_ASSERT(_dependencyProxy->numberDependencies() > 0);
  TRI_ASSERT(_dependencyInfos.empty());
  _dependencyInfos.reserve(_dependencyProxy->numberDependencies());
  for (size_t i = 0; i < _dependencyProxy->numberDependencies(); ++i) {
    _dependencyInfos.emplace_back(DependencyInfo{});
  }
  _dependencyStates.reserve(_dependencyProxy->numberDependencies());
  for (size_t i = 0; i < _dependencyProxy->numberDependencies(); ++i) {
    _dependencyStates.emplace_back(ExecutionState::HASMORE);
  }
}

size_t MultiDependencySingleRowFetcher::numberDependencies() {
  return _dependencyInfos.size();
}

void MultiDependencySingleRowFetcher::init() {
  TRI_ASSERT(_dependencyInfos.empty());
  initDependencies();
  _callsInFlight.resize(numberDependencies());
  _dependencySkipReports.resize(numberDependencies());
}

bool MultiDependencySingleRowFetcher::indexIsValid(
    const MultiDependencySingleRowFetcher::DependencyInfo& info) const {
  return info._currentBlock != nullptr && info._rowIndex < info._currentBlock->size();
}

bool MultiDependencySingleRowFetcher::isDone(
    const MultiDependencySingleRowFetcher::DependencyInfo& info) const {
  return info._upstreamState == ExecutionState::DONE;
}

auto MultiDependencySingleRowFetcher::executeForDependency(size_t const dependency,
                                                           AqlCallStack const& stack,
                                                           AqlCallList clientCall)
    -> std::tuple<ExecutionState, SkipResult, AqlItemBlockInputRange> {
      // TODO:MCHACKI canwe move here?
  auto [state, skipped, block] = _dependencyProxy->executeForDependency(dependency, stack, clientCall);

  if (state == ExecutionState::WAITING) {
    TRI_ASSERT(skipped.nothingSkipped());
    return {state, SkipResult{}, AqlItemBlockInputRange{ExecutorState::HASMORE}};
  }
//TODO:MCHACKI is this still ok?
  reportSkipForDependency(stack, skipped, dependency);

  ExecutorState execState =
      state == ExecutionState::DONE ? ExecutorState::DONE : ExecutorState::HASMORE;

  _dependencyStates.at(dependency) = state;

  if (block == nullptr) {
    return {state, skipped, AqlItemBlockInputRange{execState, skipped.getSkipCount()}};
  }
  TRI_ASSERT(block != nullptr);
  auto [start, end] = block->getRelevantRange();
  return {state, skipped,
          AqlItemBlockInputRange{execState, skipped.getSkipCount(), block, start}};
}

auto MultiDependencySingleRowFetcher::execute(AqlCallStack const& stack,
                                              AqlCallSet const& aqlCallSet)
    -> std::tuple<ExecutionState, SkipResult, std::vector<std::pair<size_t, AqlItemBlockInputRange>>> {
  TRI_ASSERT(_callsInFlight.size() == numberDependencies());
  if (!_maximumSkipReport.isInitialized()) {
    size_t levels = stack.subqueryLevel();
    _maximumSkipReport.initialize(levels);
    for (auto& depRep : _dependencySkipReports) {
      depRep.initialize(levels);
    }
  }

  auto ranges = std::vector<std::pair<size_t, AqlItemBlockInputRange>>{};
  ranges.reserve(aqlCallSet.size());

  auto depCallIdx = size_t{0};
  auto allAskedDepsAreWaiting = true;
  auto askedAtLeastOneDep = false;
  auto skippedTotal = SkipResult{};
  // Iterate in parallel over `_callsInFlight` and `aqlCall.calls`.
  // _callsInFlight[i] corresponds to aqlCalls.calls[k] iff
  // aqlCalls.calls[k].dependency = i.
  // So there is not always a matching entry in aqlCall.calls.
  for (auto dependency = size_t{0}; dependency < _callsInFlight.size(); ++dependency) {
    auto& maybeCallInFlight = _callsInFlight[dependency];

    // See if there is an entry for `dependency` in `aqlCall.calls`
    if (depCallIdx < aqlCallSet.calls.size() &&
        aqlCallSet.calls[depCallIdx].dependency == dependency) {
      // If there is a call in flight, we *must not* change the call,
      // no matter what we got. Otherwise, we save the current call.
      if (!maybeCallInFlight.has_value()) {
        auto depStack = adjustStackWithSkipReport(stack, dependency);

        maybeCallInFlight = std::make_pair(depStack, aqlCallSet.calls[depCallIdx].call);
      }
      ++depCallIdx;
      if (depCallIdx < aqlCallSet.calls.size()) {
        TRI_ASSERT(aqlCallSet.calls[depCallIdx - 1].dependency <
                   aqlCallSet.calls[depCallIdx].dependency);
      }
    }

    if (maybeCallInFlight.has_value()) {
      // We either need to make a new call, or check whether we got a result
      // for a call in flight.
      auto& [stackInFlight, clientCallInFlight] = maybeCallInFlight.value();
      auto [state, skipped, range] = executeForDependency(dependency, stackInFlight, clientCallInFlight);
      askedAtLeastOneDep = true;
      if (state != ExecutionState::WAITING) {
        // Got a result, call is no longer in flight
        maybeCallInFlight = std::nullopt;
        allAskedDepsAreWaiting = false;

        // NOTE:
        // in this fetcher case we do not have and do not want to have
        // any control of the order the upstream responses are entering.
        // Every of the upstream response will contain an identical skipped
        // stack on the subqueries.
        // We only need to forward the skipping of any one of those.
        // So we implemented the following logic to return the skip
        // information for the first on that arrives and all other
        // subquery skip informations will be discarded.
        if (!_didReturnSubquerySkips) {
          // We have nothing skipped locally.
          TRI_ASSERT(skippedTotal.subqueryDepth() == 1);
          TRI_ASSERT(skippedTotal.getSkipCount() == 0);

          // We forward the skip block as is.
          // This will also include the skips on subquery level
          skippedTotal = skipped;
          // Do this only once.
          // The first response will contain the amount of rows skipped
          // in subquery
          _didReturnSubquerySkips = true;
        } else {
          // We only need the skip amount on the top level.
          // Another dependency has forwarded the subquery level skips
          // already
          skippedTotal.mergeOnlyTopLevel(skipped);
        }

      } else {
        TRI_ASSERT(skipped.nothingSkipped());
      }

      ranges.emplace_back(dependency, range);
    }
  }

  auto const state = std::invoke([&]() {
    if (askedAtLeastOneDep && allAskedDepsAreWaiting) {
      TRI_ASSERT(skippedTotal.nothingSkipped());
      return ExecutionState::WAITING;
    } else {
      return upstreamState();
    }
  });

  return {state, skippedTotal, ranges};
}

auto MultiDependencySingleRowFetcher::upstreamState() const -> ExecutionState {
  if (std::any_of(std::begin(_dependencyStates), std::end(_dependencyStates),
                  [](ExecutionState const s) {
                    return s == ExecutionState::HASMORE;
                  })) {
    return ExecutionState::HASMORE;
  } else {
    return ExecutionState::DONE;
  }
}


auto MultiDependencySingleRowFetcher::resetDidReturnSubquerySkips(size_t shadowRowDepth) -> void {
  _didReturnSubquerySkips = false;

#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
  // Validate that the skip number is identical in all branches
  for (size_t i = 0; i < shadowRowDepth; ++i) {
    auto const reportedSkip = _maximumSkipReport.getSkipped(i);
    auto const reportedFullCount = _maximumSkipReport.getFullCount(i);
    for (auto const& rep : _dependencySkipReports) {
      TRI_ASSERT(rep.getSkipped(i) == reportedSkip);
      TRI_ASSERT(rep.getFullCount(i) == reportedFullCount);
    }
  }
#endif

  _maximumSkipReport.clearCounts(shadowRowDepth);
  for (auto& rep : _dependencySkipReports) {
    rep.clearCounts(shadowRowDepth);
  }
}

AqlCallStack MultiDependencySingleRowFetcher::adjustStackWithSkipReport(
    AqlCallStack const& callStack, const size_t dependency) {
  // Copy the original
  AqlCallStack stack = callStack;
  TRI_ASSERT(dependency < _dependencySkipReports.size());

  auto const localReport = _dependencySkipReports[dependency];
  for (size_t i = 0; i < stack.subqueryLevel(); ++i) {
    /*
     * Here we need to adjust the inbound call.
     * We have the following situation:
     * The client calls once with a stack, to any branch
     * This branch will report skipping on outer queries
     * (e.g. the main query)
     * From this point on the client will not ask again to
     * skip the very same rows.
     * However, we need to ensure that the original information
     * is passed on into every branch.
     *
     * Therefore we define the following values:
     * - originalCall := The original call send by client, unmodified of any skip
     * - clientCall := The call send by the client now. (Modified by all rows counted as skipped)
     * - branchSkip := The amount of skipped rows within the branch
     * - reportedSkip := The amount of skipped rows reported to the Client thus far, the maximum of all branchSkips
     *
     * Given the above definitions we can deduce:
     * - originalCall.offset <=> clientCall.offset + reportedSkip
     *
     * The amount of rows need to skip within this branch is given by:
     * - newCall.offset := originalCall.offset - branchSkip
     *     = clientCall.offset + reportedSkip - branchSkip
     */
    TRI_ASSERT(localReport.getSkipped(i) <= _maximumSkipReport.getSkipped(i));
    // diff := reportedSkip  - branchSkip;
    size_t diff = _maximumSkipReport.getSkipped(i) - localReport.getSkipped(i);
    if (diff > 0) {
      // We have not yet reported this skip over the given dependency
      // So ask for it.
      auto& call = stack.modifyCallAtDepth(i);
      call.offset += diff;
    }
  }
  return stack;
}

void MultiDependencySingleRowFetcher::reportSkipForDependency(AqlCallStack const& originalStack,
                                                              SkipResult const& skipRes,
                                                              const size_t dependency) {
  if (skipRes.nothingSkipped()) {
    // Nothing to report
    return;
  }
  TRI_ASSERT(dependency < _dependencySkipReports.size());

  auto& branchReport = _dependencySkipReports[dependency];
  // Skip and Stack contain the current Subquery Execution.
  // So we need to increase the level on those by one.
  for (size_t level = 1; level < originalStack.subqueryLevel(); ++level) {
    /*
     * This code part is a bit tricky.
     * We start with the following values:
     * - originalCall := The original call send by client, unmodified of any skip seen thus far
     * - call := The resulting call, if we remove the already skipped values from originalCall.
     * - reportedSkip := The last skipValue reported to client == maximum of skipped in every branch, at most originalCall.offset
     * - reportedFullCount := The last fullCount value reported to client == max(maximum of skipped in every branch - originalCall.offset , 0)
     * - branchSkip := The skipValue seen in this branch, at most originalCall.offset
     * - branchFullCount := The overflow of skipValle - originalCall.offset in this branch.
     * - skipped := Amount of rows skipped in last upstream execute
     *
     * We have the following invariants:
     * - originalCall.offset - branchSkip == call.offset
     * - reportedSkip <= originalCall.offset
     * - branchSkip <= reportedSkip
     * - branchFullCount <= reportedFullCount
     * - branchFullCount > 0 -> call.fullCount == true
     *
     * We now apply the returned `skipped` on the counter to the local branch.
     * We also need to adapt the reported values, as they are defined as
     * the maximum of all branches.
     *
     * So we need to apply the following transformations:
     * - branchSkip' := max(originalCall.offset, branchSkip + skipped)
     *               <=> max(call.offset + branchSkip, branchSkip + skipped)
     *               <=> branchSkip + max(call.offset, skipped)
     * - branchFullCount' := max(0, branchFullCount + branchSkip + skipped - branchSkip')
     * - reportedSkip' := max(reportedSkip, branchSkip')
     * - reportedFullCount' := max(reportedFullCount, branchFullCount')
     */
    auto const& skipped = skipRes.getSkipOnSubqueryLevel(level);
    if (skipped > 0) {
      // Need to report
      auto const& call = originalStack.getCallAtDepth(level);
      auto const reportLevel = level - 1;
      auto const reportedSkip = _maximumSkipReport.getSkipped(reportLevel);
      auto const reportedFullCount = _maximumSkipReport.getFullCount(reportLevel);

      auto const branchSkip = branchReport.getSkipped(reportLevel);
      auto const branchFullCount = branchReport.getFullCount(reportLevel);

      auto const branchSkipNext = branchSkip + (std::max)(call.getOffset(), skipped);
      branchReport.setSkipped(reportLevel, branchSkipNext);

      if (branchSkipNext > reportedSkip) {
        _maximumSkipReport.setSkipped(reportLevel, branchSkipNext);
      }

      if (skipped > call.getOffset()) {
        // In the current implementation we get the fullCount guaranteed in
        // one go. If this assert triggers, we can easily transform the following code
        // into an increment of branchFullCount instead of assignement
        TRI_ASSERT(branchFullCount == 0);
        // If this assert kicks in, the counters are off, too many rows are skipped
        TRI_ASSERT(call.needsFullCount());
        auto const branchFullCountNext = skipped - call.getOffset();

        branchReport.setFullCount(reportLevel, branchFullCountNext);
        if (reportedFullCount < branchFullCountNext) {
          // We can only have one fullCount value.
          TRI_ASSERT(reportedFullCount == 0);
          _maximumSkipReport.setFullCount(reportLevel, branchFullCountNext);
        } else {
          // WE cannot have different fullCounts on different Branches
          TRI_ASSERT(reportedFullCount == branchFullCountNext);
        }
      }
      // _maximumReport needs to contain maximum values
      TRI_ASSERT(_maximumSkipReport.getSkipped(reportLevel) ==
                 std::max_element(_dependencySkipReports.begin(),
                                  _dependencySkipReports.end(),
                                  [reportLevel](auto const& a, auto const& b) -> bool {
                                    return a.getSkipped(reportLevel) <
                                           b.getSkipped(reportLevel);
                                  })
                     ->getSkipped(reportLevel));
      TRI_ASSERT(_maximumSkipReport.getFullCount(reportLevel) ==
                 std::max_element(_dependencySkipReports.begin(),
                                  _dependencySkipReports.end(),
                                  [reportLevel](auto const& a, auto const& b) -> bool {
                                    return a.getFullCount(reportLevel) <
                                           b.getFullCount(reportLevel);
                                  })
                     ->getFullCount(reportLevel));
    }
  }
}
