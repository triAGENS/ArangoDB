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
/// @author Manuel Pöter
////////////////////////////////////////////////////////////////////////////////

#pragma once

// This list of includes was deliberately moved to this file to avoid
// duplication between arangod/RestServer/arangod.cpp and tests/sepp/Server.cpp
// since both use ArangodServer and therefore need to know all the features.
// This also helps to avoid accidentally breaking the sepp build.

#include "Actions/ActionFeature.h"
#include "Agency/AgencyFeature.h"
#include "ApplicationFeatures/ApplicationServer.h"
#include "ApplicationFeatures/BumpFileDescriptorsFeature.h"
#include "ApplicationFeatures/CommunicationFeaturePhase.h"
#include "ApplicationFeatures/ConfigFeature.h"
#include "ApplicationFeatures/FileSystemFeature.h"
#include "ApplicationFeatures/GreetingsFeature.h"
#include "ApplicationFeatures/GreetingsFeaturePhase.h"
#include "ApplicationFeatures/LanguageFeature.h"
#include "ApplicationFeatures/LazyApplicationFeatureReference.h"
#include "ApplicationFeatures/OptionsCheckFeature.h"
#include "ApplicationFeatures/ShellColorsFeature.h"
#include "ApplicationFeatures/ShutdownFeature.h"
#include "ApplicationFeatures/TempFeature.h"
#ifdef USE_V8
#include "V8/V8PlatformFeature.h"
#include "V8/V8SecurityFeature.h"
#endif
#include "ApplicationFeatures/VersionFeature.h"
#include "Aql/AqlFunctionFeature.h"
#include "Aql/OptimizerRulesFeature.h"
#include "Basics/ArangoGlobalContext.h"
#include "Basics/FileUtils.h"
#include "Basics/directories.h"
#include "Basics/operating-system.h"
#include "Basics/tri-strings.h"
#include "Cache/CacheManagerFeature.h"
#include "Cache/CacheOptionsFeature.h"
#include "Cluster/ClusterFeature.h"
#include "Cluster/ClusterUpgradeFeature.h"
#include "Cluster/LeaseManager/LeaseManagerFeature.h"
#include "Cluster/MaintenanceFeature.h"
#include "Cluster/ReplicationTimeoutFeature.h"
#include "Cluster/ServerState.h"
#include "ClusterEngine/ClusterEngine.h"
#include "CrashHandler/CrashHandler.h"
#include "FeaturePhases/AgencyFeaturePhase.h"
#include "FeaturePhases/AqlFeaturePhase.h"
#include "FeaturePhases/BasicFeaturePhaseServer.h"
#include "FeaturePhases/ClusterFeaturePhase.h"
#include "FeaturePhases/DatabaseFeaturePhase.h"
#include "FeaturePhases/FinalFeaturePhase.h"
#ifdef USE_V8
#include "FeaturePhases/FoxxFeaturePhase.h"
#endif
#include "FeaturePhases/ServerFeaturePhase.h"
#ifdef USE_V8
#include "FeaturePhases/V8FeaturePhase.h"
#endif
#include "GeneralServer/AuthenticationFeature.h"
#include "GeneralServer/GeneralServerFeature.h"
#include "GeneralServer/ServerSecurityFeature.h"
#include "GeneralServer/SslServerFeature.h"
#include "IResearch/IResearchAnalyzerFeature.h"
#include "IResearch/IResearchFeature.h"
#include "Logger/LoggerFeature.h"
#include "Metrics/ClusterMetricsFeature.h"
#include "Metrics/MetricsFeature.h"
#include "Network/NetworkFeature.h"
#include "ProgramOptions/ProgramOptions.h"
#include "Random/RandomFeature.h"
#include "Replication/ReplicationFeature.h"
#include "Replication/ReplicationMetricsFeature.h"
#include "Replication2/ReplicatedLog/ReplicatedLogFeature.h"
#include "Replication2/ReplicatedState/ReplicatedStateFeature.h"
#include "Replication2/StateMachines/BlackHole/BlackHoleStateMachineFeature.h"
#include "Replication2/StateMachines/Document/DocumentStateMachineFeature.h"
#include "RestServer/AqlFeature.h"
#include "RestServer/BootstrapFeature.h"
#include "RestServer/CheckVersionFeature.h"
#ifdef USE_V8
#include "RestServer/ConsoleFeature.h"
#endif
#include "RestServer/CpuUsageFeature.h"
#include "RestServer/DaemonFeature.h"
#include "RestServer/DatabaseFeature.h"
#include "RestServer/DatabasePathFeature.h"
#include "RestServer/DumpLimitsFeature.h"
#include "RestServer/EndpointFeature.h"
#include "RestServer/EnvironmentFeature.h"
#include "RestServer/FileDescriptorsFeature.h"
#include "RestServer/FlushFeature.h"
#include "RestServer/FortuneFeature.h"
#ifdef USE_V8
#include "RestServer/FrontendFeature.h"
#endif
#include "RestServer/InitDatabaseFeature.h"
#include "RestServer/LanguageCheckFeature.h"
#include "RestServer/LockfileFeature.h"
#include "RestServer/LogBufferFeature.h"
#include "RestServer/MaxMapCountFeature.h"
#include "RestServer/NonceFeature.h"
#include "RestServer/PrivilegeFeature.h"
#include "RestServer/QueryRegistryFeature.h"
#include "RestServer/RestartAction.h"
#ifdef USE_V8
#include "RestServer/ScriptFeature.h"
#endif
#include "RestServer/ServerFeature.h"
#include "RestServer/ServerIdFeature.h"
#include "RestServer/SharedPRNGFeature.h"
#include "RestServer/SoftShutdownFeature.h"
#include "RestServer/SupervisorFeature.h"
#include "RestServer/SystemDatabaseFeature.h"
#include "RestServer/TemporaryStorageFeature.h"
#include "RestServer/TimeZoneFeature.h"
#include "RestServer/TtlFeature.h"
#include "RestServer/UpgradeFeature.h"
#include "RestServer/ViewTypesFeature.h"
#include "RocksDBEngine/RocksDBEngine.h"
#include "RocksDBEngine/RocksDBIndexCacheRefillFeature.h"
#include "RocksDBEngine/RocksDBOptionFeature.h"
#include "RocksDBEngine/RocksDBRecoveryManager.h"
#include "Scheduler/SchedulerFeature.h"
#include "Sharding/ShardingFeature.h"
#include "Ssl/SslFeature.h"
#include "Statistics/StatisticsFeature.h"
#include "Statistics/StatisticsWorker.h"
#include "StorageEngine/EngineSelectorFeature.h"
#include "StorageEngine/StorageEngineFeature.h"
#include "Transaction/ManagerFeature.h"
#ifdef USE_V8
#include "V8Server/FoxxFeature.h"
#include "V8Server/V8DealerFeature.h"
#endif

#ifdef USE_ENTERPRISE
#include "Enterprise/Audit/AuditFeature.h"
#include "Enterprise/Encryption/EncryptionFeature.h"
#include "Enterprise/License/LicenseFeature.h"
#include "Enterprise/RClone/RCloneFeature.h"
#include "Enterprise/Ssl/SslServerFeatureEE.h"
#include "Enterprise/StorageEngine/HotBackupFeature.h"
#endif
