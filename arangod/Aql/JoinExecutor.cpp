////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2023 ArangoDB GmbH, Cologne, Germany
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
/// @author Jan Steemann
/// @author Lars Maier
////////////////////////////////////////////////////////////////////////////////

#include "JoinExecutor.h"
#include "Aql/Collection.h"
#include "Aql/DocumentExpressionContext.h"
#include "Aql/OutputAqlItemRow.h"
#include "Aql/QueryContext.h"
#include "Basics/system-compiler.h"
#include "Aql/ExecutorExpressionContext.h"
#include "Logger/LogMacros.h"
#include "VocBase/LogicalCollection.h"

using namespace arangodb;
using namespace arangodb::aql;

#define LOG_JOIN LOG_DEVEL_IF(false)
#define LOG_JOIN_MEMORY LOG_DEVEL_IF(false)

RegisterId JoinExecutorInfos::registerForVariable(
    VariableId id) const noexcept {
  auto it = varsToRegister.find(id);
  if (it != varsToRegister.end()) {
    return it->second;
  }
  return RegisterId::maxRegisterId;
}

void JoinExecutorInfos::determineProjectionsForRegisters() {
  if (!projectionsInitialized) {
    bool producesOutput = false;
    for (auto& it : indexes) {
      it.hasProjectionsForRegisters = it.projections.hasOutputRegisters();
      producesOutput |= it.producesOutput;
    }
    producesAnyOutput = producesOutput;
    projectionsInitialized = true;
  }
}

JoinExecutor::~JoinExecutor() {
  // handle decrease of memory usage of the projections builder
  if (!_projectionsBuilder.isEmpty()) {
    resourceMonitor().decreaseMemoryUsage(_projectionsBuilder.size());
    _projectionsBuilder.clear();
  }

  if (!_documents.empty()) {
    for (auto& docPtr : _documents) {
      if (docPtr.second > 0) {
        resourceMonitor().decreaseMemoryUsage(docPtr.second);
        docPtr.first.reset();
      }
    }
  }
}

JoinExecutor::JoinExecutor(Fetcher& fetcher, Infos& infos)
    : _infos(infos),
      _trx{_infos.query->newTrxContext()},
      _resourceMonitor(_infos.query->resourceMonitor()) {
  constructStrategy();
  _documents.resize(_infos.indexes.size());
}

namespace {
struct SpanCoveringData : arangodb::IndexIteratorCoveringData {
  explicit SpanCoveringData(std::span<VPackSlice> data) : _data(data) {}
  bool isArray() const noexcept override { return true; }
  VPackSlice at(size_t i) const override {
    TRI_ASSERT(i < _data.size())
        << "accessing index " << i << " but covering data has size "
        << _data.size();
    return _data[i];
  }
  arangodb::velocypack::ValueLength length() const override {
    return _data.size();
  }

  std::span<VPackSlice> _data;
};
}  // namespace

namespace {

VPackSlice extractSlice(VPackSlice slice) { return slice; }

VPackSlice extractSlice(std::unique_ptr<std::string>& ptr) {
  return VPackSlice{reinterpret_cast<uint8_t*>(ptr->data())};
}

void moveValueIntoRegister(OutputAqlItemRow& output, RegisterId reg,
                           InputAqlItemRow& inputRow, VPackSlice slice) {
  output.moveValueInto(reg, inputRow, slice);
}

void moveValueIntoRegister(OutputAqlItemRow& output, RegisterId reg,
                           InputAqlItemRow& inputRow,
                           std::unique_ptr<std::string>& ptr) {
  output.moveValueInto(reg, inputRow, &ptr);
}

template<typename F>
struct DocumentCallbackOverload : F {
  DocumentCallbackOverload(F&& f) : F(std::forward<F>(f)) {}

  bool operator()(LocalDocumentId token, aql::DocumentData&& data,
                  VPackSlice doc) const {
    if (data) {
      return F::template operator()<std::unique_ptr<std::string>&>(token, data);
    }
    return F::template operator()<VPackSlice>(token, doc);
  }

  bool operator()(LocalDocumentId token,
                  std::unique_ptr<std::string>& docPtr) const {
    return F::template operator()<std::unique_ptr<std::string>&>(token, docPtr);
  }
};

}  // namespace

