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
/// @author Jan Steemann
////////////////////////////////////////////////////////////////////////////////

#include "TtlFeature.h"

#include "ApplicationFeatures/ApplicationServer.h"
#include "Aql/QueryMethods.h"
#include "Aql/QueryRegistry.h"
#include "Aql/QueryResult.h"
#include "Aql/QueryString.h"
#include "Basics/ConditionVariable.h"
#include "Basics/Exceptions.h"
#include "Basics/StaticStrings.h"
#include "Basics/StringUtils.h"
#include "Basics/Thread.h"
#include "Basics/application-exit.h"
#include "Basics/debugging.h"
#include "Basics/system-functions.h"
#include "Cluster/ClusterFeature.h"
#include "Cluster/ClusterInfo.h"
#include "Cluster/FollowerInfo.h"
#include "Cluster/ServerState.h"
#include "Indexes/Index.h"
#include "Logger/LogMacros.h"
#include "Logger/Logger.h"
#include "Logger/LoggerStream.h"
#include "Network/Methods.h"
#include "Network/NetworkFeature.h"
#include "Network/Utils.h"
#include "Network/types.h"
#include "ProgramOptions/Parameters.h"
#include "ProgramOptions/ProgramOptions.h"
#include "RestServer/DatabaseFeature.h"
#include "StorageEngine/PhysicalCollection.h"
#include "Transaction/Methods.h"
#include "Transaction/OperationOrigin.h"
#include "Transaction/StandaloneContext.h"
#include "Utils/CollectionNameResolver.h"
#include "Utils/OperationOptions.h"
#include "Utils/OperationResult.h"
#include "Utils/SingleCollectionTransaction.h"
#include "VocBase/LogicalCollection.h"
#include "VocBase/vocbase.h"

#include <absl/strings/str_cat.h>
#include <velocypack/Builder.h>
#include <velocypack/Slice.h>

#include <algorithm>
#include <chrono>
#include <random>
#include <thread>

using namespace arangodb;
using namespace arangodb::options;

namespace {
// the AQL query to lookup documents
std::string const lookupQuery(
    "/*ttl cleanup*/ FOR doc IN @@collection OPTIONS { forceIndexHint: true, "
    "indexHint: @indexHint } FILTER doc.@indexAttribute >= 0 && "
    "doc.@indexAttribute <= @stamp SORT doc.@indexAttribute LIMIT @limit "
    "RETURN {_key: doc._key, _rev: doc._rev}");
}  // namespace

namespace arangodb {

TtlStatistics& TtlStatistics::operator+=(VPackSlice const& other) {
  if (!other.isObject()) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL,
                                   "expecting object for statistics");
  }

  if (other.hasKey("runs")) {
    runs += other.get("runs").getNumericValue<uint64_t>();
  }
  if (other.hasKey("documentsRemoved")) {
    documentsRemoved +=
        other.get("documentsRemoved").getNumericValue<uint64_t>();
  }
  if (other.hasKey("limitReached")) {
    limitReached += other.get("limitReached").getNumericValue<uint64_t>();
  }
  return *this;
}

void TtlStatistics::toVelocyPack(VPackBuilder& builder) const {
  builder.openObject();
  builder.add("runs", VPackValue(runs));
  builder.add("documentsRemoved", VPackValue(documentsRemoved));
  builder.add("limitReached", VPackValue(limitReached));
  builder.close();
}

void TtlProperties::toVelocyPack(VPackBuilder& builder, bool isActive) const {
  builder.openObject();
  builder.add("frequency", VPackValue(frequency));
  builder.add("maxTotalRemoves", VPackValue(maxTotalRemoves));
  builder.add("maxCollectionRemoves", VPackValue(maxCollectionRemoves));
  // this attribute is hard-coded to false since v3.8, and will be removed later
  builder.add("onlyLoadedCollections", VPackValue(false));
  builder.add("active", VPackValue(isActive));
  builder.close();
}

