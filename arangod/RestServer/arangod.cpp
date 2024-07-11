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
/// @author Dr. Frank Celler
////////////////////////////////////////////////////////////////////////////////

#include "RestServer/arangod.h"

#include <string_view>
#include <type_traits>
#include "RestServer/DatabaseFeature.h"

// The list of includes for the features is defined in the following file -
// please add new includes there!
#include "RestServer/arangod_includes.h"

using namespace arangodb;
using namespace arangodb::application_features;

constexpr auto kNonServerFeatures =
    std::array{ArangodServer::id<ActionFeature>(),
               ArangodServer::id<AgencyFeature>(),
               ArangodServer::id<ClusterFeature>(),
#ifdef ARANGODB_HAVE_FORK
               ArangodServer::id<SupervisorFeature>(),
               ArangodServer::id<DaemonFeature>(),
#endif
#ifdef USE_V8
               ArangodServer::id<FoxxFeature>(),
#endif
               ArangodServer::id<GeneralServerFeature>(),
               ArangodServer::id<GreetingsFeature>(),
               ArangodServer::id<HttpEndpointProvider>(),
               ArangodServer::id<LogBufferFeature>(),
               ArangodServer::id<ServerFeature>(),
               ArangodServer::id<SslServerFeature>(),
               ArangodServer::id<StatisticsFeature>()};

static int runServer(int argc, char** argv, ArangoGlobalContext& context) {
  try {
    CrashHandler::installCrashHandler();
    std::string name = context.binaryName();

    auto options = std::make_shared<arangodb::options::ProgramOptions>(
        argv[0], "Usage: " + name + " [<options>]",
        "For more information use:", SBIN_DIRECTORY);

    int ret{EXIT_FAILURE};
    ArangodServer server{options, SBIN_DIRECTORY};
    ServerState state{server};

    server.addReporter(
        {[&](ArangodServer::State state) {
           CrashHandler::setState(ArangodServer::stringifyState(state));

           if (state == ArangodServer::State::IN_START) {
             // drop priveleges before starting features
             server.getFeature<PrivilegeFeature>().dropPrivilegesPermanently();
           }
         },
         {}});

    server.addFeatures(Visitor{
        []<typename T>(auto& server, TypeTag<T>) {
          return std::make_unique<T>(server);
        },
#ifdef TRI_HAVE_GETRLIMIT
        [](auto& server, TypeTag<BumpFileDescriptorsFeature>) {
          return std::make_unique<BumpFileDescriptorsFeature>(
              server, "--server.descriptors-minimum");
        },
#endif
        [](auto& server, TypeTag<GreetingsFeaturePhase>) {
          return std::make_unique<GreetingsFeaturePhase>(server,
                                                         std::false_type{});
        },
        [&ret](auto& server, TypeTag<CheckVersionFeature>) {
          return std::make_unique<CheckVersionFeature>(server, &ret,
                                                       kNonServerFeatures);
        },
        [](auto& server, TypeTag<ClusterUpgradeFeature>) {
          return std::make_unique<ClusterUpgradeFeature>(
              server, server.template getFeature<arangodb::DatabaseFeature>());
        },
        [&name](auto& server, TypeTag<ConfigFeature>) {
          return std::make_unique<ConfigFeature>(server, name);
        },
        [](auto& server, TypeTag<GeneralServerFeature>) {
          return std::make_unique<GeneralServerFeature>(
              server,
              server.template getFeature<arangodb::metrics::MetricsFeature>());
        },
        [](auto& server, TypeTag<InitDatabaseFeature>) {
          return std::make_unique<InitDatabaseFeature>(server,
                                                       kNonServerFeatures);
        },
        [](auto& server, TypeTag<LoggerFeature>) {
          return std::make_unique<LoggerFeature>(server, true);
        },
        [](auto& server, TypeTag<metrics::MetricsFeature>) {
          return std::make_unique<metrics::MetricsFeature>(
              server,
              LazyApplicationFeatureReference<QueryRegistryFeature>(server),
              LazyApplicationFeatureReference<StatisticsFeature>(server),
              LazyApplicationFeatureReference<EngineSelectorFeature>(server),
              LazyApplicationFeatureReference<metrics::ClusterMetricsFeature>(
                  server),
              LazyApplicationFeatureReference<ClusterFeature>(server));
        },
        [](auto& server, TypeTag<NetworkFeature>) {
          auto& metrics =
              server.template getFeature<arangodb::metrics::MetricsFeature>();
          return std::make_unique<NetworkFeature>(
              server, metrics, network::ConnectionPool::Config{});
        },
        [](auto& server, TypeTag<cluster::LeaseManagerFeature>) {
          auto& schedulerFeature =
              server.template getFeature<SchedulerFeature>();
          auto& clusterFeature = server.template getFeature<ClusterFeature>();
          auto& networkFeature = server.template getFeature<NetworkFeature>();
          return std::make_unique<cluster::LeaseManagerFeature>(
              server, clusterFeature, networkFeature, schedulerFeature);
        },
        [](auto& server, TypeTag<QueryRegistryFeature>) {
          return std::make_unique<QueryRegistryFeature>(
              server,
              server.template getFeature<arangodb::metrics::MetricsFeature>());
        },
        [](auto& server, TypeTag<ReplicationMetricsFeature>) {
          return std::make_unique<ReplicationMetricsFeature>(
              server,
              server.template getFeature<arangodb::metrics::MetricsFeature>());
        },
        [](auto& server, TypeTag<RocksDBEngine>) {
          return std::make_unique<RocksDBEngine>(
              server,
              server.template getFeature<arangodb::RocksDBOptionFeature>(),
              server.template getFeature<arangodb::metrics::MetricsFeature>());
        },
        [](auto& server, TypeTag<SchedulerFeature>) {
          return std::make_unique<SchedulerFeature>(
              server,
              server.template getFeature<arangodb::metrics::MetricsFeature>());
        },
#ifdef USE_V8
        [&ret](auto& server, TypeTag<ScriptFeature>) {
          return std::make_unique<ScriptFeature>(server, &ret);
        },
#endif
        [&ret](auto& server, TypeTag<ServerFeature>) {
          return std::make_unique<ServerFeature>(server, &ret);
        },
        [](auto& server, TypeTag<CacheManagerFeature>) {
          return std::make_unique<CacheManagerFeature>(
              server, server.template getFeature<CacheOptionsFeature>());
        },
        [](auto& server, TypeTag<ShutdownFeature>) {
          return std::make_unique<ShutdownFeature>(
              server,
#ifdef USE_V8
              std::array { ArangodServer::id<ScriptFeature>() }
#else
              std::array { ArangodServer::id<AgencyFeaturePhase>() }
#endif
          );
        },
        [&name](auto& server, TypeTag<TempFeature>) {
          return std::make_unique<TempFeature>(server, name);
        },
        [](auto& server, TypeTag<SslServerFeature>) {
#ifdef USE_ENTERPRISE
          return std::make_unique<SslServerFeatureEE>(server);
#else
          return std::make_unique<SslServerFeature>(server);
#endif
        },
        [&ret](auto& server, TypeTag<UpgradeFeature>) {
          return std::make_unique<UpgradeFeature>(server, &ret,
                                                  kNonServerFeatures);
        },
        [&](auto& server, TypeTag<V8DealerFeature>) {
          return std::make_unique<V8DealerFeature>(
              server,
              server.template getFeature<arangodb::metrics::MetricsFeature>());
        },
        [](auto& server, TypeTag<HttpEndpointProvider>) {
          return std::make_unique<EndpointFeature>(server);
        }});

    try {
      server.run(argc, argv);
      if (server.helpShown()) {
        // --help was displayed
        ret = EXIT_SUCCESS;
      }
    } catch (std::exception const& ex) {
      LOG_TOPIC("5d508", ERR, arangodb::Logger::FIXME)
          << "arangod terminated because of an exception: " << ex.what();
      ret = EXIT_FAILURE;
    } catch (...) {
      LOG_TOPIC("3c63a", ERR, arangodb::Logger::FIXME)
          << "arangod terminated because of an exception of "
             "unknown type";
      ret = EXIT_FAILURE;
    }
    Logger::flush();
    return context.exit(ret);
  } catch (std::exception const& ex) {
    LOG_TOPIC("8afa8", ERR, arangodb::Logger::FIXME)
        << "arangod terminated because of an exception: " << ex.what();
  } catch (...) {
    LOG_TOPIC("c444c", ERR, arangodb::Logger::FIXME)
        << "arangod terminated because of an exception of "
           "unknown type";
  }
  exit(EXIT_FAILURE);
}