auto JoinExecutor::produceRows(AqlItemBlockInputRange& inputRange,
                               OutputAqlItemRow& output)
    -> std::tuple<ExecutorState, Stats, AqlCall> {
  JoinStats stats{};
  AqlCall upstreamCall{};
  upstreamCall.fullCount = output.getClientCall().fullCount;

  _infos.determineProjectionsForRegisters();

  while (inputRange.hasDataRow() && !output.isFull()) {
    if (!_currentRow) {
      std::tie(_currentRowState, _currentRow) = inputRange.peekDataRow();
      _constantBuilder.clear();
      _constantSlices.clear();
      _constantBuilder.openArray();

      for (auto const& idx : _infos.indexes) {
        if (!idx.constantExpressions.empty()) {
          for (auto& expr : idx.constantExpressions) {
            bool mustDestroy = false;
            ExecutorExpressionContext ctx{_trx, *_infos.query, _functionsCache,
                                          _currentRow,
                                          idx.expressionVarsToRegs};

            aql::AqlValue res = expr->execute(&ctx, mustDestroy);
            aql::AqlValueGuard guard{res, mustDestroy};
            LOG_JOIN << "Expression result: " << res.slice().toJson();
            _constantBuilder.add(res.slice());
          }
        }
      }
      _constantBuilder.close();  // array

      for (VPackSlice it : VPackArrayIterator(_constantBuilder.slice())) {
        _constantSlices.push_back(it);
      }
      _strategy->reset(_constantSlices);
    }

    [[maybe_unused]] std::size_t rowCount = 0;
    auto [hasMore,
          amountOfSeeks] = _strategy->next([&](std::span<LocalDocumentId>
                                                   docIds,
                                               std::span<VPackSlice>
                                                   projections) -> bool {
      // increment scanned index value for every match. The amount of
      // increment is equal to the amount of indices being used here.
      stats.incrScannedIndex(_infos.indexes.size());

      LOG_JOIN << "BEGIN OF ROW " << rowCount++;

      LOG_JOIN << "PROJECTIONS:";
      for (auto p : projections) {
        LOG_JOIN << p.toJson();
      }

      auto lookupDocument = [&](std::size_t index, LocalDocumentId id,
                                auto cb) {
        auto result =
            _infos.indexes[index]
                .collection->getCollection()
                ->getPhysical()
                ->lookup(&_trx, id,
                         {DocumentCallbackOverload{
                             [&](LocalDocumentId token, auto docPtr) {
                               cb.template operator()<decltype(docPtr)>(docPtr);
                               return true;
                             }}},
                         {.countBytes = true});
        if (ADB_UNLIKELY(result.fail())) {
          THROW_ARANGO_EXCEPTION_MESSAGE(
              result.errorNumber(),
              basics::StringUtils::concatT(
                  "failed to lookup indexed document ", id.id(),
                  " for collection ", _infos.indexes[index].collection->name(),
                  ": ", result.errorMessage()));
        }
        stats.incrDocumentLookups(1);
      };

      // first do all the filtering and only if all indexes produced a
      // value write it into the aql output block

      std::size_t projectionsOffset = 0;

      auto buildProjections = [&](size_t k, aql::Projections const& proj,
                                  bool hasProjectionsForRegisters) {
        // build the document from projections
        std::span<VPackSlice> projectionRange = {
            projections.begin() + projectionsOffset,
            projections.begin() + projectionsOffset + proj.size()};

        auto data = SpanCoveringData{projectionRange};
        if (!hasProjectionsForRegisters) {
          // write all projections combined into the global output register
          // recycle our Builder object _projectionsBuilder.clear();
          _projectionsBuilder.openObject(true);
          proj.toVelocyPackFromIndexCompactArray(_projectionsBuilder, data,
                                                 &_trx);
          _projectionsBuilder.close();

          // handle increase of memory usage of the builder
          try {
            LOG_JOIN_MEMORY
                << "(buildProjections1) Increasing memory usage by: "
                << _projectionsBuilder.size() << "for the projections builder";
            resourceMonitor().increaseMemoryUsage(_projectionsBuilder.size());
          } catch (...) {
            _projectionsBuilder.clear();
            throw;
          }
        } else {
          // write projections into individual output registers
          proj.produceFromIndexCompactArray(
              _projectionsBuilder, data, &_trx,
              [&](Variable const* variable, velocypack::Slice slice) {
                if (slice.isNone()) {
                  slice = VPackSlice::nullSlice();
                }
                RegisterId registerId =
                    _infos.registerForVariable(variable->id);
                TRI_ASSERT(registerId != RegisterId::maxRegisterId);
                output.moveValueInto(registerId, _currentRow, slice);
              });

          // handle increase of memory usage of the builder
          try {
            LOG_JOIN_MEMORY
                << "(buildProjections2) Increasing memory usage by: "
                << _projectionsBuilder.size() << " for the projections builder";
            resourceMonitor().increaseMemoryUsage(_projectionsBuilder.size());
          } catch (...) {
            _projectionsBuilder.clear();
            throw;
          }
        }
      };

      for (std::size_t k = 0; k < docIds.size(); k++) {
        auto& idx = _infos.indexes[k];
        if (idx.projections.usesCoveringIndex(idx.index)) {
          projectionsOffset += idx.projections.size();
        }
        // evaluate filter conditions
        if (!idx.filter.has_value()) {
          continue;
        }

        bool const useFilterProjections =
            idx.filter->projections.usesCoveringIndex();
        bool filtered = false;

        auto filterCallback = [&](auto docPtr) {
          TRI_ASSERT(!useFilterProjections);
          auto doc = extractSlice(docPtr);
          LOG_JOIN << "INDEX " << k << " read document " << doc.toJson();
          GenericDocumentExpressionContext ctx{_trx,
                                               *_infos.query,
                                               _functionsCache,
                                               idx.filter->filterVarsToRegs,
                                               _currentRow,
                                               idx.filter->documentVariable};
          ctx.setCurrentDocument(doc);
          bool mustDestroy;
          AqlValue result = idx.filter->expression->execute(&ctx, mustDestroy);
          AqlValueGuard guard(result, mustDestroy);
          filtered = !result.toBoolean();
          LOG_JOIN << "INDEX " << k << " filter = " << std::boolalpha
                   << filtered;

          if (!filtered) {
            // add document to the list
            if (_documents[k].second > 0) {
              LOG_JOIN_MEMORY << "(filterCB) Decreasing memory usage by: "
                              << _documents[k].second
                              << " for doc at position: " << k;
              resourceMonitor().decreaseMemoryUsage(_documents[k].second);
              _documents[k].first.reset();
            }

            try {
              LOG_JOIN_MEMORY
                  << "(filterCB) Increasing memory usage by: " << doc.byteSize()
                  << " for doc at position: " << k;
              resourceMonitor().increaseMemoryUsage(doc.byteSize());
              _documents[k] = std::make_pair(
                  std::make_unique<std::string>(doc.template startAs<char>(),
                                                doc.byteSize()),
                  doc.byteSize());
            } catch (...) {
              throw;
            }
          }
        };

        auto filterWithProjectionsCallback =
            [&](std::span<VPackSlice> projections) {
              GenericDocumentExpressionContext ctx{
                  _trx,
                  *_infos.query,
                  _functionsCache,
                  idx.filter->filterVarsToRegs,
                  _currentRow,
                  idx.filter->documentVariable};
              ctx.setCurrentDocument(VPackSlice::noneSlice());

              TRI_ASSERT(idx.filter->projections.size() == projections.size());
              for (size_t j = 0; j < projections.size(); j++) {
                TRI_ASSERT(projections[j].start() != nullptr);
                LOG_JOIN << "INDEX " << k << " set "
                         << idx.filter->filterProjectionVars[j]->id << " = "
                         << projections[j].toJson();
                ctx.setVariable(idx.filter->filterProjectionVars[j],
                                projections[j]);
              }

              bool mustDestroy;
              AqlValue result =
                  idx.filter->expression->execute(&ctx, mustDestroy);
              AqlValueGuard guard(result, mustDestroy);
              filtered = !result.toBoolean();
            };

        if (useFilterProjections) {
          LOG_JOIN << "projectionsOffset = " << projectionsOffset;
          std::span<VPackSlice> projectionRange = {
              projections.begin() + projectionsOffset,
              projections.begin() + projectionsOffset +
                  idx.filter->projections.size()};
          LOG_JOIN << "INDEX " << k << " using filter projections";
          filterWithProjectionsCallback(projectionRange);
          projectionsOffset += idx.filter->projections.size();
        } else {
          LOG_JOIN << "INDEX " << k << " looking up document " << docIds[k];
          lookupDocument(k, docIds[k], filterCallback);
        }

        if (filtered) {
          // forget about this row
          LOG_JOIN << "INDEX " << k << " eliminated pair";
          LOG_JOIN << "FILTERED ROW " << (rowCount - 1);
          stats.incrFiltered(1);
          return true;
        }
      }

      // Now produce the documents
      TRI_ASSERT(_infos.projectionsInitialized);
      projectionsOffset = 0;
      for (std::size_t k = 0; k < docIds.size(); k++) {
        auto& idx = _infos.indexes[k];

        if (!idx.producesOutput) {
          continue;
        }
        auto docProduceCallback = [&](auto docPtr) {
          auto doc = extractSlice(docPtr);
          if (idx.projections.empty()) {
            // no projections
            moveValueIntoRegister(output,
                                  _infos.indexes[k].documentOutputRegister,
                                  _currentRow, docPtr);
          } else if (!idx.hasProjectionsForRegisters) {
            // handle decrease of memory usage of the builder
            if (!_projectionsBuilder.isEmpty()) {
              resourceMonitor().decreaseMemoryUsage(_projectionsBuilder.size());
            }

            // write all projections combined into the
            // global output register recycle our
            // Builder object
            _projectionsBuilder.clear();
            _projectionsBuilder.openObject(true);
            idx.projections.toVelocyPackFromDocument(_projectionsBuilder, doc,
                                                     &_trx);
            _projectionsBuilder.close();

            // handle increase of memory usage of the builder
            try {
              LOG_JOIN_MEMORY << "(docCB1) Increasing memory usage by: "
                              << _projectionsBuilder.size()
                              << "for the projections builder";
              resourceMonitor().increaseMemoryUsage(_projectionsBuilder.size());
            } catch (...) {
              _projectionsBuilder.clear();
              throw;
            }

            output.moveValueInto(_infos.indexes[k].documentOutputRegister,
                                 _currentRow, _projectionsBuilder.slice());
          } else {
            // handle decrease of memory usage of the builder
            if (!_projectionsBuilder.isEmpty()) {
              resourceMonitor().decreaseMemoryUsage(_projectionsBuilder.size());
            }

            // write projections into individual
            // output registers
            idx.projections.produceFromDocument(
                _projectionsBuilder, doc, &_trx,
                [&](Variable const* variable, velocypack::Slice slice) {
                  if (slice.isNone()) {
                    slice = VPackSlice::nullSlice();
                  }
                  RegisterId registerId =
                      _infos.registerForVariable(variable->id);
                  TRI_ASSERT(registerId != RegisterId::maxRegisterId);
                  output.moveValueInto(registerId, _currentRow, slice);
                });

            // handle increase of memory usage of the builder
            try {
              LOG_JOIN_MEMORY << "(docCB2) Increasing memory usage by: "
                              << _projectionsBuilder.size()
                              << "for the projections builder";
              resourceMonitor().increaseMemoryUsage(_projectionsBuilder.size());
            } catch (...) {
              _projectionsBuilder.clear();
              throw;
            }
          }
        };

        // idx.projections.usesCoveringIndex(idx.index) &&
        // idx.projections.empty()  only allowed in late materialization
        // case
        TRI_ASSERT(!idx.projections.usesCoveringIndex(idx.index) ||
                   !idx.projections.empty() || idx.isLateMaterialized);

        if (auto& docPtr = _documents[k].first; docPtr) {
          TRI_ASSERT(idx.filter.has_value() &&
                     !idx.filter->projections.usesCoveringIndex());
          docProduceCallback.operator()<std::unique_ptr<std::string>&>(docPtr);
          resourceMonitor().decreaseMemoryUsage(_documents[k].second);
          _documents[k].second = 0;
          docPtr.reset();
        } else if (idx.projections.usesCoveringIndex(idx.index) &&
                   !idx.projections.empty()) {
          buildProjections(k, idx.projections, idx.hasProjectionsForRegisters);
          if (!idx.hasProjectionsForRegisters) {
            output.moveValueInto(_infos.indexes[k].documentOutputRegister,
                                 _currentRow, _projectionsBuilder.slice());
          }

          projectionsOffset += idx.projections.size();
        } else if (!idx.isLateMaterialized) {
          lookupDocument(k, docIds[k], docProduceCallback);
        }

        if (idx.isLateMaterialized) {
          AqlValue v(AqlValueHintUInt(docIds[k].id()));
          AqlValueGuard guard{v, false};
          output.moveValueInto(idx.docIdOutputRegister, _currentRow, &guard);
        }

        if (idx.filter && idx.filter->projections.usesCoveringIndex()) {
          projectionsOffset += idx.filter->projections.size();
        }
      }

      if (!_infos.producesAnyOutput) {
        output.handleEmptyRow(_currentRow);
      }

      TRI_ASSERT(output.produced());
      output.advanceRow();
      LOG_JOIN << "OUTPUT ROW " << (rowCount - 1);
      return !output.isFull();
    });

    if (!hasMore) {
      _currentRow = InputAqlItemRow{CreateInvalidInputRowHint{}};
      inputRange.advanceDataRow();
    }
    stats.incrSeeks(amountOfSeeks);
  }

  return {inputRange.upstreamState(), stats, upstreamCall};
}