Result TtlProperties::fromVelocyPack(VPackSlice const& slice) {
  if (!slice.isObject()) {
    return Result(TRI_ERROR_BAD_PARAMETER, "expecting object for properties");
  }

  try {
    uint64_t frequency = this->frequency;
    uint64_t maxTotalRemoves = this->maxTotalRemoves;
    uint64_t maxCollectionRemoves = this->maxCollectionRemoves;

    if (slice.hasKey("frequency")) {
      if (!slice.get("frequency").isNumber()) {
        return Result(TRI_ERROR_BAD_PARAMETER,
                      "expecting numeric value for frequency");
      }
      frequency = slice.get("frequency").getNumericValue<uint64_t>();
      TRI_IF_FAILURE("allow-low-ttl-frequency") {
        // for faster js tests we want to allow lower frequency values
      }
      else {
        if (frequency < TtlProperties::minFrequency) {
          return Result(TRI_ERROR_BAD_PARAMETER, "too low value for frequency");
        }
      }
    }
    if (slice.hasKey("maxTotalRemoves")) {
      if (!slice.get("maxTotalRemoves").isNumber()) {
        return Result(TRI_ERROR_BAD_PARAMETER,
                      "expecting numeric value for maxTotalRemoves");
      }
      maxTotalRemoves =
          slice.get("maxTotalRemoves").getNumericValue<uint64_t>();
    }
    if (slice.hasKey("maxCollectionRemoves")) {
      if (!slice.get("maxCollectionRemoves").isNumber()) {
        return Result(TRI_ERROR_BAD_PARAMETER,
                      "expecting numeric value for maxCollectionRemoves");
      }
      maxCollectionRemoves =
          slice.get("maxCollectionRemoves").getNumericValue<uint64_t>();
    }

    this->frequency = frequency;
    this->maxTotalRemoves = maxTotalRemoves;
    this->maxCollectionRemoves = maxCollectionRemoves;

    return Result();
  } catch (arangodb::basics::Exception const& ex) {
    return Result(ex.code(), ex.what());
  } catch (std::exception const& ex) {
    return Result(TRI_ERROR_INTERNAL, ex.what());
  }
}

class TtlThread final : public ServerThread<ArangodServer> {
 public:
  explicit TtlThread(ArangodServer& server, TtlFeature& ttlFeature)
      : ServerThread<ArangodServer>(server, "TTL"),
        _ttlFeature(ttlFeature),
        _working(false) {}

  ~TtlThread() final { shutdown(); }

  void beginShutdown() final {
    Thread::beginShutdown();

    // wake up the thread that may be waiting in run()
    wakeup();
  }

  void wakeup() {
    // wake up the thread that may be waiting in run()
    std::lock_guard guard{_condition.mutex};
    _condition.cv.notify_one();
  }

  bool isCurrentlyWorking() const { return _working.load(); }

  /// @brief frequency is specified in milliseconds
  void setNextStart(uint64_t frequency) {
    std::lock_guard guard{_condition.mutex};
    _nextStart =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(frequency);
  }

 private:
  void run() final {
    TtlProperties properties = _ttlFeature.properties();
    setNextStart(properties.frequency);

    LOG_TOPIC("c2be7", TRACE, Logger::TTL)
        << "starting TTL background thread with interval "
        << properties.frequency
        << " milliseconds, max removals per run: " << properties.maxTotalRemoves
        << ", max removals per collection per run "
        << properties.maxCollectionRemoves;

    while (true) {
      while (true) {
        if (isStopping()) {
          // server shutdown
          return;
        }
        std::unique_lock guard{_condition.mutex};
        auto now = std::chrono::steady_clock::now();
        if (now >= _nextStart) {
          break;
        }

        // wait for our start...
        _condition.cv.wait_for(
            guard, std::chrono::duration_cast<std::chrono::microseconds>(
                       _nextStart - now));
      }

      // properties may have changed... update them
      properties = _ttlFeature.properties();
      setNextStart(properties.frequency);

      try {
        TtlStatistics stats;
        // note: work() will do nothing if isActive() is false
        work(stats, properties);

        // merge stats
        _ttlFeature.updateStats(stats);
      } catch (std::exception const& ex) {
        LOG_TOPIC("6d28a", WARN, Logger::TTL)
            << "caught exception in TTL background thread: " << ex.what();
      } catch (...) {
        LOG_TOPIC("44aa8", WARN, Logger::TTL)
            << "caught unknown exception in TTL background thread";
      }
    }
  }

