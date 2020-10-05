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
/// @author Jan Christoph Uhde
////////////////////////////////////////////////////////////////////////////////

#include "Aql/SingleRemoteModificationExecutor.h"

#include "Aql/AqlValue.h"
#include "Aql/Collection.h"
#include "Aql/OutputAqlItemRow.h"
#include "Aql/SingleRowFetcher.h"
#include "Aql/QueryContext.h"
#include "Basics/Common.h"
#include "Basics/StaticStrings.h"
#include "Cluster/ClusterInfo.h"
#include "Cluster/ServerState.h"
#include "Transaction/Methods.h"
#include "VocBase/LogicalCollection.h"

#include <algorithm>

using namespace arangodb;
using namespace arangodb::aql;

namespace {
std::unique_ptr<VPackBuilder> merge(VPackSlice document, std::string const& key,
                                    RevisionId revision) {
  auto builder = std::make_unique<VPackBuilder>();
  {
    VPackObjectBuilder guard(builder.get());
    TRI_SanitizeObject(document, *builder);
    VPackSlice keyInBody = document.get(StaticStrings::KeyString);

    if (keyInBody.isNone() || keyInBody.isNull() ||
        (keyInBody.isString() && keyInBody.copyString() != key) ||
        ((revision.isSet()) && (RevisionId::fromSlice(document) != revision))) {
      // We need to rewrite the document with the given revision and key:
      builder->add(StaticStrings::KeyString, VPackValue(key));
      if (revision.isSet()) {
        builder->add(StaticStrings::RevString, VPackValue(revision.toString()));
      }
    }
  }
  return builder;
}
}  // namespace

template <typename Modifier>
SingleRemoteModificationExecutor<Modifier>::SingleRemoteModificationExecutor(Fetcher& fetcher,
                                                                             Infos& info)
    : _trx(info._query.newTrxContext()), _info(info), _upstreamState(ExecutionState::HASMORE) {
  TRI_ASSERT(arangodb::ServerState::instance()->isCoordinator());
};

template <typename Modifier>
[[nodiscard]] auto SingleRemoteModificationExecutor<Modifier>::produceRows(
    AqlItemBlockInputRange& input, OutputAqlItemRow& output)
    -> std::tuple<ExecutorState, typename SingleRemoteModificationExecutor<Modifier>::Stats, AqlCall> {
  auto stats = Stats{};

  if (input.hasDataRow()) {
    auto [state, row] = input.nextDataRow();
    auto result = doSingleRemoteModificationOperation(row, stats);
    if (result.ok()) {
      doSingleRemoteModificationOutput(row, output, result);
    }
  }

  return {input.upstreamState(), stats, AqlCall{}};
}

template <typename Modifier>
[[nodiscard]] auto SingleRemoteModificationExecutor<Modifier>::skipRowsRange(
    AqlItemBlockInputRange& input, AqlCall& call)
    -> std::tuple<ExecutorState, typename SingleRemoteModificationExecutor<Modifier>::Stats, size_t, AqlCall> {
  auto stats = Stats{};

  if (input.hasDataRow()) {
    auto [state, row] = input.nextDataRow();
    auto result = doSingleRemoteModificationOperation(row, stats);
    if (result.ok()) {
      call.didSkip(1);
      return {input.upstreamState(), stats, 1, AqlCall{}};
    }
  }
  return {input.upstreamState(), stats, 0, AqlCall{}};
}

template <typename Modifier>
auto SingleRemoteModificationExecutor<Modifier>::doSingleRemoteModificationOperation(
    InputAqlItemRow& input, Stats& stats) -> OperationResult {
  _info._options.silent = false;
  _info._options.returnOld = _info._options.returnOld ||
                             _info._outputRegisterId != RegisterPlan::MaxRegisterId;

  OperationResult result(Result(), _info._options);

  const bool isIndex = std::is_same<Modifier, IndexTag>::value;
  const bool isInsert = std::is_same<Modifier, Insert>::value;
  const bool isRemove = std::is_same<Modifier, Remove>::value;
  const bool isUpdate = std::is_same<Modifier, Update>::value;
  const bool isReplace = std::is_same<Modifier, Replace>::value;

  int possibleWrites = 0;  // TODO - get real statistic values!

  if (_info._key.empty() && _info._input1RegisterId == RegisterPlan::MaxRegisterId) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_ARANGO_DOCUMENT_NOT_FOUND,
                                   "missing document reference");
  }

  VPackBuilder inBuilder;
  VPackSlice inSlice = VPackSlice::emptyObjectSlice();
  if (_info._input1RegisterId != RegisterPlan::MaxRegisterId) {  // IF NOT REMOVE OR SELECT
    AqlValue const& inDocument = input.getValue(_info._input1RegisterId);
    inBuilder.add(inDocument.slice());
    inSlice = inBuilder.slice();
  }

  std::unique_ptr<VPackBuilder> mergedBuilder = nullptr;
  if (!_info._key.empty()) {
    mergedBuilder = merge(inSlice, _info._key, RevisionId::none());
    inSlice = mergedBuilder->slice();
  }

  if (isIndex) {
    result = _trx.document(_info._aqlCollection->name(), inSlice, _info._options);
  } else if (isInsert) {
    if (_info._options.returnOld && !_info._options.isOverwriteModeUpdateReplace()) {
      THROW_ARANGO_EXCEPTION_MESSAGE(
          TRI_ERROR_QUERY_VARIABLE_NAME_UNKNOWN,
          "OLD is only available when using INSERT with overwriteModes 'update' or 'replace'");
    }
    result = _trx.insert(_info._aqlCollection->name(), inSlice, _info._options);
    possibleWrites = 1;
  } else if (isRemove) {
    result = _trx.remove(_info._aqlCollection->name(), inSlice, _info._options);
    possibleWrites = 1;
  } else if (isReplace) {
    if (_info._replaceIndex && _info._input1RegisterId == RegisterPlan::MaxRegisterId) {
      // we have a FOR .. IN FILTER doc._key == ... REPLACE - no WITH.
      // in this case replace needs to behave as if it was UPDATE.
      result = _trx.update(_info._aqlCollection->name(), inSlice, _info._options);
    } else {
      result = _trx.replace(_info._aqlCollection->name(), inSlice, _info._options);
    }
    possibleWrites = 1;
  } else if (isUpdate) {
    result = _trx.update(_info._aqlCollection->name(), inSlice, _info._options);
    possibleWrites = 1;
  }

  // check operation result
  if (!result.ok()) {
    if (result.is(TRI_ERROR_ARANGO_DOCUMENT_NOT_FOUND) &&
        (isIndex || (isUpdate && _info._replaceIndex) ||
         (isRemove && _info._replaceIndex) || (isReplace && _info._replaceIndex))) {
      // document not there is not an error in this situation.
      // FOR ... FILTER ... REMOVE wouldn't invoke REMOVE in first place, so
      // don't throw an excetpion.
      return result;
    } else if (!_info._ignoreErrors) {  // TODO remove if
      THROW_ARANGO_EXCEPTION_MESSAGE(result.errorNumber(), result.errorMessage());
    }

    if (isIndex) {
      return result;
    }
  }

  stats.addWritesExecuted(possibleWrites);
  stats.incrScannedIndex();
  return result;
}