auto JoinExecutor::skipRowsRange(AqlItemBlockInputRange& inputRange,
                                 AqlCall& clientCall)
    -> std::tuple<ExecutorState, Stats, size_t, AqlCall> {
  JoinStats stats{};

  while (inputRange.hasDataRow() && clientCall.needSkipMore()) {
    if (!_currentRow) {
      std::tie(_currentRowState, _currentRow) = inputRange.peekDataRow();
      _strategy->reset({});
    }

    auto [hasMore, amountOfSeeks] =
        _strategy->next([&](std::span<LocalDocumentId> docIds,
                            std::span<VPackSlice> projections) -> bool {
          // increment scanned index value for every match. The amount of
          // increment is equal to the amount of indices being used here.
          stats.incrScannedIndex(_infos.indexes.size());

          auto lookupDocument = [&](std::size_t index, LocalDocumentId id,
                                    auto cb) {
            auto result =
                _infos.indexes[index]
                    .collection->getCollection()
                    ->getPhysical()
                    ->lookup(
                        &_trx, id,
                        {DocumentCallbackOverload{
                            [&](LocalDocumentId token, auto docPtr) {
                              cb.template operator()<decltype(docPtr)>(docPtr);
                              return true;
                            }}},
                        {.countBytes = true});
            if (result.fail()) {
              THROW_ARANGO_EXCEPTION_MESSAGE(
                  result.errorNumber(),
                  basics::StringUtils::concatT(
                      "failed to lookup indexed document ", id.id(),
                      " for collection ",
                      _infos.indexes[index].collection->name(), ": ",
                      result.errorMessage()));
            }
            stats.incrDocumentLookups(1);
          };

          // first do all the filtering and only if all indexes produced a
          // value write it into the aql output block

          std::size_t projectionsOffset = 0;

          for (std::size_t k = 0; k < docIds.size(); k++) {
            auto& idx = _infos.indexes[k];
            if (idx.projections.usesCoveringIndex(idx.index)) {
              projectionsOffset += idx.projections.size();
            }
            // evaluate filter conditions
            if (!idx.filter.has_value()) {
              continue;
            }

            bool const useFilterProjections =
                idx.filter->projections.usesCoveringIndex();
            bool filtered = false;

            auto filterCallback = [&](auto docPtr) {
              auto doc = extractSlice(docPtr);
              LOG_JOIN << "INDEX " << k << " read document " << doc.toJson();
              GenericDocumentExpressionContext ctx{
                  _trx,
                  *_infos.query,
                  _functionsCache,
                  idx.filter->filterVarsToRegs,
                  _currentRow,
                  idx.filter->documentVariable};
              ctx.setCurrentDocument(doc);
              bool mustDestroy;
              AqlValue result =
                  idx.filter->expression->execute(&ctx, mustDestroy);
              AqlValueGuard guard(result, mustDestroy);
              filtered = !result.toBoolean();
              LOG_JOIN << "INDEX " << k << " filter = " << std::boolalpha
                       << filtered;
            };

            auto filterWithProjectionsCallback =
                [&](std::span<VPackSlice> projections) {
                  GenericDocumentExpressionContext ctx{
                      _trx,
                      *_infos.query,
                      _functionsCache,
                      idx.filter->filterVarsToRegs,
                      _currentRow,
                      idx.filter->documentVariable};
                  ctx.setCurrentDocument(VPackSlice::noneSlice());

                  TRI_ASSERT(idx.filter->projections.size() ==
                             projections.size());
                  for (size_t j = 0; j < projections.size(); j++) {
                    TRI_ASSERT(projections[j].start() != nullptr);
                    LOG_JOIN << "INDEX " << k << " set "
                             << idx.filter->filterProjectionVars[j]->id << " = "
                             << projections[j].toJson();
                    ctx.setVariable(idx.filter->filterProjectionVars[j],
                                    projections[j]);
                  }

                  bool mustDestroy;
                  AqlValue result =
                      idx.filter->expression->execute(&ctx, mustDestroy);
                  AqlValueGuard guard(result, mustDestroy);
                  filtered = !result.toBoolean();
                };

            if (useFilterProjections) {
              LOG_JOIN << "projectionsOffset = " << projectionsOffset;
              std::span<VPackSlice> projectionRange = {
                  projections.begin() + projectionsOffset,
                  projections.begin() + projectionsOffset +
                      idx.filter->projections.size()};
              LOG_JOIN << "INDEX " << k << " using filter projections";
              filterWithProjectionsCallback(projectionRange);
              projectionsOffset += idx.filter->projections.size();
            } else {
              LOG_JOIN << "INDEX " << k << " looking up document " << docIds[k];
              lookupDocument(k, docIds[k], filterCallback);
            }

            if (filtered) {
              // forget about this row
              LOG_JOIN << "INDEX " << k << " eliminated pair";
              stats.incrFiltered(1);
              return clientCall.needSkipMore();
            }
          }

          clientCall.didSkip(1);
          return clientCall.needSkipMore();
        });

    if (!hasMore) {
      _currentRow = InputAqlItemRow{CreateInvalidInputRowHint{}};
      inputRange.advanceDataRow();
    }
    stats.incrSeeks(amountOfSeeks);
  }

  return {inputRange.upstreamState(), stats, clientCall.getSkipCount(),
          AqlCall{}};
}

