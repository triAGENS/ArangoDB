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

#include <memory>
#include <unordered_map>
#include <utility>

#include "Inspection/VPack.h"
#include "Replication2/ReplicatedState/ReplicatedState.h"
#include "Replication2/ReplicatedState/ReplicatedStateTraits.h"
#include "Replication2/Storage/PersistedStateInfo.h"
#include "RestServer/arangod.h"

namespace arangodb::replication2::replicated_log {
struct ReplicatedLog;
class LogLeader;
}  // namespace arangodb::replication2::replicated_log

namespace arangodb::replication2::replicated_state {
struct ReplicatedStateMetrics;

struct ReplicatedStateFeature {
  /**
   * Registers a new State implementation with the given name.
   * @tparam S State Machine
   * @param name Name of the implementation
   * @param args Arguments forwarded to the constructor of the factory type,
   * i.e. ReplicatedStateTraits<S>::FactoryType.
   */
  template<typename S, typename... Args>
  void registerStateType(std::string_view name, Args&&... args) {
    using Factory = typename ReplicatedStateTraits<S>::FactoryType;
    static_assert(std::is_constructible_v<Factory, Args...>);
    auto factory = std::make_shared<InternalFactory<S, Factory>>(
        std::in_place, std::forward<Args>(args)...);
    auto metrics = createMetricsObjectIndirect(name);
    auto [iter, wasInserted] = implementations.try_emplace(
        std::string{name},
        StateImplementation{std::move(factory), std::move(metrics)});
    assertWasInserted(name, wasInserted);
  }

  auto getDefaultStateOwnedMetadata(std::string_view name)
      -> storage::StateOwnedMetadata;

  /**
   * Create a new replicated state using the implementation specified by the
   * name.
   * @param name Which implementation to use.
   * @param log ReplicatedLog to use.
   * @return
   */
  auto createReplicatedState(std::string_view name, std::string_view database,
                             LogId logId,
                             std::shared_ptr<replicated_log::ReplicatedLog> log,
                             std::shared_ptr<IScheduler> scheduler)
      -> std::shared_ptr<ReplicatedStateBase>;

  auto createReplicatedState(std::string_view name, std::string_view database,
                             LogId logId,
                             std::shared_ptr<replicated_log::ReplicatedLog> log,
                             LoggerContext const& loggerContext,
                             std::shared_ptr<IScheduler> scheduler)
      -> std::shared_ptr<ReplicatedStateBase>;

  virtual ~ReplicatedStateFeature() = default;

 protected:
  /*
   * This stunt is here to work around an internal compiler error that is
   * triggered when using GCC 11 on ARM, with LTO.
   */
#if (defined(__GNUC__) && !defined(__clang__))
#define ABD_CREATE_METRICS_OBJECT_NO_INLINE __attribute__((noinline))
#else
#define ABD_CREATE_METRICS_OBJECT_NO_INLINE
#endif
  auto ABD_CREATE_METRICS_OBJECT_NO_INLINE createMetricsObjectIndirect(
      std::string_view impl) -> std::shared_ptr<ReplicatedStateMetrics> {
    return createMetricsObject(impl);
  }
  virtual auto createMetricsObject(std::string_view impl)
      -> std::shared_ptr<ReplicatedStateMetrics>;

 private:
  static void assertWasInserted(std::string_view name, bool wasInserted);
  struct InternalFactoryBase
      : std::enable_shared_from_this<InternalFactoryBase> {
    virtual ~InternalFactoryBase() = default;
    virtual auto createReplicatedState(
        GlobalLogIdentifier, std::shared_ptr<replicated_log::ReplicatedLog>,
        LoggerContext, std::shared_ptr<ReplicatedStateMetrics>,
        std::shared_ptr<IScheduler>)
        -> std::shared_ptr<ReplicatedStateBase> = 0;
    virtual auto getDefaultStateOwnedMetadata()
        -> storage::StateOwnedMetadata = 0;
  };

  template<typename S, typename Factory>
  struct InternalFactory;

  struct StateImplementation {
    std::shared_ptr<InternalFactoryBase> factory;
    std::shared_ptr<ReplicatedStateMetrics> metrics;
  };

  std::unordered_map<std::string, StateImplementation> implementations;
};

template<typename S,
         typename Factory = typename ReplicatedStateTraits<S>::FactoryType>
struct ReplicatedStateFeature::InternalFactory : InternalFactoryBase,
                                                 private Factory {
  template<typename... Args>
  explicit InternalFactory(std::in_place_t, Args&&... args)
      : Factory(std::forward<Args>(args)...) {}

  auto createReplicatedState(GlobalLogIdentifier gid,
                             std::shared_ptr<replicated_log::ReplicatedLog> log,
                             LoggerContext loggerContext,
                             std::shared_ptr<ReplicatedStateMetrics> metrics,
                             std::shared_ptr<IScheduler> scheduler)
      -> std::shared_ptr<ReplicatedStateBase> override {
    return std::make_shared<ReplicatedState<S>>(
        std::move(gid), std::move(log), getStateFactory(),
        std::move(loggerContext), std::move(metrics), std::move(scheduler));
  }

  auto getStateFactory() -> std::shared_ptr<Factory> {
    return {shared_from_this(), static_cast<Factory*>(this)};
  }

  auto getDefaultStateOwnedMetadata() -> storage::StateOwnedMetadata override {
    using MetadataType = typename ReplicatedStateTraits<S>::MetadataType;
    auto defaultMetadata = MetadataType();
    auto sharedSlice = velocypack::serialize(defaultMetadata);
    return storage::StateOwnedMetadata{.slice = std::move(sharedSlice)};
  }
};

struct ReplicatedStateAppFeature : ArangodFeature, ReplicatedStateFeature {
  static constexpr std::string_view name() noexcept {
    return "ReplicatedState";
  }

 protected:
  auto createMetricsObject(std::string_view impl)
      -> std::shared_ptr<ReplicatedStateMetrics> override;

 public:
  explicit ReplicatedStateAppFeature(Server& server);
};

}  // namespace arangodb::replication2::replicated_state
