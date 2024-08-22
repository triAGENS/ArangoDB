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

#include "StreamProxy.h"

#include "Replication2/Exceptions/ParticipantResignedException.h"
#include "Replication2/ReplicatedState/LazyDeserializingIterator.h"
#include "Replication2/Streams/StreamSpecification.h"
#include "Replication2/Streams/IMetadataTransaction.h"

namespace arangodb::replication2::replicated_state {

#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
namespace document {
struct DocumentStateMetadata;
}
#endif

template<typename T>
struct MetadataTransactionImpl : streams::IMetadataTransaction<T> {
  using MetadataType = T;
  explicit MetadataTransactionImpl(
      std::unique_ptr<replicated_log::IStateMetadataTransaction> trx);

#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
  ~MetadataTransactionImpl() override {
    if constexpr (std::is_same_v<T, document::DocumentStateMetadata>) {
      if (_trx != nullptr) {
        LOG_DEVEL << "Losing metadata transaction before commit; metadata was "
                  << _metadata.lowestSafeIndexesForReplay;
      }
    }
  }
#endif

  auto
  destruct() && -> std::unique_ptr<replicated_log::IStateMetadataTransaction>;

  auto get() noexcept -> MetadataType& override;

 private:
  std::unique_ptr<replicated_log::IStateMetadataTransaction> _trx;
  MetadataType _metadata;
};
template<typename T>
auto MetadataTransactionImpl<T>::get() noexcept -> MetadataType& {
  return _metadata;
}

template<typename T>
MetadataTransactionImpl<T>::MetadataTransactionImpl(
    std::unique_ptr<replicated_log::IStateMetadataTransaction> trx)
    : _trx(std::move(trx)),
      _metadata(
          velocypack::deserialize<MetadataType>(_trx->get().slice.slice())) {
  // _metadata now owns the data, until destruction, where it gets serialized
  // again; so we release the slice now.
  _trx->get().slice = velocypack::SharedSlice{};
}

template<typename T>
auto MetadataTransactionImpl<T>::destruct() && -> std::unique_ptr<
    replicated_log::IStateMetadataTransaction> {
  auto builder = velocypack::Builder();
  velocypack::serialize(builder, _metadata);
  _trx->get().slice = builder.sharedSlice();
  return std::move(_trx);
}

template<typename S, template<typename> typename Interface,
         ValidStreamLogMethods ILogMethodsT>
StreamProxy<S, Interface, ILogMethodsT>::StreamProxy(
    std::unique_ptr<ILogMethodsT> methods)
    : _logMethods(std::move(methods)) {}

template<typename S, template<typename> typename Interface,
         ValidStreamLogMethods ILogMethodsT>
auto StreamProxy<S, Interface, ILogMethodsT>::methods() -> MethodsGuard {
  return MethodsGuard{this->_logMethods.getLockedGuard()};
}

template<typename S, template<typename> typename Interface,
         ValidStreamLogMethods ILogMethodsT>
auto StreamProxy<S, Interface,
                 ILogMethodsT>::resign() && -> std::unique_ptr<ILogMethodsT> {
  // cppcheck-suppress returnStdMoveLocal ; bogus
  return std::move(this->_logMethods.getLockedGuard().get());
}

template<typename S, template<typename> typename Interface,
         ValidStreamLogMethods ILogMethodsT>
auto StreamProxy<S, Interface, ILogMethodsT>::isResigned() const noexcept
    -> bool {
  return _logMethods.getLockedGuard().get() == nullptr;
}

template<typename S, template<typename> typename Interface,
         ValidStreamLogMethods ILogMethodsT>
auto StreamProxy<S, Interface, ILogMethodsT>::waitFor(LogIndex index)
    -> futures::Future<WaitForResult> {
  // TODO We might want to remove waitFor here, in favor of updateCommitIndex.
  //      It's currently used by DocumentLeaderState::replicateOperation.
  //      If this is removed, delete it also in streams::Stream.
  auto guard = _logMethods.getLockedGuard();
  return guard.get()->waitFor(index).thenValue(
      [](auto const&) { return WaitForResult(); });
}

template<typename S, template<typename> typename Interface,
         ValidStreamLogMethods ILogMethodsT>
auto StreamProxy<S, Interface, ILogMethodsT>::waitForIterator(LogIndex index)
    -> futures::Future<std::unique_ptr<Iterator>> {
  // TODO As far as I can tell right now, we can get rid of this, but for the
  //      PrototypeState (currently). So:
  //      Delete this, also in streams::Stream.
  auto guard = _logMethods.getLockedGuard();
  return guard.get()->waitForIterator(index).thenValue([](auto&& logIter) {
    std::unique_ptr<Iterator> deserializedIter =
        std::make_unique<LazyDeserializingIterator<EntryType, Deserializer>>(
            std::move(logIter));
    return deserializedIter;
  });
}

template<typename S, template<typename> typename Interface,
         ValidStreamLogMethods ILogMethodsT>
auto StreamProxy<S, Interface, ILogMethodsT>::release(LogIndex index) -> void {
  auto guard = _logMethods.getLockedGuard();
  if (auto& methods = guard.get(); methods != nullptr) [[likely]] {
    methods->releaseIndex(index);
  } else {
    throwResignedException();
  }
}

template<typename S, template<typename> typename Interface,
         ValidStreamLogMethods ILogMethodsT>
auto StreamProxy<S, Interface, ILogMethodsT>::beginMetadataTrx()
    -> std::unique_ptr<streams::IMetadataTransaction<MetadataType>> {
  auto guard = _logMethods.getLockedGuard();
  if (auto& methods = guard.get(); methods != nullptr) [[likely]] {
    auto trx = methods->beginMetadataTrx();
    return std::make_unique<MetadataTransactionImpl<MetadataType>>(
        std::move(trx));
  } else {
    throwResignedException();
  }
}
template<typename S, template<typename> typename Interface,
         ValidStreamLogMethods ILogMethodsT>
auto StreamProxy<S, Interface, ILogMethodsT>::commitMetadataTrx(
    std::unique_ptr<streams::IMetadataTransaction<MetadataType>> ptr)
    -> Result {
  auto guard = _logMethods.getLockedGuard();
  if (auto& methods = guard.get(); methods != nullptr) [[likely]] {
    auto trx =
        std::move(dynamic_cast<MetadataTransactionImpl<MetadataType>&>(*ptr))
            .destruct();
    return methods->commitMetadataTrx(std::move(trx));
  } else {
    throwResignedException();
  }
}
template<typename S, template<typename> typename Interface,
         ValidStreamLogMethods ILogMethodsT>
auto StreamProxy<S, Interface, ILogMethodsT>::getCommittedMetadata() const
    -> MetadataType {
  auto guard = _logMethods.getLockedGuard();
  if (auto& methods = guard.get(); methods != nullptr) [[likely]] {
    auto data = methods->getCommittedMetadata();
    return velocypack::deserialize<MetadataType>(data.slice.slice());
  } else {
    throwResignedException();
  }
}

template<typename S, template<typename> typename Interface,
         ValidStreamLogMethods ILogMethodsT>
void StreamProxy<S, Interface, ILogMethodsT>::throwResignedException() {
  static constexpr auto errorCode = ([]() consteval->ErrorCode {
    if constexpr (std::is_same_v<ILogMethodsT,
                                 replicated_log::IReplicatedLogLeaderMethods>) {
      return TRI_ERROR_REPLICATION_REPLICATED_LOG_LEADER_RESIGNED;
    } else if constexpr (std::is_same_v<
                             ILogMethodsT,
                             replicated_log::IReplicatedLogFollowerMethods>) {
      return TRI_ERROR_REPLICATION_REPLICATED_LOG_FOLLOWER_RESIGNED;
    } else {
      []<bool flag = false>() {
        static_assert(flag,
                      "Unhandled log methods. This should have been "
                      "prevented by the concept ValidStreamLogMethods.");
      }
      ();
      ADB_PROD_ASSERT(false);
    }
  })();

  // The DocumentStates do not synchronize calls to `release()` or `insert()` on
  // the stream with resigning (see
  // https://github.com/arangodb/arangodb/pull/17850).
  // It relies on the stream throwing an exception in that case.
  throw replicated_log::ParticipantResignedException(errorCode);
}

template<typename S>
ProducerStreamProxy<S>::ProducerStreamProxy(
    std::unique_ptr<replicated_log::IReplicatedLogLeaderMethods> methods)
    : StreamProxy<S, streams::ProducerStream,
                  replicated_log::IReplicatedLogLeaderMethods>(
          std::move(methods)) {
  // this produces a lock inversion
  // ADB_PROD_ASSERT(this->_logMethods.getLockedGuard().get() != nullptr);
}

template<typename S>
auto ProducerStreamProxy<S>::insert(EntryType const& v, bool waitForSync)
    -> LogIndex {
  auto guard = this->_logMethods.getLockedGuard();
  if (auto& methods = guard.get(); methods != nullptr) [[likely]] {
    return methods->insert(serialize(v), waitForSync);
  } else {
    this->throwResignedException();
  }
}

template<typename S>
auto ProducerStreamProxy<S>::serialize(EntryType const& v) -> LogPayload {
  auto builder = velocypack::Builder();
  std::invoke(Serializer{}, streams::serializer_tag<EntryType>, v, builder);
  return LogPayload{*builder.steal()};
}

}  // namespace arangodb::replication2::replicated_state