  /// @brief whether or not the background thread shall continue working
  bool isActive() const {
    return _ttlFeature.isActive() && !isStopping() && !ServerState::readOnly();
  }

  void work(TtlStatistics& stats, TtlProperties const& properties) {
    if (!isActive()) {
      return;
    }

    // mark ourselves as busy
    _working = true;
    auto guard = scopeGuard([this]() noexcept { _working = false; });

    LOG_TOPIC("139af", TRACE, Logger::TTL) << "ttl thread work()";

    stats.runs++;

    // these are only needed if we are on a DB server
    size_t coordinatorIndex = 0;
    std::vector<ServerID> coordinators;

    if (ServerState::instance()->isDBServer()) {
      // fetch list of current coordinators
      auto& ci = server().getFeature<ClusterFeature>().clusterInfo();
      coordinators = ci.getCurrentCoordinators();
    }

    std::random_device rd;
    std::mt19937 randGen(rd());

    constexpr uint64_t maxResultsPerQuery = 4000;

    double const stamp = TRI_microtime();
    uint64_t limitLeft = properties.maxTotalRemoves;

    // iterate over all databases
    auto& db = server().getFeature<DatabaseFeature>();

    auto databases = db.getDatabaseNames();
    // randomize the list of databases so that we do not favor particular
    // databases in the removals. the total amount of documents to remove
    // is limited, so a deterministic input could favor the first few
    // databases in the list.
    std::shuffle(databases.begin(), databases.end(), randGen);
    for (auto const& name : databases) {
      if (!isActive()) {
        // feature deactivated
        return;
      }

      auto vocbase = db.useDatabase(name);

      if (vocbase == nullptr) {
        continue;
      }

      LOG_TOPIC("ec905", TRACE, Logger::TTL)
          << "TTL thread going to process database '" << vocbase->name() << "'";

      std::vector<std::shared_ptr<arangodb::LogicalCollection>> collections =
          vocbase->collections(false);
      // randomize the list of collections so that we do not favor particular
      // collections in the removals. the total amount of documents to remove
      // is limited, so a deterministic input could favor the first few
      // collections in the list.
      std::shuffle(collections.begin(), collections.end(), randGen);

      for (auto const& collection : collections) {
        if (!isActive()) {
          // feature deactivated
          return;
        }

        if (ServerState::instance()->isDBServer() &&
            !collection->isLeadingShard()) {
          // we are a follower for this shard. do not remove any data here, but
          // let the leader carry out the removal and replicate it
          continue;
        }

        std::vector<std::shared_ptr<Index>> indexes =
            collection->getPhysical()->getReadyIndexes();

        for (auto const& index : indexes) {
          // we are only interested in collections with TTL indexes
          if (index->type() != Index::TRI_IDX_TYPE_TTL_INDEX) {
            continue;
          }

          TRI_ASSERT(!index->inProgress());

          // serialize the index description so we can read the "expireAfter"
          // attribute
          _builder.clear();
          index->toVelocyPack(_builder, Index::makeFlags());
          VPackSlice ea = _builder.slice().get(StaticStrings::IndexExpireAfter);
          if (!ea.isNumber()) {
            // index description somehow invalid
            continue;
          }

          double expireAfter = ea.getNumericValue<double>();
          LOG_TOPIC("5cca5", DEBUG, Logger::TTL)
              << "TTL thread going to work for collection '"
              << collection->name()
              << "', expireAfter: " << Logger::FIXED(expireAfter, 0)
              << ", stamp: " << (stamp - expireAfter) << ", limit: "
              << std::min(properties.maxCollectionRemoves, limitLeft);

          auto bindVars = [&]() {
            auto bindVars = std::make_shared<VPackBuilder>();
            bindVars->openObject();
            bindVars->add("indexHint", VPackValue(index->name()));
            bindVars->add("@collection", VPackValue(collection->name()));
            bindVars->add(VPackValue("indexAttribute"));
            bindVars->openArray();
            for (auto const& it : index->fields()[0]) {
              bindVars->add(VPackValue(it.name));
            }
            bindVars->close();
            bindVars->add("stamp", VPackValue(stamp - expireAfter));
            bindVars->add(
                "limit",
                VPackValue(std::min(
                    maxResultsPerQuery,
                    std::min(properties.maxCollectionRemoves, limitLeft))));
            bindVars->close();
            return bindVars;
          }();

          auto origin =
              transaction::OperationOriginInternal{"ttl index cleanup"};

          size_t leftForCurrentCollection =
              std::min(properties.maxCollectionRemoves, limitLeft);

          while (leftForCurrentCollection > 0) {
            aql::QueryOptions options;
            // don't let query runtime restrictions affect the lookup query
            options.maxRuntime = 0.0;
            // no need to make the lookup query appear in the audit log
            // every time we do a potential TTL index purge
            options.skipAudit = true;

            auto queryFuture = arangodb::aql::runStandaloneAqlQuery(
                *vocbase, origin, aql::QueryString(::lookupQuery),
                std::move(bindVars), std::move(options));
            auto queryResult = std::move(queryFuture.waitAndGet());

            if (queryResult.result.fail()) {
              // we can probably live with an error here...
              // the thread will try to remove the documents again on next
              // iteration
              if (!queryResult.result.is(
                      TRI_ERROR_QUERY_FORCED_INDEX_HINT_UNUSABLE) &&
                  !queryResult.result.is(TRI_ERROR_ARANGO_DATABASE_NOT_FOUND) &&
                  !queryResult.result.is(
                      TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND)) {
                LOG_TOPIC("08300", WARN, Logger::TTL)
                    << "unexpected error during TTL document removal for "
                       "collection '"
                    << collection->name()
                    << "': " << queryResult.result.errorMessage();
              }
              break;
            }

            if (queryResult.data == nullptr ||
                !queryResult.data->slice().isArray()) {
              break;
            }

            VPackSlice docsToRemove = queryResult.data->slice();
            size_t found = docsToRemove.length();
            if (found == 0) {
              break;
            }

            if (found > limitLeft) {
              limitLeft = 0;
            } else {
              limitLeft -= found;
            }

            if (found < maxResultsPerQuery ||
                found > leftForCurrentCollection) {
              // no more documents to remove
              leftForCurrentCollection = 0;
            } else {
              leftForCurrentCollection -= found;
            }

            if (ServerState::instance()->isDBServer() &&
                collection->isRemoteSmartEdgeCollection()) {
              // SmartGraph edge collection
              TRI_ASSERT(collection->type() == TRI_COL_TYPE_EDGE);

              // look up cluster-wide collection name from shard
              CollectionNameResolver resolver(collection->vocbase());
              std::string cname = resolver.getCollectionName(collection->id());

              auto& nf = server().getFeature<arangodb::NetworkFeature>();
              network::ConnectionPool* pool = nf.pool();
              if (pool == nullptr) {
                break;
              }

              if (coordinators.empty()) {
                // no coordinator to send the request to
                break;
              }

              network::RequestOptions reqOptions;
              reqOptions.param(StaticStrings::IgnoreRevsString, "false");
              reqOptions.param(StaticStrings::WaitForSyncString, "false");
              reqOptions.database = collection->vocbase().name();
              reqOptions.timeout = network::Timeout(30.0);

              // pick next coordinator in list (round robin)
              TRI_ASSERT(!coordinators.empty());
              auto const& coordinator =
                  coordinators[++coordinatorIndex % coordinators.size()];

              // send batch document deletion request to the picked coordinator
              std::string url = absl::StrCat(
                  "/_api/document/", basics::StringUtils::urlEncode(cname));
              auto f = network::sendRequestRetry(
                  pool, "server:" + coordinator, fuerte::RestVerb::Delete, url,
                  std::move(*queryResult.data->steal()), reqOptions);
              auto& val = f.waitAndGet();
              Result res = val.combinedResult();

              if (res.fail()) {
                // we can ignore these errors here, as they can be expected if
                // some other operations concurrently modify/remove documents in
                // the collection
                if (!res.is(TRI_ERROR_ARANGO_READ_ONLY) &&
                    !res.is(TRI_ERROR_ARANGO_CONFLICT) &&
                    !res.is(TRI_ERROR_ARANGO_DOCUMENT_NOT_FOUND) &&
                    !res.is(TRI_ERROR_LOCKED) &&
                    !res.is(TRI_ERROR_ARANGO_DATABASE_NOT_FOUND) &&
                    !res.is(TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND)) {
                  LOG_TOPIC("f211d", WARN, Logger::TTL)
                      << "unexpected error during TTL document removal for "
                         "SmartGraph collection '"
                      << cname << "': " << res.errorMessage();
                }
                break;
              }

              size_t count = 0;
              for (auto it : VPackArrayIterator(val.slice())) {
                VPackSlice error = it.get(StaticStrings::Error);
                if (!error.isTrue()) {
                  ++count;
                }
              }

              stats.documentsRemoved += count;
              LOG_TOPIC("2455f", INFO, Logger::TTL)
                  << "TTL thread removed " << count
                  << " documents for SmartGraph collection '" << cname << "'";

            } else {
              // single server or non-SmartGraph collection
              auto trx = std::make_unique<SingleCollectionTransaction>(
                  transaction::StandaloneContext::create(*vocbase, origin),
                  collection->name(), AccessMode::Type::WRITE);

              Result res = trx->begin();

              if (res.fail()) {
                // we can ignore these errors here, as they can be expected if
                // some other operations concurrently modify/remove documents in
                // the collection
                if (!res.is(TRI_ERROR_ARANGO_READ_ONLY) &&
                    !res.is(TRI_ERROR_ARANGO_CONFLICT) &&
                    !res.is(TRI_ERROR_ARANGO_DOCUMENT_NOT_FOUND) &&
                    !res.is(TRI_ERROR_LOCKED) &&
                    !res.is(TRI_ERROR_ARANGO_DATABASE_NOT_FOUND) &&
                    !res.is(TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND)) {
                  LOG_TOPIC("f211c", WARN, Logger::TTL)
                      << "unexpected error during TTL document removal for "
                         "collection '"
                      << collection->name() << "': " << res.errorMessage();
                }
                break;
              }

              OperationOptions opOptions;
              opOptions.ignoreRevs = false;
              opOptions.waitForSync = false;

              OperationResult opRes =
                  trx->remove(collection->name(), docsToRemove, opOptions);
              if (opRes.fail()) {
                LOG_TOPIC("86e90", WARN, Logger::TTL)
                    << "could not remove " << found
                    << " documents in TTL thread: "
                    << opRes.result.errorMessage();
                break;
              }

              bool doCommit = true;
              size_t count = 0;
              for (auto it : VPackArrayIterator(opRes.slice())) {
                VPackSlice error = it.get(StaticStrings::Error);
                if (!error.isTrue()) {
                  ++count;
                  continue;
                }
                error = it.get(StaticStrings::ErrorNum);
                if (error.isNumber()) {
                  auto code = ErrorCode{error.getNumericValue<int>()};
                  if (code != TRI_ERROR_ARANGO_READ_ONLY &&
                      code != TRI_ERROR_ARANGO_CONFLICT &&
                      code != TRI_ERROR_ARANGO_DOCUMENT_NOT_FOUND &&
                      code != TRI_ERROR_LOCKED) {
                    LOG_TOPIC("86e91", WARN, Logger::TTL)
                        << "could not remove " << found
                        << " documents in TTL thread: "
                        << it.get(StaticStrings::ErrorMessage).stringView();
                    doCommit = false;
                    break;
                  }
                }
              }

              if (!doCommit || count == 0) {
                break;
              }

              res = trx->finish(res);
              if (res.fail()) {
                LOG_TOPIC("86e92", WARN, Logger::TTL)
                    << "unable to commit removal operation for " << count
                    << " documents in TTL thread: " << res.errorMessage();
                break;
              }

              stats.documentsRemoved += count;
              LOG_TOPIC("2455e", INFO, Logger::TTL)
                  << "TTL thread removed " << count
                  << " documents for collection '" << collection->name() << "'";
            }
          }

          // there can only be one TTL index per collection, so we can abort the
          // loop here
          break;
        }

        if (limitLeft == 0) {
          // removed as much as we are allowed to. now stop and remove more in
          // next iteration
          ++stats.limitReached;
          return;
        }

        if (isStopping()) {
          // server has been stopped, so abort our loop(s)
          return;
        }
      }
    }
  }

