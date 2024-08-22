target_sources(arango_replication2 PRIVATE
  AgencySpecificationInspectors.cpp
  Algorithms.cpp
  ILogInterfaces.cpp
  NetworkAttachedFollower.cpp
  NetworkMessages.cpp
  ReplicatedLog.cpp
  SupervisionAgencyTrx.cpp
  ReplicatedLogFeature.cpp
  ReplicatedLogMetrics.tpp
  DefaultRebootIdCache.cpp DefaultRebootIdCache.h
  )

target_sources(arango_replication2_pure PRIVATE
  AgencyLogSpecification.cpp
  Algorithms.cpp
  ILogInterfaces.cpp
  InMemoryLog.cpp
  InMemoryLogEntry.cpp
  IRebootIdCache.h
  LogCommon.cpp
  LogEntry.cpp
  LogEntryView.cpp
  LogLeader.cpp
  LogMetaPayload.cpp
  LogPayload.cpp
  LogStatus.cpp
  NetworkMessages.cpp
  ReplicatedLog.cpp
  ReplicatedLogMetrics.tpp
  Supervision.cpp
  SupervisionAction.cpp
  TermIndexMapping.cpp
  types.cpp
  WaitForBag.cpp
  Components/AppendEntriesManager.cpp Components/AppendEntriesManager.h
  Components/CompactionManager.cpp Components/CompactionManager.h
  Components/ExclusiveBool.h
  Components/FollowerCommitManager.cpp Components/FollowerCommitManager.h
  Components/IAppendEntriesManager.h
  Components/ICompactionManager.h
  Components/IFollowerCommitManager.h
  Components/IInMemoryLogManager.h
  Components/IMessageIdManager.h
  Components/IMethodsProvider.h
  Components/ISnapshotManager.h
  Components/IStateHandleManager.h
  Components/IStateMetadataTransaction.h
  Components/IStorageManager.h
  Components/InMemoryLogManager.cpp Components/InMemoryLogManager.h
  Components/LogFollower.cpp Components/LogFollower.h
  Components/MessageIdManager.cpp Components/MessageIdManager.h
  Components/MethodsProvider.cpp Components/MethodsProvider.h
  Components/SnapshotManager.cpp Components/SnapshotManager.h
  Components/StateHandleManager.cpp Components/StateHandleManager.h
  Components/StateMetadataTransaction.cpp Components/StateMetadataTransaction.h
  Components/StorageManager.cpp Components/StorageManager.h
  )
