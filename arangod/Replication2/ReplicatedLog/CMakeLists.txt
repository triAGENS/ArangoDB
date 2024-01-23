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
  Algorithms.cpp
  AppendEntriesErrorReason.cpp
  CommitFailReason.cpp
  CompactionResponse.cpp
  CompactionStopReason.cpp
  FollowerState.cpp
  GlobalLogIdentifier.cpp
  ILogInterfaces.cpp
  InMemoryLog.cpp
  InMemoryLogEntry.cpp
  IRebootIdCache.h
  LocalStateMachineStatus.cpp
  LogEntry.cpp
  LogEntryView.cpp
  LogId.cpp
  LogIndex.cpp
  LogLeader.cpp
  LogMetaPayload.cpp
  LogPayload.cpp
  LogRange.cpp
  LogStatistics.cpp
  LogStatus.cpp
  LogTerm.cpp
  NetworkMessages.cpp
  ParticipantFlags.cpp
  QuorumData.cpp
  ReplicatedLog.cpp
  ReplicatedLogMetrics.tpp
  Supervision.cpp
  SupervisionAction.cpp
  TermIndexPair.cpp
  TermIndexMapping.cpp
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
  Components/IStorageManager.h
  Components/InMemoryLogManager.cpp Components/InMemoryLogManager.h
  Components/LogFollower.cpp Components/LogFollower.h
  Components/MessageIdManager.cpp Components/MessageIdManager.h
  Components/MethodsProvider.cpp Components/MethodsProvider.h
  Components/SnapshotManager.cpp Components/SnapshotManager.h
  Components/StateHandleManager.cpp Components/StateHandleManager.h
  Components/StorageManager.cpp Components/StorageManager.h
  )

add_subdirectory(Agency)