  TtlFeature& _ttlFeature;

  arangodb::basics::ConditionVariable _condition;

  /// @brief next time the thread should run
  std::chrono::time_point<std::chrono::steady_clock> _nextStart;

  /// @brief a builder object we reuse to save a few memory allocations
  VPackBuilder _builder;

  /// @brief set to true while the TTL thread is actually performing deletions,
  /// false otherwise
  std::atomic<bool> _working;
};

}  // namespace arangodb

TtlFeature::TtlFeature(Server& server)
    : ArangodFeature{server, *this}, _active(true) {
  startsAfter<application_features::DatabaseFeaturePhase>();
  startsAfter<application_features::ServerFeaturePhase>();
}

TtlFeature::~TtlFeature() { shutdownThread(); }

void TtlFeature::collectOptions(std::shared_ptr<ProgramOptions> options) {
  options->addSection("ttl", "TTL index options");

  options
      ->addOption(
          "--ttl.frequency",
          "The frequency (in milliseconds) for the TTL background thread "
          "invocation (0 = turn the TTL background thread off entirely).",
          new UInt64Parameter(&_properties.frequency))
      .setLongDescription(R"(The lower this value, the more frequently the TTL
background thread kicks in and scans all available TTL indexes for expired
documents, and the earlier the expired documents are actually removed.)");

  options
      ->addOption("--ttl.max-total-removes",
                  "The maximum number of documents to remove per invocation of "
                  "the TTL thread.",
                  new UInt64Parameter(&_properties.maxTotalRemoves, /*base*/ 1,
                                      /*minValue*/ 1))
      .setLongDescription(R"(In order to avoid "random" load spikes by the
background thread suddenly kicking in and removing a lot of documents at once,
you can cap the number of to-be-removed documents per thread invocation.

The TTL background thread goes back to sleep once it has removed the configured
number of documents in one iteration. If more candidate documents are left for
removal, they are removed in subsequent runs of the background thread.)");

  options
      ->addOption(
          "--ttl.max-collection-removes",
          "The maximum number of documents to remove per collection in each "
          "invocation of the TTL thread.",
          new UInt64Parameter(&_properties.maxCollectionRemoves, /*base*/ 1,
                              /*minValue*/ 1))
      .setLongDescription(R"(You can configure this value separately from the
total removal amount so that the per-collection time window for locking and
potential write-write conflicts can be reduced.)");

  // the following option was obsoleted in 3.8
  options->addObsoleteOption(
      "--ttl.only-loaded-collection",
      "only consider already loaded collections for removal", false);
}