// The following is a hack which is currently (September 2019) needed to
// let our static executables compiled with libmusl and gcc 8.3.0 properly
// detect that we are a multi-threaded application.
// See https://gcc.gnu.org/bugzilla/show_bug.cgi?id=91737 for developments
// in gcc/libgcc to address this issue.

static void* g(void* p) { return p; }

static void gg() {}

static void f() {
  pthread_t t;
  pthread_create(&t, nullptr, g, nullptr);
  pthread_cancel(t);
  pthread_join(t, nullptr);
  static pthread_once_t once_control = PTHREAD_ONCE_INIT;
  pthread_once(&once_control, gg);
}

int main(int argc, char* argv[]) {
  // Do not delete this! See above for an explanation.
  if (argc >= 1 && strcmp(argv[0], "not a/valid name") == 0) {
    f();
  }

  std::string workdir(arangodb::basics::FileUtils::currentDirectory().result());

  TRI_GET_ARGV(argc, argv);
  ArangoGlobalContext context(argc, argv, SBIN_DIRECTORY);

  arangodb::restartAction = nullptr;

  int res = runServer(argc, argv, context);
  if (res != 0) {
    return res;
  }
  if (arangodb::restartAction == nullptr) {
    return 0;
  }
  try {
    res = (*arangodb::restartAction)();
  } catch (...) {
    res = -1;
  }
  delete arangodb::restartAction;
  if (res != 0) {
    std::cerr << "FATAL: RestartAction returned non-zero exit status: " << res
              << ", giving up." << std::endl;
    return res;
  }
  // It is not clear if we want to do the following under Linux and OSX,
  // it is a clean way to restart from scratch with the same process ID,
  // so the process does not have to be terminated. On Windows, we have
  // to do this because the solution below is not possible. In these
  // cases, we need outside help to get the process restarted.
  res = chdir(workdir.c_str());
  if (res != 0) {
    std::cerr << "WARNING: could not change into directory '" << workdir << "'"
              << std::endl;
  }
  if (execvp(argv[0], argv) == -1) {
    std::cerr << "WARNING: could not execvp ourselves, restore will not work!"
              << std::endl;
  }
}
