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
/// @author Andrey Abramov
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include "ApplicationFeatures/ApplicationFeature.h"
#include "Basics/TypeList.h"
#include "Basics/operating-system.h"

namespace arangodb {
namespace application_features {

class AgencyFeaturePhase;
class CommunicationFeaturePhase;
class AqlFeaturePhase;
class BasicFeaturePhaseServer;
class ClusterFeaturePhase;
class DatabaseFeaturePhase;
class FinalFeaturePhase;
class FoxxFeaturePhase;
class GreetingsFeaturePhase;
class ServerFeaturePhase;
class V8FeaturePhase;

template<typename Features>
class ApplicationServerT;

}  // namespace application_features
namespace metrics {

class MetricsFeature;
class ClusterMetricsFeature;

}  // namespace metrics

namespace cluster {
class LeaseManagerFeature;
}

class AqlFeature;
class AgencyFeature;
class ActionFeature;
class AuthenticationFeature;
class BootstrapFeature;
class BumpFileDescriptorsFeature;
class CacheManagerFeature;
class CacheOptionsFeature;
class CheckVersionFeature;
class ClusterFeature;
class ClusterUpgradeFeature;
class ConfigFeature;
class ConsoleFeature;
class CpuUsageFeature;
class DatabaseFeature;
class DatabasePathFeature;
class DumpLimitsFeature;
class HttpEndpointProvider;
class EngineSelectorFeature;
class EnvironmentFeature;
class FileDescriptorsFeature;
class FileSystemFeature;
class FlushFeature;
class FortuneFeature;
class FoxxFeature;
class FrontendFeature;
class GeneralServerFeature;
class GreetingsFeature;
class InitDatabaseFeature;
class LanguageCheckFeature;
class LanguageFeature;
class TimeZoneFeature;
class LockfileFeature;
class LogBufferFeature;
class LoggerFeature;
class MaintenanceFeature;
class MaxMapCountFeature;
class NetworkFeature;
class NonceFeature;
class OptionsCheckFeature;
class PrivilegeFeature;
class QueryRegistryFeature;
class RandomFeature;
class ReplicationFeature;
class ReplicatedLogFeature;
class ReplicationMetricsFeature;
class ReplicationTimeoutFeature;
class SchedulerFeature;
class ScriptFeature;
class ServerFeature;
class ServerIdFeature;
class ServerSecurityFeature;
class ShardingFeature;
class SharedPRNGFeature;
class ShellColorsFeature;
class ShutdownFeature;
class SoftShutdownFeature;
class SslFeature;
class StatisticsFeature;
class StorageEngineFeature;
class SystemDatabaseFeature;
class TempFeature;
class TemporaryStorageFeature;
class TtlFeature;
class UpgradeFeature;
class V8DealerFeature;
class V8PlatformFeature;
class V8SecurityFeature;
class VersionFeature;
class ViewTypesFeature;
class ClusterEngine;
class DaemonFeature;
class SupervisorFeature;
class WindowsServiceFeature;
class AuditFeature;
class LicenseFeature;
class RCloneFeature;
class HotBackupFeature;
class EncryptionFeature;
class SslServerFeature;
class RocksDBEngine;
class RocksDBIndexCacheRefillFeature;
class RocksDBOptionFeature;
class RocksDBRecoveryManager;

namespace transaction {

class ManagerFeature;

}  // namespace transaction
namespace aql {

class AqlFunctionFeature;
class OptimizerRulesFeature;

}  // namespace aql
namespace iresearch {

class IResearchAnalyzerFeature;
class IResearchFeature;

}  // namespace iresearch
namespace replication2::replicated_state {

struct ReplicatedStateAppFeature;

namespace black_hole {

struct BlackHoleStateMachineFeature;

}  // namespace black_hole

namespace document {
struct DocumentStateMachineFeature;
}
}  // namespace replication2::replicated_state

using namespace application_features;

// clang-format off
using ArangodFeaturesList = TypeList<
    // Adding the Phases
    AgencyFeaturePhase,
    CommunicationFeaturePhase,
    AqlFeaturePhase,
    BasicFeaturePhaseServer,
    ClusterFeaturePhase,
    DatabaseFeaturePhase,
    FinalFeaturePhase,
#ifdef USE_V8
    FoxxFeaturePhase,
#endif
    GreetingsFeaturePhase,
    ServerFeaturePhase,
#ifdef USE_V8
    V8FeaturePhase,
#endif
    // Adding the features
    metrics::MetricsFeature, // metrics::MetricsFeature must go first
    metrics::ClusterMetricsFeature,
    VersionFeature,
    ActionFeature,
    AgencyFeature,
    AqlFeature,
    AuthenticationFeature,
    BootstrapFeature,
#ifdef TRI_HAVE_GETRLIMIT
    BumpFileDescriptorsFeature,
#endif
    CacheOptionsFeature,
    CacheManagerFeature,
    CheckVersionFeature,
    ClusterFeature,
    DatabaseFeature,
    ClusterUpgradeFeature,
    ConfigFeature,
#ifdef USE_V8
    ConsoleFeature,
#endif
    CpuUsageFeature,
    DatabasePathFeature,
    DumpLimitsFeature,
    HttpEndpointProvider,
    EngineSelectorFeature,
    EnvironmentFeature,
    FileSystemFeature,
    FlushFeature,
    FortuneFeature,
#ifdef USE_V8
    FoxxFeature,
    FrontendFeature,
#endif
    GeneralServerFeature,
    GreetingsFeature,
    InitDatabaseFeature,
    LanguageCheckFeature,
    LanguageFeature,
    TimeZoneFeature,
    LockfileFeature,
    LogBufferFeature,
    LoggerFeature,
    MaintenanceFeature,
    MaxMapCountFeature,
    NetworkFeature,
    NonceFeature,
    OptionsCheckFeature,
    PrivilegeFeature,
    QueryRegistryFeature,
    RandomFeature,
    ReplicationFeature,
    ReplicatedLogFeature,
    ReplicationMetricsFeature,
    ReplicationTimeoutFeature,
    SchedulerFeature,
#ifdef USE_V8
    ScriptFeature,
#endif
    ServerFeature,
    ServerIdFeature,
    ServerSecurityFeature,
    ShardingFeature,
    SharedPRNGFeature,
    ShellColorsFeature,
    ShutdownFeature,
    SoftShutdownFeature,
    SslFeature,
    StatisticsFeature,
    StorageEngineFeature,
    SystemDatabaseFeature,
    TempFeature,
    TemporaryStorageFeature,
    TtlFeature,
    UpgradeFeature,
#ifdef USE_V8
    V8DealerFeature,
    V8PlatformFeature,
    V8SecurityFeature,
#endif
    transaction::ManagerFeature,
    ViewTypesFeature,
    // Required to be registered after: SchedulerFeature, NetworkFeature and ClusterFeature
    cluster::LeaseManagerFeature,
    aql::AqlFunctionFeature,
    aql::OptimizerRulesFeature,
    RocksDBIndexCacheRefillFeature,
    RocksDBOptionFeature,
    RocksDBRecoveryManager,
#ifdef TRI_HAVE_GETRLIMIT
    FileDescriptorsFeature,
#endif
#ifdef ARANGODB_HAVE_FORK
    DaemonFeature,
    SupervisorFeature,
#endif
#ifdef USE_ENTERPRISE
    AuditFeature,
    LicenseFeature,
    RCloneFeature,
    HotBackupFeature,
    EncryptionFeature,
#endif
    SslServerFeature,
    iresearch::IResearchAnalyzerFeature,
    iresearch::IResearchFeature,
    ClusterEngine,
    RocksDBEngine,
    replication2::replicated_state::ReplicatedStateAppFeature,
    replication2::replicated_state::black_hole::BlackHoleStateMachineFeature,
    replication2::replicated_state::document::DocumentStateMachineFeature
>;  // clang-format on
struct ArangodFeatures : ArangodFeaturesList {};
using ArangodServer = application_features::ApplicationServerT<ArangodFeatures>;
using ArangodFeature = application_features::ApplicationFeatureT<ArangodServer>;

}  // namespace arangodb