void TtlFeature::validateOptions(std::shared_ptr<ProgramOptions> options) {
  if (_properties.maxCollectionRemoves == 0) {
    LOG_TOPIC("2ab82", FATAL, arangodb::Logger::STARTUP)
        << "invalid value for '--ttl.max-collection-removes'.";
    FATAL_ERROR_EXIT();
  }

  std::lock_guard locker{_propertiesMutex};

  if (_properties.frequency > 0 &&
      _properties.frequency < TtlProperties::minFrequency) {
    LOG_TOPIC("ea696", FATAL, arangodb::Logger::STARTUP)
        << "too low value for '--ttl.frequency'.";
    FATAL_ERROR_EXIT();
  }
}

void TtlFeature::start() {
  // the thread will not run on a coordinator or an agency node,
  // just locally on DB servers or single servers
  if (ServerState::instance()->isCoordinator() ||
      ServerState::instance()->isAgent()) {
    LOG_TOPIC("e94bb", DEBUG, Logger::TTL)
        << "turning off TTL feature because of coordinator / agency";
    return;
  }

  DatabaseFeature& databaseFeature = server().getFeature<DatabaseFeature>();
  if (databaseFeature.checkVersion() || databaseFeature.upgrade()) {
    LOG_TOPIC("5614a", DEBUG, Logger::TTL)
        << "turning off TTL feature because of version checking or upgrade "
           "procedure";
    return;
  }

  {
    std::lock_guard locker{_propertiesMutex};

    // a frequency of 0 means the thread is not started at all
    if (_properties.frequency == 0) {
      return;
    }
  }

  std::lock_guard locker{_threadMutex};

  if (server().isStopping()) {
    // don't create the thread if we are already shutting down
    return;
  }

  _thread = std::make_unique<TtlThread>(server(), *this);

  if (!_thread->start()) {
    LOG_TOPIC("33c33", FATAL, Logger::TTL)
        << "could not start ttl background thread";
    FATAL_ERROR_EXIT();
  }
}

