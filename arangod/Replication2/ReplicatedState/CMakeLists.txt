target_sources(arango_replication2 PRIVATE
  ReplicatedStateFeature.cpp
  ReplicatedStateMetrics.cpp
  StateCommon.cpp
  WaitForQueue.cpp
)

target_sources(arango_replication2_pure PRIVATE
  StateStatus.h
  StateCommon.cpp StateCommon.h
  StateInterfaces.tpp StateInterfaces.h
)
