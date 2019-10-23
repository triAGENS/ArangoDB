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
/// @author Markus Pfeiffer
////////////////////////////////////////////////////////////////////////////////

#ifndef ARANGOD_AQL_UPSERT_MODIFIER_H
#define ARANGOD_AQL_UPSERT_MODIFIER_H

#include "Aql/ModificationExecutor2.h"
#include "Aql/ModificationExecutorInfos.h"

#include "Aql/InsertModifier.h"
#include "Aql/UpdateReplaceModifier.h"

namespace arangodb {
namespace aql {

struct ModificationExecutorInfos;

class UpsertModifier {
 public:
  using OutputTuple = std::tuple<ModOperationType, InputAqlItemRow, VPackSlice>;
  using ModOp = std::pair<ModOperationType, InputAqlItemRow>;

 public:
  UpsertModifier(ModificationExecutorInfos& infos);
  ~UpsertModifier();

  void reset();
  void close();

  Result accumulate(InputAqlItemRow& row);
  Result transact();

  size_t size() const;
  size_t nrOfOperations() const;

  // TODO: Make this a real iterator
  Result setupIterator(ModifierIteratorMode mode);
  bool isFinishedIterator();
  OutputTuple getOutput();
  void advanceIterator();

  size_t getBatchSize() const;

 private:
  ModOperationType updateReplaceCase(VPackBuilder& accu, AqlValue const& inDoc,
                                     AqlValue const& updateDoc);
  ModOperationType insertCase(VPackBuilder& accu, AqlValue const& insertDoc);

  ModificationExecutorInfos& _infos;
  std::vector<ModOp> _operations;
  VPackBuilder _insertAccumulator;
  VPackBuilder _updateAccumulator;

  OperationResult _updateResults;
  OperationResult _insertResults;

  std::vector<ModOp>::const_iterator _operationsIterator;

  VPackArrayIterator _updateResultsIterator;
  VPackArrayIterator _insertResultsIterator;

  ModifierIteratorMode _iteratorMode;
  size_t const _batchSize;
};

}  // namespace aql
}  // namespace arangodb
#endif
