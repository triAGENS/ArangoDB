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
/// @author Lars Maier
////////////////////////////////////////////////////////////////////////////////
#pragma once

#include "Basics/Result.h"
#include "Replication2/ReplicatedLog/LogCommon.h"
#include "Replication2/ReplicatedLog/TypedLogIterator.h"
#include "Replication2/ReplicatedState/ReplicatedStateTraits.h"
#include "Replication2/Streams/IMetadataTransaction.h"
#include "Replication2/Streams/StreamSpecification.h"

#include <memory>

namespace arangodb {
struct DeferredAction;
}

namespace arangodb::futures {
template<typename>
class Future;
}

namespace arangodb::replication2::streams {

/**
 * Object returned by a stream iterator. Allows read only access
 * to the stored object. The view does not own the value and remains
 * valid until the iterator is destroyed or next() is called.
 * @tparam T Object Type
 */
template<typename T>
using StreamEntryView = std::pair<LogIndex, T const&>;
template<typename T>
using StreamEntry = std::pair<LogIndex, T>;

template<typename T>
struct IMetadataTransaction;

/**
 * Consumer interface for a multiplexed object stream. Provides methods for
 * interaction with the replicated logs stream.
 * @tparam T Object Type
 */
template<typename S>
struct Stream {
  using EntryType =
      typename replicated_state::ReplicatedStateTraits<S>::EntryType;
  using MetadataType =
      typename replicated_state::ReplicatedStateTraits<S>::MetadataType;
  virtual ~Stream() = default;

  struct WaitForResult {};
  virtual auto waitFor(LogIndex) -> futures::Future<WaitForResult> = 0;

  using Iterator = TypedLogRangeIterator<StreamEntryView<EntryType>>;
  virtual auto waitForIterator(LogIndex)
      -> futures::Future<std::unique_ptr<Iterator>> = 0;

  virtual auto release(LogIndex) -> void = 0;

  virtual auto beginMetadataTrx()
      -> std::unique_ptr<IMetadataTransaction<MetadataType>> = 0;
  virtual auto commitMetadataTrx(
      std::unique_ptr<IMetadataTransaction<MetadataType>> ptr) -> Result = 0;
  virtual auto getCommittedMetadata() const -> MetadataType = 0;
};

/**
 * Producing interface for a multiplexed object stream. Besides the Stream<T>
 * methods it additionally provides a insert method.
 * @tparam T Object Type
 */
template<typename S>
struct ProducerStream : Stream<S> {
  using EntryType =
      typename replicated_state::ReplicatedStateTraits<S>::EntryType;
  virtual auto insert(EntryType const&, bool waitForSync = false)
      -> LogIndex = 0;
};

/**
 * StreamGenericBase is the base for all Stream implementations. In general
 * users don't need to access this object directly. It provides more information
 * about the stream.
 * @tparam Descriptor The associated stream descriptor.
 * @tparam StreamType Either Stream<T> or ProducerStream<T>.
 * @tparam Type Object Type, default is extracted from Descriptor
 */
template<typename Descriptor, template<typename> typename StreamType,
         typename Type = stream_descriptor_type_t<Descriptor>>
struct StreamGenericBase : StreamType<Type> {
  static_assert(is_stream_descriptor_v<Descriptor>,
                "Descriptor is not a valid stream descriptor");

  using Iterator = typename StreamType<Type>::Iterator;
  virtual auto getAllEntriesIterator() -> std::unique_ptr<Iterator> = 0;
};

template<typename Descriptor>
using StreamBase = StreamGenericBase<Descriptor, Stream>;
template<typename Descriptor>
using ProducerStreamBase = StreamGenericBase<Descriptor, ProducerStream>;

template<typename, template<typename> typename>
struct StreamDispatcherBase;

/**
 * This class declares the general interface for an entity that provides a given
 * set of streams. It has the StreamBases as virtual base classes.
 * @tparam Streams
 * @tparam StreamType Either Stream<T> or ProducerStream<T>
 */
template<typename... Streams, template<typename> typename StreamType>
struct StreamDispatcherBase<stream_descriptor_set<Streams...>, StreamType>
    : virtual StreamGenericBase<Streams, StreamType>... {};

}  // namespace arangodb::replication2::streams