void TtlFeature::beginShutdown() {
  // this will make the TTL background thread stop as soon as possible
  deactivate();

  std::lock_guard locker{_threadMutex};

  if (_thread != nullptr) {
    // this will also wake up the thread if it should be sleeping
    _thread->beginShutdown();
  }
}

void TtlFeature::stop() { shutdownThread(); }

void TtlFeature::waitForThreadWork() {
  while (true) {
    {
      std::lock_guard locker{_threadMutex};

      if (_thread == nullptr) {
        break;
      }

      _thread->wakeup();

      if (!_thread->isCurrentlyWorking()) {
        break;
      }
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
}

void TtlFeature::activate() {
  {
    std::lock_guard locker{_propertiesMutex};
    if (_active) {
      // already activated
      return;
    }
    _active = true;
  }

  LOG_TOPIC("79862", DEBUG, Logger::TTL) << "activated TTL background thread";
}

void TtlFeature::deactivate() {
  {
    std::lock_guard locker{_propertiesMutex};
    if (!_active) {
      // already deactivated
      return;
    }
    _active = false;
  }

  waitForThreadWork();

  LOG_TOPIC("898a7", DEBUG, Logger::TTL) << "deactivated TTL background thread";
}

bool TtlFeature::isActive() const {
  std::lock_guard locker{_propertiesMutex};
  return _active;
}

void TtlFeature::statsToVelocyPack(VPackBuilder& builder) const {
  std::lock_guard locker{_statisticsMutex};
  _statistics.toVelocyPack(builder);
}

void TtlFeature::updateStats(TtlStatistics const& stats) {
  std::lock_guard locker{_statisticsMutex};
  _statistics += stats;
}

void TtlFeature::propertiesToVelocyPack(VPackBuilder& builder) const {
  std::lock_guard locker{_propertiesMutex};
  _properties.toVelocyPack(builder, _active);
}

TtlProperties TtlFeature::properties() const {
  std::lock_guard locker{_propertiesMutex};
  return _properties;
}

Result TtlFeature::propertiesFromVelocyPack(VPackSlice const& slice,
                                            VPackBuilder& out) {
  Result res;
  uint64_t frequency;
  bool active;

  {
    std::lock_guard locker{_propertiesMutex};

    bool const hasActiveFlag = slice.isObject() && slice.hasKey("active");
    if (hasActiveFlag && !slice.get("active").isBool()) {
      return Result(TRI_ERROR_BAD_PARAMETER,
                    "active flag should be a boolean value");
    }

    // store properties
    res = _properties.fromVelocyPack(slice);
    if (!res.fail() && hasActiveFlag) {
      // update active flag
      _active = slice.get("active").getBool();
    }
    _properties.toVelocyPack(out, _active);
    frequency = _properties.frequency;
    active = _active;
  }

  {
    std::lock_guard locker{_threadMutex};

    if (_thread != nullptr) {
      _thread->setNextStart(frequency);
      _thread->wakeup();

      while (!active && _thread->isCurrentlyWorking()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
    }
  }

  return res;
}

void TtlFeature::shutdownThread() noexcept {
  std::lock_guard locker{_threadMutex};

  if (_thread != nullptr) {
    try {
      _thread->beginShutdown();
      while (_thread->isRunning()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
    } catch (...) {
    }

    _thread.reset();
  }
}
