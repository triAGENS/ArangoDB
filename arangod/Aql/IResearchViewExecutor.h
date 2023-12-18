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
/// @author Tobias Gödderz
/// @author Andrey Abramov
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include "Aql/ExecutionState.h"
#include "Aql/IResearchViewNode.h"
#include "Aql/InputAqlItemRow.h"
#include "Aql/AqlFunctionsInternalCache.h"
#include "Aql/MultiGet.h"
#include "Aql/RegisterInfos.h"
#include "Aql/VarInfoMap.h"
#include "Containers/FlatHashSet.h"
#include "IResearch/ExpressionFilter.h"
#include "IResearch/IResearchFilterFactory.h"
#include "IResearch/IResearchExecutionPool.h"
#include "IResearch/IResearchExpressionContext.h"
#include "IResearch/IResearchVPackComparer.h"
#include "IResearch/IResearchView.h"
#include "IResearch/IResearchFeature.h"
#include "IResearch/SearchDoc.h"
#include "Indexes/IndexIterator.h"
#include "VocBase/Identifiers/LocalDocumentId.h"

#ifdef USE_ENTERPRISE
#include "Enterprise/IResearch/IResearchOptimizeTopK.h"
#endif

#include <formats/formats.hpp>
#include <index/heap_iterator.hpp>
#include <utils/empty.hpp>
#include <search/score.hpp>

#include <utility>
#include <variant>

namespace irs {

struct score;

}  // namespace irs
namespace arangodb {

class LogicalCollection;

namespace iresearch {

struct SearchFunc;
class SearchMeta;

}  // namespace iresearch
namespace aql {

struct AqlCall;
class AqlItemBlockInputRange;
struct ExecutionStats;
class OutputAqlItemRow;
template<BlockPassthrough>
class SingleRowFetcher;
class QueryContext;

struct IResearchViewExecutorInfos {
  bool isOldMangling() const noexcept { return meta == nullptr; }

  iresearch::ViewSnapshotPtr reader;
  iresearch::SearchMeta const* meta;

  QueryContext* query;
  Ast* ast;
  VarInfoMap const* varInfo;
  AstNode const* filter;
  Variable const* outVar;

  RegisterId docReg;
  RegisterId searchDocReg;
  std::vector<RegisterId> scoreRegs;
  iresearch::IResearchViewNode::ViewValuesRegisters valuesRegs;

  std::span<iresearch::SearchFunc const> scorers;
  iresearch::IResearchViewStoredValues const* storedValues;

  std::span<iresearch::HeapSortElement const> heapSort;
  size_t heapSortLimit;
#ifdef USE_ENTERPRISE
  iresearch::IResearchOptimizeTopK const* optimizeTopK;
#endif
  std::pair<iresearch::IResearchSortBase const*, size_t> sort;

  int32_t depth;
  uint32_t immutableParts;
  iresearch::FilterOptimization filterOptimization;
  iresearch::CountApproximate countApproximate;
  bool emptyFilter;
  bool volatileSort;
  bool volatileFilter;

  size_t parallelism;
  iresearch::IResearchExecutionPool* parallelExecutionPool;
};

class IResearchViewStats {
 public:
  void incrScanned() noexcept { ++_scannedIndex; }
  void incrScanned(size_t value) noexcept { _scannedIndex += value; }
  void operator+=(IResearchViewStats const& stats) {
    _scannedIndex += stats._scannedIndex;
  }
  size_t getScanned() const noexcept { return _scannedIndex; }

 private:
  size_t _scannedIndex{};
};

inline ExecutionStats& operator+=(ExecutionStats& executionStats,
                                  IResearchViewStats const& viewStats) noexcept;

struct ColumnIterator {
  irs::doc_iterator::ptr itr;
  irs::payload const* value{};
};

struct FilterCtx final : irs::attribute_provider {
  explicit FilterCtx(iresearch::ViewExpressionContext& ctx) noexcept
      : _execCtx(ctx) {}

  irs::attribute* get_mutable(irs::type_info::type_id type) noexcept override {
    return irs::type<iresearch::ExpressionExecutionContext>::id() == type
               ? &_execCtx
               : nullptr;
  }