template <typename Modifier>
auto SingleRemoteModificationExecutor<Modifier>::doSingleRemoteModificationOutput(
    InputAqlItemRow& input, OutputAqlItemRow& output, OperationResult& result) -> void {
  OperationOptions& options = _info._options;

  if (!(_info._outputRegisterId != RegisterPlan::MaxRegisterId ||
        _info._outputOldRegisterId != RegisterPlan::MaxRegisterId ||
        _info._outputNewRegisterId != RegisterPlan::MaxRegisterId)) {
    if (_info._hasParent) {
      output.copyRow(input);
    }
    return;  //  _info._hasParent;
  }

  // Fill itemblock
  // create block that can hold a result with one entry and a number of
  // variables corresponding to the amount of out variables

  // only copy 1st row of registers inherited from previous frame(s)
  TRI_ASSERT(result.ok());
  VPackSlice outDocument = VPackSlice::nullSlice();
  if (result.buffer) {
    outDocument = result.slice().resolveExternal();
  }
  
  const bool isIndex = std::is_same<Modifier, IndexTag>::value;
    
  VPackSlice oldDocument = VPackSlice::nullSlice();
  VPackSlice newDocument = VPackSlice::nullSlice();
  if (!isIndex && outDocument.isObject()) {
    if (_info._outputNewRegisterId != RegisterPlan::MaxRegisterId &&
        outDocument.hasKey(StaticStrings::New)) {
      newDocument = outDocument.get(StaticStrings::New);
    }
    if (outDocument.hasKey(StaticStrings::Old)) {
      outDocument = outDocument.get(StaticStrings::Old);
      if (_info._outputOldRegisterId != RegisterPlan::MaxRegisterId) {
        oldDocument = outDocument;
      }
    }
  }

  TRI_ASSERT(_info._outputRegisterId != RegisterPlan::MaxRegisterId ||
             _info._outputOldRegisterId != RegisterPlan::MaxRegisterId ||
             _info._outputNewRegisterId != RegisterPlan::MaxRegisterId);

  // place documents as in the out variable slots of the result
  if (_info._outputRegisterId != RegisterPlan::MaxRegisterId) {
    AqlValue value(outDocument);
    AqlValueGuard guard(value, true);
    output.moveValueInto(_info._outputRegisterId, input, guard);
  }

  if (_info._outputOldRegisterId != RegisterPlan::MaxRegisterId) {
    TRI_ASSERT(options.returnOld);
    AqlValue value(oldDocument);
    AqlValueGuard guard(value, true);
    output.moveValueInto(_info._outputOldRegisterId, input, guard);
  }

  if (_info._outputNewRegisterId != RegisterPlan::MaxRegisterId) {
    TRI_ASSERT(options.returnNew);
    AqlValue value(newDocument);
    AqlValueGuard guard(value, true);
    output.moveValueInto(_info._outputNewRegisterId, input, guard);
  }

  TRI_IF_FAILURE("SingleRemoteModificationOperationBlock::moreDocuments") {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_DEBUG);
  }
}

template struct ::arangodb::aql::SingleRemoteModificationExecutor<IndexTag>;
template struct ::arangodb::aql::SingleRemoteModificationExecutor<Insert>;
template struct ::arangodb::aql::SingleRemoteModificationExecutor<Remove>;
template struct ::arangodb::aql::SingleRemoteModificationExecutor<Replace>;
template struct ::arangodb::aql::SingleRemoteModificationExecutor<Update>;