ResourceMonitor& JoinExecutor::resourceMonitor() { return _resourceMonitor; }

void JoinExecutor::constructStrategy() {
  std::vector<IndexJoinStrategyFactory::Descriptor> indexDescription;
  for (auto const& idx : _infos.indexes) {
    IndexStreamOptions options;
    options.usedKeyFields = idx.usedKeyFields;
    options.constantFields = idx.constantFields;

    auto& desc = indexDescription.emplace_back();
    desc.isUnique = idx.index->unique();
    desc.numProjections = 0;
    desc.numConstants = idx.constantFields.size();
    desc.numKeyComponents = idx.usedKeyFields.size();

    if (idx.projections.usesCoveringIndex()) {
      TRI_ASSERT(!idx.filter.has_value() ||
                 idx.filter->projections.usesCoveringIndex());
      LOG_JOIN << "USING DOCUMENT PROJECTIONS";
      std::transform(idx.projections.projections().begin(),
                     idx.projections.projections().end(),
                     std::back_inserter(options.projectedFields),
                     [&](Projections::Projection const& proj) {
                       return proj.coveringIndexPosition;
                     });

      desc.numProjections += idx.projections.size();
    }
    if (idx.filter && idx.filter->projections.usesCoveringIndex()) {
      LOG_JOIN << "USING FILTER PROJECTIONS";
      std::transform(idx.filter->projections.projections().begin(),
                     idx.filter->projections.projections().end(),
                     std::back_inserter(options.projectedFields),
                     [&](Projections::Projection const& proj) {
                       return proj.coveringIndexPosition;
                     });

      desc.numProjections += idx.filter->projections.size();
    }
    LOG_JOIN << "PROJECTIONS FOR INDEX " << options.projectedFields;

    auto stream = idx.index->streamForCondition(&_trx, options);
    TRI_ASSERT(stream != nullptr);
    desc.iter = std::move(stream);
  }

  // TODO actually we want to have different strategies, like hash join and
  // special implementations for n = 2, 3, ...
  // TODO maybe make this an template parameter
  _strategy = IndexJoinStrategyFactory{}.createStrategy(
      std::move(indexDescription),
      _infos.query->queryOptions().desiredJoinStrategy);
}