  iresearch::ExpressionExecutionContext _execCtx;
};

union HeapSortValue {
  HeapSortValue() : score{} {}

  irs::score_t score;
  velocypack::Slice slice;
};

struct PushTag {};

// Holds and encapsulates the data read from the iresearch index.
template<typename ValueType, bool copyStored>
class IndexReadBuffer {
 public:
  using KeyValueType = ValueType;

  explicit IndexReadBuffer(size_t numScores, ResourceMonitor& monitor);

  ValueType const& getValue(size_t idx) const noexcept {
    assertSizeCoherence();
    TRI_ASSERT(_keyBuffer.size() > idx);
    return _keyBuffer[idx];
  }

  ValueType& getValue(size_t idx) noexcept {
    TRI_ASSERT(_keyBuffer.size() > idx);
    return _keyBuffer[idx];
  }

  iresearch::SearchDoc const& getSearchDoc(size_t idx) noexcept {
    TRI_ASSERT(_searchDocs.size() > idx);
    return _searchDocs[idx];
  }

  auto getScores(size_t idx) noexcept {
    assertSizeCoherence();
    return std::span{_scoreBuffer.data() + idx * _numScores, _numScores};
  }

  void setHeapSort(std::span<iresearch::HeapSortElement const> s,
                   iresearch::IResearchViewNode::ViewValuesRegisters const&
                       storedValues) noexcept {
    TRI_ASSERT(_heapSort.empty());
    TRI_ASSERT(_heapOnlyColumnsCount == 0);
    auto const storedValuesCount = storedValues.size();
    size_t j = 0;
    for (auto const& c : s) {
      auto& sort = _heapSort.emplace_back(BufferHeapSortElement{c});
      if (c.isScore()) {
        continue;
      }
      auto it = storedValues.find(c.source);
      if (it != storedValues.end()) {
        sort.container = &_storedValuesBuffer;
        sort.multiplier = storedValuesCount;
        sort.offset =
            static_cast<size_t>(std::distance(storedValues.begin(), it));
      } else {
        sort.container = &_heapOnlyStoredValuesBuffer;
        sort.offset = j++;
        ++_heapOnlyColumnsCount;
      }
    }
    for (auto& c : _heapSort) {
      if (c.container == &_heapOnlyStoredValuesBuffer) {
        c.multiplier = _heapOnlyColumnsCount;
      }
    }
  }

  irs::score_t* pushNoneScores();

  template<typename T, typename Id>
  void makeValue(T idx, iresearch::ViewSegment const& segment, Id id) {
    if constexpr (std::is_same_v<T, PushTag>) {
      _keyBuffer.emplace_back(segment, id);
    } else {
      _keyBuffer[idx] = ValueType{segment, id};
    }
  }

  template<typename T>
  void makeSearchDoc(T idx, iresearch::ViewSegment const& segment,
                     irs::doc_id_t docId) {
    if constexpr (std::is_same_v<T, PushTag>) {
      _searchDocs.emplace_back(segment, docId);
    } else {
      _searchDocs[idx] = iresearch::SearchDoc{segment, docId};
    }
  }

  template<typename ColumnReaderProvider>
  void pushSortedValue(ColumnReaderProvider& columnReader, ValueType&& value,
                       irs::score_t const* scores, irs::score const& score,
                       irs::score_t& threshold);

  void finalizeHeapSort(size_t skip);
  // A note on the scores: instead of saving an array of AqlValues, we could
  // save an array of floats plus a bitfield noting which entries should be None

  void reset() noexcept {
    // Should only be called after everything was consumed
    TRI_ASSERT(empty());
    clear();
  }

  void clear() noexcept;

  size_t size() const noexcept;

  bool empty() const noexcept { return size() == 0; }

  size_t popFront() noexcept;

  void skip(size_t count) noexcept { _keyBaseIdx += count; }

  // This is violated while documents and scores are pushed, but must hold
  // before and after.
  void assertSizeCoherence() const noexcept;

