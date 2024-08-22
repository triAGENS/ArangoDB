target_sources(arango_replication2 PRIVATE
  ActiveTransactionsQueue.cpp ActiveTransactionsQueue.h
  CollectionReader.cpp CollectionReader.h
  CreateIndexReplicationCallback.h
  DocumentCore.cpp DocumentCore.h
  DocumentStateErrorHandler.cpp DocumentStateErrorHandler.h
  DocumentFollowerState.cpp DocumentFollowerState.h
  DocumentLeaderState.cpp DocumentLeaderState.h
  DocumentLogEntry.cpp DocumentLogEntry.h
  DocumentStateHandlersFactory.cpp DocumentStateHandlersFactory.h
  DocumentStateMachine.cpp DocumentStateMachine.h
  DocumentStateMachineFeature.cpp DocumentStateMachineFeature.h
  DocumentStateMethods.cpp DocumentStateMethods.h
  DocumentStateNetworkHandler.cpp DocumentStateNetworkHandler.h
  DocumentStateShardHandler.cpp DocumentStateShardHandler.h
  DocumentStateSnapshot.cpp DocumentStateSnapshot.h
  DocumentStateSnapshotHandler.cpp DocumentStateSnapshotHandler.h
  DocumentStateTransaction.cpp DocumentStateTransaction.h
  DocumentStateTransactionHandler.cpp DocumentStateTransactionHandler.h
  LowestSafeIndexesForReplay.cpp LowestSafeIndexesForReplay.h
  LowestSafeIndexesForReplayUtils.cpp LowestSafeIndexesForReplayUtils.h
  MaintenanceActionExecutor.cpp MaintenanceActionExecutor.h
  ReplicatedOperation.cpp ReplicatedOperation.h
  ReplicatedOperationInspectors.h
)

add_subdirectory(Actor)
