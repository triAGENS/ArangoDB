#include "LoadingState.h"

#include "Metrics/Gauge.h"
#include "Pregel/Conductor/Conductor.h"
#include "Pregel/MasterContext.h"
#include "Pregel/Messaging/Message.h"
#include "Pregel/Messaging/WorkerMessages.h"
#include "Pregel/PregelFeature.h"

using namespace arangodb::pregel::conductor;

Loading::Loading(Conductor& conductor) : conductor{conductor} {
  conductor._timing.loading.start();
  conductor._feature.metrics()->pregelConductorsLoadingNumber->fetch_add(1);
}

Loading::~Loading() {
  conductor._timing.loading.finish();
  conductor._feature.metrics()->pregelConductorsLoadingNumber->fetch_sub(1);
}

auto Loading::run() -> std::optional<std::unique_ptr<State>> {
  LOG_PREGEL_CONDUCTOR_STATE("3a255", DEBUG)
      << "Telling workers to load the data";
  return _aggregate.doUnderLock(
      [&](auto& agg) -> std::optional<std::unique_ptr<State>> {
        auto aggregate = conductor._workers.loadGraph(LoadGraph{});
        if (aggregate.fail()) {
          LOG_PREGEL_CONDUCTOR_STATE("dddad", ERR) << aggregate.errorMessage();
          return std::make_unique<FatalError>(conductor);
        }
        agg = aggregate.get();
        return std::nullopt;
      });
}

auto Loading::receive(MessagePayload message)
    -> std::optional<std::unique_ptr<State>> {
  auto explicitMessage = getResultTMessage<GraphLoaded>(message);
  if (explicitMessage.fail()) {
    LOG_PREGEL_CONDUCTOR_STATE("7698e", ERR) << explicitMessage.errorMessage();
    return std::make_unique<FatalError>(conductor);
  }
  auto finishedAggregate =
      _aggregate.doUnderLock([message = std::move(explicitMessage).get()](
                                 auto& agg) { return agg.aggregate(message); });
  if (!finishedAggregate.has_value()) {
    return std::nullopt;
  }

  auto graphLoadedData = finishedAggregate.value();
  conductor._totalVerticesCount += graphLoadedData.vertexCount;
  conductor._totalEdgesCount += graphLoadedData.edgeCount;

  if (conductor._masterContext) {
    conductor._masterContext->initialize(conductor._totalVerticesCount,
                                         conductor._totalEdgesCount,
                                         conductor._aggregators.get());
  }
  return std::make_unique<Computing>(conductor);
}

auto Loading::cancel() -> std::optional<std::unique_ptr<State>> {
  return std::make_unique<Canceled>(conductor);
}