  size_t memoryUsage(size_t maxSize, size_t stored) const noexcept {
    auto res = maxSize * sizeof(_keyBuffer[0]) +
               maxSize * _numScores * sizeof(_scoreBuffer[0]) +
               maxSize * sizeof(_searchDocs[0]) +
               maxSize * stored * sizeof(_storedValuesBuffer[0]);
    if (auto sortSize = _heapSort.size(); sortSize != 0) {
      res += maxSize * _heapOnlyColumnsCount *
             sizeof(_heapOnlyStoredValuesBuffer[0]);
      res += _heapOnlyColumnsCount * sizeof(_currentDocumentBuffer[0]);
      res += maxSize * sortSize * sizeof(_heapSortValues[0]);
    }
    return res;
  }

  void resizeTo(size_t atMost, size_t stored) {
    _keyBuffer.resize(atMost);
    _searchDocs.resize(atMost);
    _scoreBuffer.resize(atMost * _numScores);
    _storedValuesBuffer.resize(atMost * stored);
  }

  void preAllocateStoredValuesBuffer(size_t atMost, size_t stored,
                                     bool parallelAccess = false) {
    TRI_ASSERT(_storedValuesBuffer.empty());
    auto heapSize = _heapSort.size();
    parallelAccess |= heapSize != 0;
    if (_keyBuffer.capacity() < atMost) {
      auto newMemoryUsage = memoryUsage(atMost, stored);
      auto tracked = _memoryTracker.tracked();
      if (newMemoryUsage > tracked) {
        _memoryTracker.increase(newMemoryUsage - tracked);
      }
      if (!parallelAccess) {
        _keyBuffer.reserve(atMost);
        _searchDocs.reserve(atMost);
        _scoreBuffer.reserve(atMost * _numScores);
        _storedValuesBuffer.reserve(atMost * stored);
      }
    }
    if (parallelAccess) {
      resizeTo(atMost, stored);
    }
    if (heapSize != 0) {
      if (_numScores == 0) {  // save ourselves 1 "if" during pushing values
        _scoreBuffer.emplace_back();
      }
      _heapSortValues.resize(atMost * heapSize);
      _currentDocumentBuffer.reserve(heapSize);
      _heapOnlyStoredValuesBuffer.resize(atMost * _heapOnlyColumnsCount);
      _heapSizeLeft = atMost;
    } else {
      _heapSizeLeft = 0;
    }
  }

  std::vector<size_t> getMaterializeRange(size_t skip) const;

  template<typename T>
  void makeStoredValue(T idx, irs::bytes_view value) {
    if constexpr (std::is_same_v<T, PushTag>) {
      TRI_ASSERT(_storedValuesBuffer.size() < _storedValuesBuffer.capacity());
      _storedValuesBuffer.emplace_back(value.data(), value.size());
    } else {
      TRI_ASSERT(idx < _storedValuesBuffer.size());
      _storedValuesBuffer[idx] = value;
    }
  }

  using StoredValue =
      std::conditional_t<copyStored, irs::bstring, irs::bytes_view>;

  using StoredValues = std::vector<StoredValue>;

  StoredValues const& getStoredValues() const noexcept {
    return _storedValuesBuffer;
  }

  struct BufferHeapSortElement : iresearch::HeapSortElement {
    StoredValues* container{};
    size_t offset{};
    size_t multiplier{};
  };

 private:
  template<typename ColumnReaderProvider>
  velocypack::Slice readHeapSortColumn(iresearch::HeapSortElement const& cmp,
                                       irs::doc_id_t doc,
                                       ColumnReaderProvider& readerProvider,
                                       size_t readerSlot);
  template<typename ColumnReaderProvider>
  void finalizeHeapSortDocument(size_t idx, irs::doc_id_t doc,
                                irs::score_t const* scores,
                                ColumnReaderProvider& readerProvider);

  std::vector<ValueType> _keyBuffer;
  // FIXME(gnusi): compile time
  std::vector<iresearch::SearchDoc> _searchDocs;
  std::vector<irs::score_t> _scoreBuffer;
  StoredValues _storedValuesBuffer;

  // Heap Sort facilities
  // atMost * <num heap only values>
  StoredValues _heapOnlyStoredValuesBuffer;
  // <num heap only values>
  std::vector<ColumnIterator> _heapOnlyStoredValuesReaders;
  // <num heap only values>
  StoredValues _currentDocumentBuffer;
  IRS_NO_UNIQUE_ADDRESS
  irs::utils::Need<!copyStored, std::vector<velocypack::Slice>>
      _currentDocumentSlices;

  std::vector<BufferHeapSortElement> _heapSort;
  std::vector<size_t> _rows;
  std::vector<HeapSortValue> _heapSortValues;

  size_t _heapOnlyColumnsCount{0};
  size_t _currentReaderOffset{std::numeric_limits<size_t>::max()};

  size_t _numScores;
  size_t _keyBaseIdx{0};
  size_t _heapSizeLeft{0};
  ResourceUsageScope _memoryTracker;
};

template<bool copyStored, bool ordered, bool emitSearchDoc,
         iresearch::MaterializeType materializeType>
struct ExecutionTraits {
  static constexpr bool EmitSearchDoc = emitSearchDoc;
  static constexpr bool Ordered = ordered;
  static constexpr bool CopyStored = copyStored;
  static constexpr iresearch::MaterializeType MaterializeType = materializeType;
};

template<typename Impl>
struct IResearchViewExecutorTraits;

template<typename Impl, typename ExecutionTraits>
class IResearchViewExecutorBase {
 public:
  struct Traits : ExecutionTraits, IResearchViewExecutorTraits<Impl> {};

  struct Properties {
    // even with "ordered = true", this block preserves the order; it just
    // writes scorer information in additional register for a following sort
    // block to use.
    static constexpr bool preservesOrder = true;
    static constexpr BlockPassthrough allowsBlockPassthrough =
        BlockPassthrough::Disable;
  };
  using Fetcher = SingleRowFetcher<Properties::allowsBlockPassthrough>;
  using Infos = IResearchViewExecutorInfos;
  using Stats = IResearchViewStats;

  static consteval bool contains(auto type) noexcept {
    return static_cast<uint32_t>(Traits::MaterializeType & type) ==
           static_cast<uint32_t>(type);
  }

  static constexpr bool isLateMaterialized =
      contains(iresearch::MaterializeType::LateMaterialize);

  static constexpr bool isMaterialized =
      contains(iresearch::MaterializeType::Materialize);

  static constexpr bool usesStoredValues =
      contains(iresearch::MaterializeType::UseStoredValues);

  using IndexReadBufferType =
      IndexReadBuffer<typename Traits::IndexBufferValueType,
                      Traits::CopyStored>;
  /**
   * @brief produce the next Rows of Aql Values.
   *
   * @return ExecutorState, the stats, and a new Call that needs to be send to
   * upstream
   */
  [[nodiscard]] std::tuple<ExecutorState, Stats, AqlCall> produceRows(
      AqlItemBlockInputRange& input, OutputAqlItemRow& output);

  /**
   * @brief skip the next Row of Aql Values.
   *
   * @return ExecutorState, the stats, and a new Call that needs to be send to
   * upstream
   */
  [[nodiscard]] std::tuple<ExecutorState, Stats, size_t, AqlCall> skipRowsRange(
      AqlItemBlockInputRange& inputRange, AqlCall& call);

  void initializeCursor();

  IResearchViewExecutorBase() = delete;
  IResearchViewExecutorBase(IResearchViewExecutorBase const&) = delete;

 protected:
  class ReadContext {
   public:
    explicit ReadContext(RegisterId documentOutReg, InputAqlItemRow& inputRow,
                         OutputAqlItemRow& outputRow);

    InputAqlItemRow& inputRow;
    OutputAqlItemRow& outputRow;

    auto getDocumentIdReg() const noexcept {
      static_assert(isLateMaterialized);
      return documentOutReg;
    }

    void moveInto(aql::DocumentData data) noexcept;

   private:
    RegisterId documentOutReg;
  };

  IResearchViewExecutorBase(IResearchViewExecutorBase&&) = default;
  IResearchViewExecutorBase(Fetcher& fetcher, Infos&);
  ~IResearchViewExecutorBase() = default;

  void fillScores(irs::score const& score) {
    TRI_ASSERT(Traits::Ordered);

    // Scorer registers are placed right before document output register.
    // Allocate block for scores (registerId's are sequential) and fill it.
    score(_indexReadBuffer.pushNoneScores());
  }

  template<typename Value>
  bool writeRowImpl(ReadContext& ctx, size_t idx, Value const& value);

  void writeSearchDoc(ReadContext& ctx, iresearch::SearchDoc const& doc,
                      RegisterId reg);

  void reset();

  bool writeStoredValue(
      ReadContext& ctx,
      typename IndexReadBufferType::StoredValues const& storedValues,
      size_t index, FieldRegisters const& fieldsRegs);
  bool writeStoredValue(ReadContext& ctx, irs::bytes_view storedValue,
                        FieldRegisters const& fieldsRegs);

  template<typename T>
  void makeStoredValues(T idx, irs::doc_id_t docId, size_t readerIndex);

  bool getStoredValuesReaders(irs::SubReader const& segmentReader,
                              size_t readerIndex);

  transaction::Methods _trx;
  iresearch::MonitorManager _memory;
  AqlFunctionsInternalCache _aqlFunctionsInternalCache;
  Infos& _infos;
  InputAqlItemRow _inputRow;
  IndexReadBufferType _indexReadBuffer;
  IRS_NO_UNIQUE_ADDRESS
  irs::utils::Need<isMaterialized, MultiGetContext> _context;
  iresearch::ViewExpressionContext _ctx;
  FilterCtx _filterCtx;
  iresearch::ViewSnapshotPtr _reader;
  irs::proxy_filter::cache_ptr _cache;
  irs::filter::prepared::ptr _filter = irs::filter::prepared::empty();
  irs::filter::prepared const** _filterCookie{};
  containers::SmallVector<irs::Scorer::ptr, 2> _scorersContainer;
  irs::Scorers _scorers;
  irs::WandContext _wand;
  std::vector<ColumnIterator> _storedValuesReaders;
  containers::FlatHashSet<ptrdiff_t> _storedColumnsMask;
  std::array<char, iresearch::kSearchDocBufSize> _buf;
  bool _isInitialized{};
  bool _isMaterialized{};
  // new mangling only:
  iresearch::AnalyzerProvider _provider;

 private:
  void materialize();
  bool next(ReadContext& ctx, IResearchViewStats& stats);
};

template<typename ExecutionTraits>
class IResearchViewExecutor
    : public IResearchViewExecutorBase<IResearchViewExecutor<ExecutionTraits>,
                                       ExecutionTraits> {
 public:
  using Base = IResearchViewExecutorBase<IResearchViewExecutor<ExecutionTraits>,
                                         ExecutionTraits>;
  using Fetcher = typename Base::Fetcher;
  using Infos = typename Base::Infos;

  static constexpr bool kSorted = false;

  IResearchViewExecutor(IResearchViewExecutor&&) = default;
  IResearchViewExecutor(Fetcher& fetcher, Infos&);
  ~IResearchViewExecutor();

 private:
  friend Base;
  using ReadContext = typename Base::ReadContext;

  struct SegmentReader {
    void finalize() {
      itr.reset();
      doc = nullptr;
      currentSegmentPos = 0;
    }

    // current primary key reader
    ColumnIterator pkReader;
    irs::doc_iterator::ptr itr;
    irs::document const* doc{};
    size_t readerOffset{0};
    // current document iterator position in segment
    size_t currentSegmentPos{0};
    // total position for full snapshot
    size_t totalPos{0};
    size_t atMost{0};
    LogicalCollection const* collection{};
    iresearch::ViewSegment const* segment{};
    irs::score const* scr{&irs::score::kNoScore};
  };

  size_t skip(size_t toSkip, IResearchViewStats&);
  size_t skipAll(IResearchViewStats&);

  bool fillBuffer(ReadContext& ctx);

  template<bool parallel>
  bool readSegment(SegmentReader& reader, std::atomic_size_t& bufferIdxGlobal);

  bool writeRow(ReadContext& ctx, size_t idx);

  bool resetIterator(SegmentReader& reader);

  void reset(bool needFullCount);

  // Returns true unless the iterator is exhausted. documentId will always be
  // written. It will always be unset when readPK returns false, but may also be
  // unset if readPK returns true.
  static bool readPK(LocalDocumentId& documentId, SegmentReader& reader);

  std::vector<SegmentReader> _segmentReaders;
  size_t _segmentOffset;
  uint64_t _allocatedThreads{0};
  uint64_t _demandedThreads{0};
};

union DocumentValue {
  irs::doc_id_t docId;
  LocalDocumentId id{};
  size_t result;
};

struct ExecutorValue {
  ExecutorValue() = default;

  explicit ExecutorValue(iresearch::ViewSegment const& segment,
                         LocalDocumentId id) noexcept {
    translate(segment, id);
  }

  void translate(iresearch::ViewSegment const& segment,
                 LocalDocumentId id) noexcept {
    _value.id = id;
    _reader.segment = &segment;
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
    TRI_ASSERT(_state == State::IResearch);
    _state = State::RocksDB;
#endif
  }

  void translate(size_t i) noexcept {
    _value.result = i;
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
    TRI_ASSERT(_state == State::RocksDB);
    _state = State::Executor;
#endif
  }

  [[nodiscard]] iresearch::ViewSegment const* segment() const noexcept {
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
    TRI_ASSERT(_state != State::IResearch);
#endif
    return _reader.segment;
  }

  [[nodiscard]] auto const& value() const noexcept {
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
    TRI_ASSERT(_state != State::IResearch);
#endif
    return _value;
  }

 protected:
  DocumentValue _value;
  union {
    size_t offset;
    iresearch::ViewSegment const* segment;
  } _reader;
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
  enum class State {
    IResearch = 0,
    RocksDB,
    Executor,
  } _state{};
#endif
};

template<typename ExecutionTraits>
struct IResearchViewExecutorTraits<IResearchViewExecutor<ExecutionTraits>> {
  using IndexBufferValueType =
      std::conditional_t<(ExecutionTraits::MaterializeType &
                          iresearch::MaterializeType::LateMaterialize) ==
                             iresearch::MaterializeType::LateMaterialize,
                         iresearch::SearchDoc, ExecutorValue>;
  static constexpr bool ExplicitScanned = false;
};

template<typename ExecutionTraits>
class IResearchViewMergeExecutor
    : public IResearchViewExecutorBase<
          IResearchViewMergeExecutor<ExecutionTraits>, ExecutionTraits> {
 public:
  using Base =
      IResearchViewExecutorBase<IResearchViewMergeExecutor<ExecutionTraits>,
                                ExecutionTraits>;
  using Fetcher = typename Base::Fetcher;
  using Infos = typename Base::Infos;

  static constexpr bool Ordered = ExecutionTraits::Ordered;
  static constexpr bool kSorted = true;

  IResearchViewMergeExecutor(IResearchViewMergeExecutor&&) = default;
  IResearchViewMergeExecutor(Fetcher& fetcher, Infos&);

 private:
  friend Base;
  using ReadContext = typename Base::ReadContext;

  struct Segment {
    Segment(irs::doc_iterator::ptr&& docs, irs::document const& doc,
            irs::score const& score, irs::doc_iterator::ptr&& pkReader,
            size_t index, irs::doc_iterator* sortReaderRef,
            irs::payload const* sortReaderValue,
            irs::doc_iterator::ptr&& sortReader) noexcept;
    Segment(Segment const&) = delete;
    Segment(Segment&&) = default;
    Segment& operator=(Segment const&) = delete;
    Segment& operator=(Segment&&) = delete;

    irs::doc_iterator::ptr docs;
    irs::document const* doc{};
    irs::score const* score{};
    ColumnIterator pkReader;
    size_t segmentIndex;  // first stored values index
    irs::doc_iterator* sortReaderRef;
    irs::payload const* sortValue;
    irs::doc_iterator::ptr sortReader;
    size_t segmentPos{0};
  };

  class MinHeapContext {
   public:
    using Value = Segment;

    MinHeapContext(iresearch::IResearchSortBase const& sort,
                   size_t sortBuckets) noexcept;

    // advance
    bool operator()(Value& segment) const;

    // compare
    bool operator()(const Value& lhs, const Value& rhs) const;

   private:
    iresearch::VPackComparer<iresearch::IResearchSortBase> _less;
  };

  // reads local document id from a specified segment
  LocalDocumentId readPK(Segment& segment);

  bool fillBuffer(ReadContext& ctx);

  bool writeRow(ReadContext& ctx, size_t idx);

  void reset(bool needFullCount);
  size_t skip(size_t toSkip, IResearchViewStats&);
  size_t skipAll(IResearchViewStats&);

  std::vector<Segment> _segments;
  irs::ExternalMergeIterator<MinHeapContext> _heap_it;
};

template<typename ExecutionTraits>
struct IResearchViewExecutorTraits<
    IResearchViewMergeExecutor<ExecutionTraits>> {
  using IndexBufferValueType =
      std::conditional_t<(ExecutionTraits::MaterializeType &
                          iresearch::MaterializeType::LateMaterialize) ==
                             iresearch::MaterializeType::LateMaterialize,
                         iresearch::SearchDoc, ExecutorValue>;
  static constexpr bool ExplicitScanned = false;
};

template<typename ExecutionTraits>
class IResearchViewHeapSortExecutor
    : public IResearchViewExecutorBase<
          IResearchViewHeapSortExecutor<ExecutionTraits>, ExecutionTraits> {
 public:
  using Base =
      IResearchViewExecutorBase<IResearchViewHeapSortExecutor<ExecutionTraits>,
                                ExecutionTraits>;
  using Fetcher = typename Base::Fetcher;
  using Infos = typename Base::Infos;

  static constexpr bool kSorted = true;

  IResearchViewHeapSortExecutor(IResearchViewHeapSortExecutor&&) = default;
  IResearchViewHeapSortExecutor(Fetcher& fetcher, Infos&);

 private:
  friend Base;
  using ReadContext = typename Base::ReadContext;

  size_t skip(size_t toSkip, IResearchViewStats& stats);
  size_t skipAll(IResearchViewStats& stats);
  size_t getScanned() const noexcept { return _totalCount; }
  bool canSkipAll() const noexcept { return _bufferFilled && _totalCount; }

  void reset(bool needFullCount);
  bool fillBuffer(ReadContext& ctx);
  bool fillBufferInternal(size_t skip);

  bool writeRow(ReadContext& ctx, size_t idx);

  size_t _totalCount{};
  size_t _scannedCount{};
  size_t _bufferedCount{};
  bool _bufferFilled{};
};

struct HeapSortExecutorValue : ExecutorValue {
  using ExecutorValue::ExecutorValue;

  explicit HeapSortExecutorValue(size_t offset, irs::doc_id_t docId) noexcept {
    _value.docId = docId;
    _reader.offset = offset;
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
    _state = State::IResearch;
#endif
  }

  [[nodiscard]] size_t readerOffset() const noexcept {
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
    TRI_ASSERT(_state == State::IResearch);
#endif
    return _reader.offset;
  }

  [[nodiscard]] irs::doc_id_t docId() const noexcept {
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
    TRI_ASSERT(_state == State::IResearch);
#endif
    return _value.docId;
  }
};

#ifndef ARANGODB_ENABLE_MAINTAINER_MODE
static_assert(sizeof(HeapSortExecutorValue) <= 16,
              "HeapSortExecutorValue size is not optimal");
#endif

template<typename ExecutionTraits>
struct IResearchViewExecutorTraits<
    IResearchViewHeapSortExecutor<ExecutionTraits>> {
  using IndexBufferValueType = HeapSortExecutorValue;
  static constexpr bool ExplicitScanned = true;
};

}  // namespace aql
}  // namespace arangodb
