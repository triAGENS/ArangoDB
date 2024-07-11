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
/// @author Kaveh Vahedipour
////////////////////////////////////////////////////////////////////////////////

#include "State.h"

#include <velocypack/Buffer.h>
#include <velocypack/Slice.h>

#include <chrono>
#include <iomanip>
#include <sstream>
#include <thread>

#include "Agency/Agent.h"
#include "ApplicationFeatures/ApplicationServer.h"
#include "Aql/QueryMethods.h"
#include "Aql/QueryResult.h"
#include "Aql/QueryString.h"
#include "Basics/StaticStrings.h"
#include "Basics/StringUtils.h"
#include "Basics/VelocyPackHelper.h"
#include "Basics/application-exit.h"
#include "Cluster/ServerState.h"
#include "Logger/LogMacros.h"
#include "Transaction/OperationOrigin.h"
#include "Transaction/StandaloneContext.h"
#include "Utils/OperationOptions.h"
#include "Utils/OperationResult.h"
#include "Utils/SingleCollectionTransaction.h"
#include "Utilities/NameValidator.h"
#include "VocBase/LogicalCollection.h"
#include "VocBase/vocbase.h"

#include "Metrics/GaugeBuilder.h"
#include "Metrics/MetricsFeature.h"

using namespace arangodb;
using namespace arangodb::application_features;
using namespace arangodb::aql;
using namespace arangodb::consensus;
using namespace arangodb::velocypack;
using namespace arangodb::rest;
using namespace arangodb::basics;

DECLARE_GAUGE(arangodb_agency_log_size_bytes, uint64_t,
              "Agency replicated log size [bytes]");
DECLARE_GAUGE(arangodb_agency_client_lookup_table_size, uint64_t,
              "Current number of entries in agency client id lookup table");

/// Constructor:
State::State(metrics::MetricsFeature& metrics)
    : _agent(nullptr),
      _vocbase(nullptr),
      _ready(false),
      _collectionsLoaded(false),
      _nextCompactionAfter(0),
      _lastCompactionAt(0),
      _cur(0),
      _log_size(metrics.add(arangodb_agency_log_size_bytes{})),
      _clientIdLookupCount(
          metrics.add(arangodb_agency_client_lookup_table_size{})) {}

/// Default dtor
State::~State() = default;

inline static std::string timestamp(uint64_t m) {
  TRI_ASSERT(m != 0);

  using namespace std::chrono;

  std::time_t t =
      (m == 0)
          ? std::time(nullptr)
          : system_clock::to_time_t(system_clock::time_point(milliseconds(m)));
  char mbstr[100];
  return std::strftime(mbstr, sizeof(mbstr), "%Y-%m-%d %H:%M:%S %Z",
                       std::gmtime(&t))
             ? std::string(mbstr)
             : std::string();
}

inline static std::string stringify(index_t index) {
  std::ostringstream i_str;
  i_str << std::setw(20) << std::setfill('0') << index;
  return i_str.str();
}

/// Persist one entry
bool State::persist(index_t index, term_t term, uint64_t millis,
                    arangodb::velocypack::Slice const& entry,
                    std::string const& clientId) const {
  TRI_IF_FAILURE("State::persist") { return true; }

  LOG_TOPIC("b735e", TRACE, Logger::AGENCY)
      << "persist index=" << index << " term=" << term
      << " entry: " << entry.toJson();

  Builder body;
  {
    VPackObjectBuilder b(&body);
    body.add(StaticStrings::KeyString, Value(stringify(index)));
    body.add("term", Value(term));
    body.add("request", entry);
    body.add("clientId", Value(clientId));
    body.add("timestamp", Value(timestamp(millis)));
    body.add("epoch_millis", Value(millis));
  }

  TRI_ASSERT(_vocbase != nullptr);
  auto origin =
      transaction::OperationOriginInternal{"persisting agency log entry"};
  transaction::StandaloneContext ctx(*_vocbase, origin);
  SingleCollectionTransaction trx(
      std::shared_ptr<transaction::Context>(
          std::shared_ptr<transaction::Context>(), &ctx),
      "log", AccessMode::Type::WRITE);

  trx.addHint(transaction::Hints::Hint::SINGLE_OPERATION);

  Result res = trx.begin();

  if (!res.ok()) {
    THROW_ARANGO_EXCEPTION(res);
  }

  try {
    OperationResult result = trx.insert("log", body.slice(), _options);
    res = trx.finish(result.result);
  } catch (std::exception const& e) {
    LOG_TOPIC("ec1ca", ERR, Logger::AGENCY)
        << "Failed to persist log entry:" << e.what();
    return false;
  }

  LOG_TOPIC("e0321", TRACE, Logger::AGENCY)
      << "persist done index=" << index << " term=" << term
      << " entry: " << entry.toJson() << " ok:" << res.ok();

  return res.ok();
}

bool State::persistConf(index_t index, term_t term, uint64_t millis,
                        arangodb::velocypack::Slice const& entry,
                        std::string const& clientId) const {
  TRI_IF_FAILURE("State::persistConf") { return true; }

  LOG_TOPIC("7d1c0", TRACE, Logger::AGENCY)
      << "persist configuration index=" << index << " term=" << term
      << " entry: " << entry.toJson();

  // The conventional log entry-------------------------------------------------
  Builder log;
  {
    VPackObjectBuilder b(&log);
    log.add(StaticStrings::KeyString, Value(stringify(index)));
    log.add("term", Value(term));
    log.add("request", entry);
    log.add("clientId", Value(clientId));
    log.add("timestamp", Value(timestamp(millis)));
    log.add("epoch_millis", Value(millis));
  }

  // The new configuration to be persisted.-------------------------------------
  // Actual agent's configuration is changed after successful persistence.
  Slice config;
  if (entry.valueAt(0).hasKey("new")) {
    config = entry.valueAt(0).get("new");
  } else {
    config = entry.valueAt(0);
  }
  auto const myId = _agent->id();
  Builder builder;
  if (config.get("id").copyString() != myId) {
    {
      VPackObjectBuilder b(&builder);
      for (auto const& i : VPackObjectIterator(config)) {
        auto key = i.key.copyString();
        if (key == "endpoint") {
          builder.add(key, VPackValue(_agent->endpoint()));
        } else if (key == "id") {
          builder.add(key, VPackValue(myId));
        } else {
          builder.add(key, i.value);
        }
      }
    }
    config = builder.slice();
  }

  Builder configuration;
  {
    VPackObjectBuilder b(&configuration);
    configuration.add(StaticStrings::KeyString, VPackValue("0"));
    configuration.add("cfg", config);
  }

  // Multi docment transaction for log entry and configuration replacement -----
  TRI_ASSERT(_vocbase != nullptr);

  auto origin =
      transaction::OperationOriginInternal{"persisting agency configuration"};
  transaction::StandaloneContext ctx(*_vocbase, origin);
  transaction::Methods trx(std::shared_ptr<transaction::Context>(
                               std::shared_ptr<transaction::Context>(), &ctx),
                           {}, {"log", "configuration"}, {},
                           transaction::Options());

  Result res = trx.begin();
  if (!res.ok()) {
    THROW_ARANGO_EXCEPTION(res);
  }

  try {
    OperationResult logResult = trx.insert("log", log.slice(), _options);
    if (logResult.fail()) {
      THROW_ARANGO_EXCEPTION(logResult.result);
    }
    OperationResult confResult =
        trx.replace("configuration", configuration.slice(), _options);
    res = trx.finish(confResult.result);
  } catch (std::exception const& e) {
    LOG_TOPIC("ced35", ERR, Logger::AGENCY)
        << "Failed to persist log entry: " << e.what();
    return false;
  }

  // Successful persistence affects local configuration ------------------------
  if (res.ok()) {
    _agent->updateConfiguration(config);
  }

  LOG_TOPIC("089ba", TRACE, Logger::AGENCY)
      << "persist done index=" << index << " term=" << term
      << " entry: " << entry.toJson() << " ok:" << res.ok();

  return res.ok();
}

/// Log transaction (leader)
std::vector<index_t> State::logLeaderMulti(
    arangodb::velocypack::Slice transactions,
    std::vector<apply_ret_t> const& applicable, term_t term) {
  using namespace std::chrono;

  if (!transactions.isArray()) {
    THROW_ARANGO_EXCEPTION_MESSAGE(
        TRI_ERROR_AGENCY_MALFORMED_TRANSACTION,
        "Agency syntax requires array of transactions [[<queries>]]");
  }

  std::vector<index_t> idx(applicable.size());
  if (transactions.length() != applicable.size()) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_AGENCY_MALFORMED_TRANSACTION,
                                   "Invalid transaction syntax");
  }

  std::lock_guard mutexLocker{_logLock};

  TRI_ASSERT(!_log.empty());  // log must never be empty

  size_t j = 0;
  for (auto const& i : VPackArrayIterator(transactions)) {
    if (!i.isArray()) {
      THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_AGENCY_MALFORMED_TRANSACTION,
                                     "Transaction syntax is [{<operations>}, "
                                     "{<preconditions>}, \"clientId\"]");
    }

    if (applicable[j] == APPLIED) {
      std::string clientId((i.length() == 3) ? i[2].copyString() : "");

      auto transaction = i[0];
      TRI_ASSERT(transaction.isObject());
      TRI_ASSERT(transaction.length() > 0);
      size_t pos = transaction.keyAt(0).copyString().find(RECONFIGURE);

      idx[j] = logNonBlocking(
          _log.back().index + 1, i[0], term,
          duration_cast<milliseconds>(system_clock::now().time_since_epoch())
              .count(),
          clientId, true, pos == 0 || pos == 1);
    }
    ++j;
  }

  return idx;
}

index_t State::logLeaderSingle(arangodb::velocypack::Slice slice, term_t term,
                               std::string const& clientId) {
  std::lock_guard mutexLocker{_logLock};  // log entries must stay in order
  using namespace std::chrono;
  return logNonBlocking(
      _log.back().index + 1, slice, term,
      duration_cast<milliseconds>(system_clock::now().time_since_epoch())
          .count(),
      clientId, true);
}

/// Log transaction (leader)
index_t State::logNonBlocking(index_t idx, velocypack::Slice slice, term_t term,
                              uint64_t millis, std::string const& clientId,
                              bool leading, bool reconfiguration) {
  // verbose logging for all agency operations
  // there are two different log levels in use here for the AGENCYSTORE topic
  // - DEBUG: will log writes only on the leader
  // - TRACE: will log writes on both leaders and followers
  // the default log level for the AGENCYSTORE topic is WARN
  if (leading) {
    LOG_TOPIC("b578f", DEBUG, Logger::AGENCYSTORE)
        << "leader: true, client: " << clientId << ", index: " << idx
        << ", term: " << term << ", data: " << slice.toJson();
  } else {
    LOG_TOPIC("f586f", TRACE, Logger::AGENCYSTORE)
        << "leader: false, client: " << clientId << ", index: " << idx
        << ", term: " << term << ", data: " << slice.toJson();
  }

  bool success = reconfiguration
                     ? persistConf(idx, term, millis, slice, clientId)
                     : persist(idx, term, millis, slice, clientId);

  if (!success) {  // log to disk or die
    LOG_TOPIC("f5adb", FATAL, Logger::AGENCY)
        << "RAFT member fails to persist log entries!";
    FATAL_ERROR_EXIT();
  }

  auto byteSize = slice.byteSize();
  auto buf = std::make_shared<Buffer<uint8_t>>(byteSize);
  buf->append(slice.begin(), byteSize);

  logEmplaceBackNoLock(log_t(idx, term, std::move(buf), clientId, millis));

  return _log.back().index;
}

void State::logEmplaceBackNoLock(log_t&& l) {
  if (!l.clientId.empty()) {
    try {
      _clientIdLookupTable.emplace(  // keep track of client or die
          std::pair<std::string, index_t>{l.clientId, l.index});
      _clientIdLookupCount += 1;
    } catch (...) {
      LOG_TOPIC("f5ade", FATAL, Logger::AGENCY)
          << "RAFT member fails to expand client lookup table!";
      FATAL_ERROR_EXIT();
    }
  }

  try {
    _log_size += l.entry->byteSize();
    _log.emplace_back(std::forward<log_t>(l));  // log to RAM or die
  } catch (std::bad_alloc const&) {
    LOG_TOPIC("f5adc", FATAL, Logger::AGENCY)
        << "RAFT member fails to allocate volatile log entries!";
    FATAL_ERROR_EXIT();
  }
}

/// Log transactions (follower)
index_t State::logFollower(VPackSlice transactions) {
  size_t nqs = transactions.length();
  using namespace std::chrono;

  while (!_ready && !_agent->isStopping()) {
    LOG_TOPIC("8dd4c", DEBUG, Logger::AGENCY)
        << "Waiting for state to get ready ...";
    std::this_thread::sleep_for(std::chrono::duration<double>(0.1));
  }

  // Check whether we have got a snapshot in the first position:
  bool gotSnapshot = transactions.length() > 0 && transactions[0].isObject() &&
                     !transactions[0].get("readDB").isNone();

  std::lock_guard logLock{_logLock};

  // In case of a snapshot, there are three possibilities:
  //   1. Our highest log index is smaller than the snapshot index, in this
  //      case we must throw away our complete local log and start from the
  //      snapshot (note that snapshot indexes are always committed by a
  //      majority).
  //   2. For the snapshot index we have an entry with this index in
  //      our log (and it is not yet compacted), in this case we verify
  //      that the terms match and if so, we can simply ignore the
  //      snapshot. If the term in our log entry is smaller (cannot be
  //      larger because compaction snapshots are always committed), then
  //      our complete log must be deleted as in 1.
  //   3. Our highest log index is larger than the snapshot index but we
  //      no longer have an entry in the log for the snapshot index due to
  //      our own compaction. In this case we have compacted away the
  //      snapshot index, therefore we know it was committed by a majority
  //      and thus the snapshot can be ignored safely as well.
  if (gotSnapshot) {
    bool useSnapshot = false;  // if this remains, we ignore the snapshot

    index_t snapshotIndex =
        static_cast<index_t>(transactions[0].get("index").getNumber<index_t>());
    term_t snapshotTerm =
        static_cast<term_t>(transactions[0].get("term").getNumber<index_t>());
    index_t ourLastIndex = _log.back().index;
    if (ourLastIndex < snapshotIndex) {
      useSnapshot = true;  // this implies that we completely eradicate our log
    } else {
      try {
        log_t logEntry = atNoLock(snapshotIndex);
        if (logEntry.term != snapshotTerm) {  // can only be < as in 2.
          useSnapshot = true;
        }
      } catch (...) {
        // Simply ignore that we no longer have the entry, useSnapshot remains
        // false and we will ignore the snapshot as in 3. above
      }
    }
    if (useSnapshot) {
      // Now we must completely erase our log and compaction snapshots and
      // start from the snapshot
      Store snapshot("snapshot");
      snapshot.setNodeValue(transactions[0]);
      if (!storeLogFromSnapshot(snapshot, snapshotIndex, snapshotTerm)) {
        LOG_TOPIC("f7250", FATAL, Logger::AGENCY)
            << "Could not restore received log snapshot.";
        FATAL_ERROR_EXIT();
      }
      // Now the log is empty, but this will soon be rectified.
      _nextCompactionAfter =
          snapshotIndex + _agent->config().compactionStepSize();
    }
  }

  size_t ndups = removeConflicts(transactions, gotSnapshot);

  if (nqs > ndups) {
    TRI_ASSERT(transactions.isArray());

    for (size_t i = ndups; i < nqs; ++i) {
      VPackSlice slice = transactions[i];

      auto query = slice.get("query");
      TRI_ASSERT(query.isObject());
      TRI_ASSERT(query.length() > 0);

      auto term = slice.get("term").getUInt();
      auto clientId = slice.get("clientId").copyString();
      auto index = slice.get("index").getUInt();

      uint64_t tstamp = 0;
      if (auto timeSlice = slice.get("timestamp"); timeSlice.isInteger()) {
        // compatibility with older appendEntries protocol
        tstamp = timeSlice.getUInt();
      }
      if (tstamp == 0) {
        tstamp =
            duration_cast<milliseconds>(system_clock::now().time_since_epoch())
                .count();
      }

      bool reconfiguration = query.keyAt(0).isEqualString(RECONFIGURE);

      // first to disk
      if (logNonBlocking(index, query, term, tstamp, clientId, false,
                         reconfiguration) == 0) {
        break;
      }
    }
  }
  return _log.back().index;  // never empty
}

size_t State::removeConflicts(VPackSlice transactions, bool gotSnapshot) {
  // Under _logLock MUTEX from _log, which is the only place calling this.
  // Note that this will ignore a possible snapshot in the first position!
  // This looks through the transactions and skips over those that are
  // already present (or even already compacted). As soon as we find one
  // for which the new term is higher than the locally stored term, we erase
  // the locally stored log from that position and return, such that we
  // can append from this point on the new stuff. If our log is behind,
  // we might find a position at which we do not yet have log entries,
  // in which case we return and let others update our log.
  TRI_ASSERT(transactions.isArray());
  size_t ndups = gotSnapshot ? 1 : 0;

  LOG_TOPIC("4083e", TRACE, Logger::AGENCY)
      << "removeConflicts " << transactions.toJson();
  try {
    // If we've got a snapshot anything we might have is obsolete, note that
    // this happens if and only if we decided at the call site that we actually
    // use the snapshot and we have erased our _log there (see
    // storeLogFromSnapshot which was called above)!
    if (_log.empty()) {
      TRI_ASSERT(gotSnapshot);
      return 1;
    }
    index_t lastIndex = _log.back().index;

    while (ndups < transactions.length()) {
      VPackSlice slice = transactions[ndups];
      index_t idx = slice.get("index").getUInt();
      if (idx > lastIndex) {
        LOG_TOPIC("e3d7a", TRACE, Logger::AGENCY)
            << "removeConflicts " << idx << " > " << lastIndex << " break.";
        break;
      }
      if (idx < _cur) {  // already compacted, treat as equal
        ++ndups;
        continue;
      }
      term_t trm = slice.get("term").getUInt();
      size_t pos = idx - _cur;  // position in _log
      TRI_ASSERT(pos < _log.size());
      if (idx == _log.at(pos).index && trm != _log.at(pos).term) {
        // Found an outdated entry, remove everything from here in our local
        // log. Note that if a snapshot is taken at index cind, then at the
        // entry with index cind is kept in _log to avoid it being
        // empty. Furthermore, compacted indices are always committed by
        // a majority, thus they will never be overwritten. This means
        // that pos here is always a positive index.
        LOG_TOPIC("ec8d0", DEBUG, Logger::AGENCY)
            << "Removing " << _log.size() - pos
            << " entries from log starting with " << idx
            << "==" << _log.at(pos).index << " and " << trm << "="
            << _log.at(pos).term;

        // persisted logs
        std::string const aql(
            "FOR l IN log FILTER l._key >= @key REMOVE l IN log");

        auto bindVars = std::make_shared<VPackBuilder>();
        bindVars->openObject();
        bindVars->add("key", VPackValue(stringify(idx)));
        bindVars->close();

        TRI_ASSERT(
            nullptr !=
            _vocbase);  // this check was previously in the Query constructor
        auto origin = transaction::OperationOriginInternal{
            "removing agency log conflicts"};
        auto queryFuture = arangodb::aql::runStandaloneAqlQuery(
            *_vocbase, origin, aql::QueryString(aql), std::move(bindVars));
        auto queryResult = std::move(queryFuture.waitAndGet());
        if (queryResult.result.fail()) {
          THROW_ARANGO_EXCEPTION(queryResult.result);
        }

        // volatile logs, as mentioned above, this will never make _log
        // completely empty!
        logEraseNoLock(_log.begin() + pos, _log.end());

        LOG_TOPIC("1321d", TRACE, Logger::AGENCY)
            << "removeConflicts done: ndups=" << ndups
            << " first log entry: " << _log.front().index
            << " last log entry: " << _log.back().index;

        break;
      } else {
        ++ndups;
      }
    }
  } catch (std::exception const& e) {
    LOG_TOPIC("9e1df", DEBUG, Logger::AGENCY) << e.what();
  }

  return ndups;
}

void State::logEraseNoLock(std::deque<log_t>::iterator rbegin,
                           std::deque<log_t>::iterator rend) {
  size_t numRemoved = 0;
  uint64_t delSize = 0;

  for (auto lit = rbegin; lit != rend; lit++) {
    std::string const& clientId = lit->clientId;
    delSize += lit->entry->byteSize();
    if (!clientId.empty()) {
      auto ret = _clientIdLookupTable.equal_range(clientId);
      for (auto it = ret.first; it != ret.second;) {
        if (it->second == lit->index) {
          it = _clientIdLookupTable.erase(it);
          ++numRemoved;
        } else {
          it++;
        }
      }
    }
  }

  _clientIdLookupCount -= numRemoved;

  _log.erase(rbegin, rend);
  TRI_ASSERT(delSize <= _log_size.load());
  _log_size -= delSize;
}

std::pair<index_t, index_t> State::determineLogBounds(
    index_t start, index_t end) const noexcept {
  // _logLock must be held when this method is called
  TRI_ASSERT(!_log.empty());

  // start must be greater than or equal to the lowest index
  // and smaller than or equal to the largest index
  if (start < _log.front().index) {
    start = _log.front().index;
  } else if (start > _log.back().index) {
    start = _log.back().index;
  }

  // end must be greater than or equal to start
  // and smaller than or equal to the largest index
  if (end <= start) {
    end = start;
  } else if (end == (std::numeric_limits<uint64_t>::max)() ||
             end > _log.back().index) {
    end = _log.back().index;
  }

  // subtract offset _cur
  start -= _cur;
  end -= (_cur - 1);

  TRI_ASSERT(start <= end);
  return std::make_pair(start, end);
}

/// Get log entries from indices "start" to "end"
std::vector<log_t> State::get(index_t start, index_t end) const {
  std::vector<log_t> entries;
  std::lock_guard mutexLocker{_logLock};  // Cannot be read lock (Compaction)

  if (_log.empty()) {
    return entries;
  }

  auto [s, e] = determineLogBounds(start, end);

  for (size_t i = s; i < e; ++i) {
    // only for debugging purposes
    TRI_ASSERT(i < _log.size()) << [this, sNow = s, eNow = e, start, end]() {
      std::stringstream s;
      s << "log size: " << _log.size() << ", start: " << sNow
        << ", end: " << eNow << ", cur: " << _cur << ", orig start: " << start
        << ", orig end: " << end << ", log entry indexes:";
      for (auto const& it : _log) {
        s << " " << it.index;
      }
      return s.str();
    }();

    entries.push_back(_log[i]);
  }

  return entries;
}

/// Get log entries from indices "start" to "end", into the builder as an array
index_t State::toVelocyPack(velocypack::Builder& result, index_t start,
                            index_t end) const {
  VPackArrayBuilder guard(&result, /*allowUnindexed*/ true);

  std::lock_guard mutexLocker{_logLock};  // Cannot be read lock (Compaction)

  if (_log.empty()) {
    return 0;
  }

  auto [s, e] = determineLogBounds(start, end);

  for (size_t i = s; i < e; ++i) {
    _log[i].toVelocyPackCompact(result);
  }

  return _log[s].index;
}

/// Get log entries from indices "start" to "end"
/// Throws std::out_of_range exception
log_t State::at(index_t index) const {
  std::lock_guard mutexLocker{_logLock};  // Cannot be read lock (Compaction)
  return atNoLock(index);
}

log_t State::atNoLock(index_t index) const {
  if (_cur > index) {
    std::string excMessage =
        std::string(
            "Access before the start of the log deque: (first, requested): (") +
        std::to_string(_cur) + ", " + std::to_string(index);
    LOG_TOPIC("06fe5", DEBUG, Logger::AGENCY) << excMessage;
    throw std::out_of_range(excMessage);
  }

  auto pos = index - _cur;
  if (pos >= _log.size()) {
    std::string excMessage =
        std::string(
            "Access beyond the end of the log deque: (last, requested): (") +
        std::to_string(_cur + _log.size()) + ", " + std::to_string(index) + ")";
    LOG_TOPIC("96882", DEBUG, Logger::AGENCY) << excMessage;
    throw std::out_of_range(excMessage);
  }

  return _log[pos];
}

/// Check for a log entry, returns 0, if the log does not contain an entry
/// with index `index`, 1, if it does contain one with term `term` and
/// -1, if it does contain one with another term than `term`:
int State::checkLog(index_t index, term_t term) const {
  std::lock_guard mutexLocker{_logLock};  // Cannot be read lock (Compaction)

  // If index above highest entry
  if (!_log.empty() && index > _log.back().index) {
    return -1;
  }

  // Catch exceptions and avoid overflow:
  if (index < _cur || index - _cur >= _log.size()) {
    return 0;
  }

  return _log[index - _cur].term == term ? 1 : -1;
}

/// Have log with specified index and term
bool State::has(index_t index, term_t term) const {
  std::lock_guard mutexLocker{_logLock};  // Cannot be read lock (Compaction)

  // Catch exceptions and avoid overflow:
  if (index < _cur || index - _cur >= _log.size()) {
    return false;
  }

  return _log[index - _cur].term == term;
}

/// Get vector of past transaction from 'start' to 'end'
VPackBuilder State::slices(index_t start, index_t end) const {
  VPackBuilder slices;
  slices.openArray();

  std::unique_lock mutexLocker{_logLock};  // Cannot be read lock (Compaction)

  if (!_log.empty()) {
    if (start < _log.front().index) {  // no start specified
      start = _log.front().index;
    }

    if (start > _log.back().index) {  // no end specified
      slices.close();
      return slices;
    }

    if (end == (std::numeric_limits<uint64_t>::max)() ||
        end > _log.back().index) {
      end = _log.back().index;
    }

    for (size_t i = start - _cur; i <= end - _cur; ++i) {
      try {  //{ "a" : {"op":"set", "ttl":20, ...}}
        auto slice = VPackSlice(_log.at(i).entry->data());
        VPackObjectBuilder o(&slices);
        for (auto const& oper : VPackObjectIterator(slice)) {
          slices.add(VPackValue(oper.key.copyString()));

          if (oper.value.isObject() && oper.value.hasKey("op") &&
              oper.value.get("op").isEqualString("set") &&
              oper.value.hasKey("ttl")) {
            VPackObjectBuilder oo(&slices);
            for (auto const& i : VPackObjectIterator(oper.value)) {
              slices.add(i.key.stringView(), i.value);
            }
            slices.add("epoch_millis",
                       VPackValue(_log.at(i).timestamp.count()));
          } else {
            slices.add(oper.value);
          }
        }
      } catch (std::exception const&) {
        break;
      }
    }
  }

  mutexLocker.unlock();
  slices.close();
  return slices;
}

/// Get log entry by log index, copy entry because we do no longer have the
/// lock after the return
log_t State::operator[](index_t index) const {
  std::lock_guard mutexLocker{_logLock};  // Cannot be read lock (Compaction)
  TRI_ASSERT(index - _cur < _log.size());
  return _log.at(index - _cur);
}

/// Get last log entry, copy entry because we do no longer have the lock
/// after the return
log_t State::lastLog() const {
  std::lock_guard mutexLocker{_logLock};  // Cannot be read lock (Compaction)
  TRI_ASSERT(!_log.empty());
  return _log.back();
}

/// Configure with agent
bool State::configure(Agent* agent) {
  _agent = agent;
  _nextCompactionAfter = _agent->config().compactionStepSize();
  return true;
}

/// Check collection by name
bool State::checkCollection(std::string const& name) {
  return (_vocbase->lookupCollection(name) != nullptr);
}

/// Drop
void State::dropCollection(std::string const& colName) {
  try {
    auto col = _vocbase->lookupCollection(colName);
    if (col == nullptr) {
      return;
    }
    auto res = _vocbase->dropCollection(col->id(), false);
    if (res.fail()) {
      LOG_TOPIC("ba841", FATAL, Logger::AGENCY)
          << "unable to drop collection '" << colName
          << "': " << res.errorMessage();
      FATAL_ERROR_EXIT();
    }
  } catch (std::exception const& e) {
    LOG_TOPIC("69f4c", FATAL, Logger::AGENCY)
        << "unable to drop collection '" << colName << "': " << e.what();
    FATAL_ERROR_EXIT();
  }
}

/// Create collection by name
bool State::ensureCollection(std::string const& name, bool drop) {
  if (_vocbase->lookupCollection(name) != nullptr) {
    // collection already exists
    return true;
  }

  Builder body;
  {
    VPackObjectBuilder b(&body);
    body.add(StaticStrings::DataSourceType,
             VPackValue(static_cast<int>(TRI_COL_TYPE_DOCUMENT)));
    body.add(StaticStrings::DataSourceName, VPackValue(name));
    body.add(StaticStrings::DataSourceSystem,
             VPackValue(NameValidator::isSystemName(name)));
  }

  if (drop && _vocbase->lookupCollection(name) != nullptr) {
    dropCollection(name);
  }

  auto collection = _vocbase->createCollection(body.slice());

  if (collection == nullptr) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_errno(), "cannot create collection");
  }

  return true;
}

// Are we ready for action?
bool State::ready() const { return _ready; }

/// Load collections
bool State::loadCollections(TRI_vocbase_t* vocbase, bool waitForSync) {
  using namespace std::chrono;
  auto const epoch_millis =
      duration_cast<milliseconds>(system_clock::now().time_since_epoch())
          .count();

  _vocbase = vocbase;
  TRI_ASSERT(_vocbase != nullptr);

  _options.waitForSync = waitForSync;
  _options.silent = true;

  if (loadPersisted()) {
    std::lock_guard logLock{_logLock};
    if (_log.empty()) {
      std::shared_ptr<Buffer<uint8_t>> buf =
          std::make_shared<Buffer<uint8_t>>();
      VPackSlice value = arangodb::velocypack::Slice::emptyObjectSlice();
      buf->append(value.startAs<char const>(), value.byteSize());
      _log.emplace_back(
          log_t(index_t(0), term_t(0), buf, std::string(), epoch_millis));
      _log_size += value.byteSize();
      persist(0, 0, epoch_millis, value, std::string());
    }
    _ready = true;
    return true;
  }

  return false;
}

/// Load actually persisted collections
bool State::loadPersisted() {
  TRI_ASSERT(_vocbase != nullptr);

  ensureCollection("configuration", false);
  loadOrPersistConfiguration();

  ensureCollection("election", false);

  if (checkCollection("log") && checkCollection("compact")) {
    index_t lastCompactionIndex = loadCompacted();
    if (loadRemaining(lastCompactionIndex)) {
      return true;
    } else {
      LOG_TOPIC("1a476", INFO, Logger::AGENCY)
          << "Non matching compaction and log indexes. Dropping both "
             "collections";
      {
        std::lock_guard logLock{_logLock};
        _log.clear();
        _cur = 0;
      }
      dropCollection("log");
      dropCollection("compact");
    }
  }

  LOG_TOPIC("9e72a", DEBUG, Logger::AGENCY)
      << "No persisted log: creating collections.";

  // This is a combined create of logs and compact, as otherwise inconsistencies
  // in log and compact cannot be mitigated after this point. We are here
  // because of the above missing / incomplete log / compaction. Including the
  // case of only one of the two collections being present.
  dropCollection("log");
  dropCollection("compact");
  ensureCollection("log", true);
  ensureCollection("compact", true);

  return true;
}

/// @brief load a compacted snapshot, returns true if successful and false
/// otherwise. In case of success store and index are modified. The store
/// is reset to the state after log index `index` has been applied. Sets
/// `index` to 0 if there is no compacted snapshot.
bool State::loadLastCompactedSnapshot(Store& store, index_t& index,
                                      term_t& term) {
  std::string const aql("FOR c IN compact SORT c._key DESC LIMIT 1 RETURN c");

  TRI_ASSERT(nullptr != _vocbase);
  auto origin = transaction::OperationOriginInternal{
      "loading last compacted agency snapshot"};
  auto queryFuture = arangodb::aql::runStandaloneAqlQuery(
      *_vocbase, origin, aql::QueryString(aql), nullptr);
  auto queryResult = std::move(queryFuture.waitAndGet());

  if (queryResult.result.fail()) {
    THROW_ARANGO_EXCEPTION(queryResult.result);
  }

  VPackSlice result = queryResult.data->slice();
  // AQL results are guaranteed to be arrays
  TRI_ASSERT(result.isArray());

  // Default: No compaction snapshot yet
  index = 0;
  term = 0;

  if (result.length()) {
    // we queried with LIMIT 1, so there must be exactly one result now
    TRI_ASSERT(result.length() == 1);

    VPackSlice ii = result[0];
    try {
      store.setNodeValue(ii);
      index = extractIndexFromKey(ii);
      term = ii.get("term").getNumber<uint64_t>();
    } catch (std::exception const& e) {
      LOG_TOPIC("8ef2a", ERR, Logger::AGENCY) << e.what();
      return false;
    }
  }

  return true;
}

/// Load compaction collection
index_t State::loadCompacted() {
  std::string const aql("FOR c IN compact SORT c._key DESC LIMIT 1 RETURN c");

  TRI_ASSERT(nullptr !=
             _vocbase);  // this check was previously in the Query constructor
  auto origin =
      transaction::OperationOriginInternal{"loading compacted agency snapshot"};
  auto queryFuture = arangodb::aql::runStandaloneAqlQuery(
      *_vocbase, origin, aql::QueryString(aql), nullptr);
  auto queryResult = std::move(queryFuture.waitAndGet());

  if (queryResult.result.fail()) {
    THROW_ARANGO_EXCEPTION(queryResult.result);
  }

  VPackSlice result = queryResult.data->slice();
  // AQL results are guaranteed to be arrays
  TRI_ASSERT(result.isArray());

  if (result.length()) {
    // Result can only have length 0 or 1.
    VPackSlice ii = result[0];

    std::lock_guard logLock{_logLock};
    _agent->setPersistedState(ii);
    try {
      _cur = extractIndexFromKey(ii);
      _log.clear();  // will be filled in loadRemaining
      _log_size = 0;
      _clientIdLookupTable.clear();
      _clientIdLookupCount = 0;
      // Schedule next compaction:
      _lastCompactionAt = _cur;
      _nextCompactionAfter = _cur + _agent->config().compactionStepSize();
    } catch (std::exception const& e) {
      _cur = 0;
      LOG_TOPIC("bc330", ERR, Logger::AGENCY) << e.what();
    }
  }

  return _cur;
}

/// Load persisted configuration
bool State::loadOrPersistConfiguration() {
  auto loadConfiguration = [&]() -> aql::QueryResult {
    std::string const aql(
        "FOR c in configuration FILTER c._key==\"0\" RETURN c.cfg");

    TRI_ASSERT(nullptr !=
               _vocbase);  // this check was previously in the Query constructor
    auto origin =
        transaction::OperationOriginInternal{"loading agency configuration"};
    auto queryFuture = arangodb::aql::runStandaloneAqlQuery(
        *_vocbase, origin, aql::QueryString(aql), nullptr);
    return std::move(queryFuture.waitAndGet());
  };

  aql::QueryResult queryResult = loadConfiguration();

  if (queryResult.result.fail()) {
    THROW_ARANGO_EXCEPTION(queryResult.result);
  }

  VPackSlice result = queryResult.data->slice();
  // AQL results are guaranteed to be arrays
  TRI_ASSERT(result.isArray());

  if (result.length()) {  // We already have a persisted conf
    auto conf = result[0];
    TRI_ASSERT(conf.hasKey("id"));
    auto id = conf.get("id");

    TRI_ASSERT(id.isString());
    if (ServerState::instance()->hasPersistedId()) {
      TRI_ASSERT(id.copyString() == ServerState::instance()->getPersistedId());
    } else {
      ServerState::instance()->writePersistedId(id.copyString());
    }

    try {
      LOG_TOPIC("504da", DEBUG, Logger::AGENCY)
          << "Merging configuration " << conf.toJson();
      _agent->mergeConfiguration(conf);
      auto pool = _agent->config().pool();
      auto it = pool.find(_agent->config().id());
      if (it == pool.end()) {
        LOG_TOPIC("6acd3", FATAL, Logger::AGENCY)
            << "Ended up with a pool of agents which does not include "
               "ourselves, configuration: "
            << _agent->config().toBuilder()->slice().toJson();
        FATAL_ERROR_EXIT();
      }
    } catch (std::exception const& e) {
      LOG_TOPIC("6acd2", FATAL, Logger::AGENCY)
          << "Failed to merge persisted configuration into runtime "
             "configuration: "
          << e.what();
      FATAL_ERROR_EXIT();
    }

  } else {  // Fresh start or disaster recovery

    std::lock_guard guard{_configurationWriteLock};

    LOG_TOPIC("a27cb", DEBUG, Logger::AGENCY) << "New agency!";

    TRI_ASSERT(_agent != nullptr);

    // If we have persisted id, we use that. Otherwise we check, if we were
    // given a disaster recovery id that wins then before a new one is generated
    // and that choice persisted.
    std::string uuid;
    if (ServerState::instance()->hasPersistedId()) {
      uuid = ServerState::instance()->getPersistedId();
    } else {
      std::string recoveryId = _agent->config().recoveryId();
      if (recoveryId.empty()) {
        uuid = ServerState::instance()->generatePersistedId(
            ServerState::ROLE_AGENT);
      } else {
        uuid = recoveryId;
        ServerState::instance()->writePersistedId(recoveryId);
      }
    }
    _agent->id(uuid);
    ServerState::instance()->setId(uuid);

    auto origin =
        transaction::OperationOriginInternal{"storing agency configuration"};
    transaction::StandaloneContext ctx(*_vocbase, origin);
    SingleCollectionTransaction trx(
        std::shared_ptr<transaction::Context>(
            std::shared_ptr<transaction::Context>(), &ctx),
        "configuration", AccessMode::Type::WRITE);
    Result res = trx.begin();

    if (!res.ok()) {
      THROW_ARANGO_EXCEPTION(res);
    }

    Builder doc;
    {
      VPackObjectBuilder d(&doc);
      doc.add(StaticStrings::KeyString, VPackValue("0"));
      doc.add("cfg", _agent->config().toBuilder()->slice());
    }

    try {
      OperationResult result =
          trx.insert("configuration", doc.slice(), _options);
      res = trx.finish(result.result);
    } catch (std::exception const& e) {
      LOG_TOPIC("4384a", FATAL, Logger::AGENCY)
          << "Failed to persist configuration entry:" << e.what();
      FATAL_ERROR_EXIT();
    }

    LOG_TOPIC("c5d88", DEBUG, Logger::AGENCY)
        << "Persisted configuration: " << doc.slice().toJson();

    return res.ok();
  }

  return true;
}

/// Load beyond last compaction and check if compaction index
/// matches any log entry
bool State::loadRemaining(index_t cind) {
  index_t lastIndex;
  {
    // read current index initially, which should be the last compacted
    // state. we can this as the lower bound of log entries we will be
    // interested in, because we will skip anything with a lower index
    // value anyway.
    // note that further down below we will fetch _cur once again, and
    // that it may have moved forward then. this does not matter, because
    // we are using the initial _cur value only as a lower bound in the
    // AQL query, but use the second _cur value for the actual, per-log
    // entry filtering.
    std::lock_guard logLock{_logLock};
    lastIndex = _cur;
  }

  auto bindVars = std::make_shared<VPackBuilder>();
  bindVars->openObject();
  // in case lastIndex is still 0, we need to compare against and include
  // key "0", which sorts lower than the full-padded key "00000000000000000000".
  bindVars->add("key", VPackValue(lastIndex == 0 ? "0" : stringify(lastIndex)));
  bindVars->close();
  bool match = false;

  std::string const aql(
      "FOR l IN log FILTER l._key >= @key SORT l._key RETURN l");

  TRI_ASSERT(nullptr !=
             _vocbase);  // this check was previously in the Query constructor
  auto origin = transaction::OperationOriginInternal{
      "loading remaining agency log entries"};
  auto queryFuture = arangodb::aql::runStandaloneAqlQuery(
      *_vocbase, origin, aql::QueryString(aql), std::move(bindVars));
  auto queryResult = std::move(queryFuture.waitAndGet());

  if (queryResult.result.fail()) {
    THROW_ARANGO_EXCEPTION(queryResult.result);
  }

  auto result = queryResult.data->slice();

  std::lock_guard logLock{_logLock};
  if (result.isArray() && result.length() > 0) {
    TRI_ASSERT(_log.empty());  // was cleared in loadCompacted
    // We know that _cur has been set in loadCompacted to the index of the
    // snapshot that was loaded or to 0 if there is no snapshot.
    lastIndex = _cur;
    std::string clientId;

    for (auto const& ii : VPackArrayIterator(result)) {
      // _key
      index_t const index = extractIndexFromKey(ii);

      // Ignore log entries, which are older than lastIndex:
      if (index < lastIndex) {
        continue;
      }

      auto req = ii.get("request");

      // clientId
      if (auto clientSlice = ii.get("clientId"); clientSlice.isString()) {
        clientId = clientSlice.copyString();
      } else {
        clientId.clear();
      }

      // epoch_millis
      uint64_t millis = 0;
      if (auto milliSlice = ii.get("epoch_millis"); milliSlice.isNumber()) {
        try {
          millis = milliSlice.getNumber<uint64_t>();
        } catch (...) {
        }
      }

      // Empty patches :
      if (index > lastIndex + 1) {
        // Dummy fill missing entries (Not good at all.)
        std::shared_ptr<Buffer<uint8_t>> buf =
            std::make_shared<Buffer<uint8_t>>();
        VPackSlice value = arangodb::velocypack::Slice::emptyObjectSlice();
        buf->append(value.startAs<char const>(), value.byteSize());
        term_t term(ii.get("term").getNumber<uint64_t>());
        for (index_t i = lastIndex + 1; i < index; ++i) {
          LOG_TOPIC("f95c7", WARN, Logger::AGENCY)
              << "Missing index " << i << " in RAFT log.";
          _log.emplace_back(log_t(i, term, buf, std::string()));
          _log_size += value.byteSize();
          // This has empty clientId, so we do not need to adjust
          // _clientIdLookupTable.
          lastIndex = i;
        }
        // After this loop, index will be lastIndex + 1
      }

      if (index == lastIndex + 1 || (index == lastIndex && _log.empty())) {
        // Real entries
        buffer_t tmp =
            std::make_shared<arangodb::velocypack::Buffer<uint8_t>>();
        tmp->append(req.startAs<char const>(), req.byteSize());

        logEmplaceBackNoLock(log_t(index, ii.get("term").getNumber<uint64_t>(),
                                   std::move(tmp), clientId, millis));
        if (index == cind) {
          match = true;
        }
        lastIndex = index;
      }
    }
  }

  return !_log.empty() && match;
}

/// Find entry by index and term
bool State::find(index_t prevIndex, term_t prevTerm) {
  std::lock_guard mutexLocker{_logLock};
  if (prevIndex >= _log.size()) {
    return false;
  }
  return _log[prevIndex].term == prevTerm;
}

index_t State::lastCompactionAt() const { return _lastCompactionAt; }

/// Log compaction
bool State::compact(index_t cind, index_t keep) {
  TRI_IF_FAILURE("State::compact") { return true; }

  // We need to compute the state at index cind and use:
  //   cind <= _commitIndex
  // We start at the latest compaction state and advance from there:
  // We keep at least `keep` log entries before the compacted state,
  // for forensic analysis and such that the log is never empty.
  {
    std::lock_guard _logLocker{_logLock};
    if (cind <= _cur) {
      LOG_TOPIC("69afe", DEBUG, Logger::AGENCY)
          << "Not compacting log at index " << cind
          << ", because we already have a later snapshot at index " << _cur;
      return true;
    }
  }

  // Move next compaction index forward to avoid a compaction wakeup
  // whilst we are working:
  _nextCompactionAfter =
      (std::max)(_nextCompactionAfter.load(),
                 cind + _agent->config().compactionStepSize());

  Store snapshot("snapshot");
  index_t index;
  term_t term;
  if (!loadLastCompactedSnapshot(snapshot, index, term)) {
    return false;
  }
  if (index > cind) {
    LOG_TOPIC("2cda3", ERR, Logger::AGENCY)
        << "Strange, last compaction snapshot " << index << " is younger than "
        << "currently attempted snapshot " << cind;
    return false;
  } else if (index == cind) {
    return true;  // already have snapshot for this index
  } else {        // now we know index < cind
    // Apply log entries to snapshot up to and including index cind:
    auto logs = slices(index + 1, cind);
    log_t last = at(cind);
    snapshot.applyLogEntries(logs, cind, last.term);

    if (!persistCompactionSnapshot(cind, last.term, snapshot)) {
      LOG_TOPIC("3b34a", ERR, Logger::AGENCY)
          << "Could not persist compaction snapshot.";
      return false;
    }
  }

  // Now clean up old stuff which is included in the latest compaction snapshot:
  try {
    compactVolatile(cind, keep);
    compactPersisted(cind, keep);
    removeObsolete(cind);
  } catch (std::exception const& e) {
    if (!_agent->isStopping()) {
      LOG_TOPIC("13fc9", ERR, Logger::AGENCY)
          << "Failed to compact persisted store.";
      LOG_TOPIC("33ff0", ERR, Logger::AGENCY) << e.what();
    } else {
      LOG_TOPIC("474ae", INFO, Logger::AGENCY)
          << "Failed to compact persisted store "
             "(no problem, already in shutdown).";
      LOG_TOPIC("62b5d", INFO, Logger::AGENCY) << e.what();
    }
  }
  return true;
}

/// Compact volatile state
bool State::compactVolatile(index_t cind, index_t keep) {
  // Note that we intentionally keep some log entries before cind
  // although it is, strictly speaking, no longer necessary. This is to
  // make sure that _log does not become empty! DO NOT CHANGE! This is
  // used elsewhere in the code! Furthermore, it allows for forensic
  // analysis in case of bad things having happened.
  if (keep >= cind) {  // simply keep everything
    return true;
  }
  TRI_ASSERT(keep < cind);
  index_t cut = cind - keep;
  std::lock_guard mutexLocker{_logLock};
  if (!_log.empty() && cut > _cur && cut - _cur < _log.size()) {
    logEraseNoLock(_log.begin(), _log.begin() + (cut - _cur));
    TRI_ASSERT(_log.begin()->index == cut);
    _cur = _log.begin()->index;
  }
  return true;
}

/// Compact persisted state
bool State::compactPersisted(index_t cind, index_t keep) {
  // Note that we intentionally keep some log entries before cind
  // although it is, strictly speaking, no longer necessary. This is to
  // make sure that _log does not become empty! DO NOT CHANGE! This is
  // used elsewhere in the code! Furthermore, it allows for forensic
  // analysis in case of bad things having happened.
  if (keep >= cind) {  // simply keep everything
    return true;
  }
  TRI_ASSERT(keep < cind);
  index_t cut = cind - keep;

  auto bindVars = std::make_shared<VPackBuilder>();
  auto cutStr = stringify(cut);
  bindVars->openObject();
  bindVars->add("key", VPackValue(cutStr));
  bindVars->close();

  std::string const aql("FOR l IN log FILTER l._key < @key REMOVE l IN log");

  TRI_ASSERT(nullptr !=
             _vocbase);  // this check was previously in the Query constructor

  LOG_TOPIC("a8123", DEBUG, Logger::AGENCY)
      << "Removing log entries with keys lower than " << cutStr;

  auto origin =
      transaction::OperationOriginInternal{"compacting agency log entries"};
  auto queryFuture = arangodb::aql::runStandaloneAqlQuery(
      *_vocbase, origin, aql::QueryString(aql), std::move(bindVars));
  auto queryResult = std::move(queryFuture.waitAndGet());

  if (queryResult.result.fail()) {
    THROW_ARANGO_EXCEPTION(queryResult.result);
  }

  LOG_TOPIC("a8132", DEBUG, Logger::AGENCY) << "Compacting log collection";
  try {
    auto col = _vocbase->lookupCollection("log");
    if (col == nullptr) {
      LOG_TOPIC("d131f", FATAL, Logger::AGENCY)
          << "Failed to find log collection in agency.";
      FATAL_ERROR_EXIT();
    } else {
      col->compact();
    }
  } catch (const std::exception& e) {
    LOG_TOPIC("d13f1", WARN, Logger::AGENCY)
        << "Failed to compact agent's physical log collection: " << e.what();
  }

  return true;
}

/// Remove outdated compaction snapshots
bool State::removeObsolete(index_t cind) {
  if (cind > 3 * _agent->config().compactionKeepSize()) {
    auto bindVars = std::make_shared<VPackBuilder>();
    bindVars->openObject();
    bindVars->add("key",
                  VPackValue(stringify(
                      cind - 3 * _agent->config().compactionKeepSize())));
    bindVars->close();

    std::string const aql(
        "FOR c IN compact FILTER c._key < @key REMOVE c IN compact");

    TRI_ASSERT(nullptr !=
               _vocbase);  // this check was previously in the Query constructor
    auto origin = transaction::OperationOriginInternal{
        "removing obsolete agency snapshots"};
    auto queryFuture = arangodb::aql::runStandaloneAqlQuery(
        *_vocbase, origin, aql::QueryString(aql), std::move(bindVars));
    auto queryResult = std::move(queryFuture.waitAndGet());

    if (queryResult.result.fail()) {
      THROW_ARANGO_EXCEPTION(queryResult.result);
    }
  }

  return true;
}

/// Persist a compaction snapshot
bool State::persistCompactionSnapshot(index_t cind,
                                      arangodb::consensus::term_t term,
                                      arangodb::consensus::Store& snapshot) {
  TRI_IF_FAILURE("State::persistCompactionSnapshot") { return true; }

  if (checkCollection("compact")) {
    Builder store;
    {
      VPackObjectBuilder s(&store);
      store.add(VPackValue("readDB"));
      {
        VPackArrayBuilder a(&store);
        snapshot.dumpToBuilder(store);
      }
      store.add("term", VPackValue(term));
      store.add(StaticStrings::KeyString, VPackValue(stringify(cind)));
      store.add("version", VPackValue(2));
    }

    TRI_ASSERT(_vocbase != nullptr);
    auto origin = transaction::OperationOriginInternal{
        "persisting agency compacted snapshot"};
    transaction::StandaloneContext ctx(*_vocbase, origin);
    SingleCollectionTransaction trx(
        std::shared_ptr<transaction::Context>(
            std::shared_ptr<transaction::Context>(), &ctx),
        "compact", AccessMode::Type::WRITE);

    Result res = trx.begin();

    if (!res.ok()) {
      THROW_ARANGO_EXCEPTION(res);
    }

    try {
      OperationResult result = trx.insert("compact", store.slice(), _options);
      if (!result.ok()) {
        if (result.is(TRI_ERROR_ARANGO_UNIQUE_CONSTRAINT_VIOLATED)) {
          LOG_TOPIC("b1b55", DEBUG, Logger::AGENCY)
              << "Failed to insert compacted agency state, will attempt to "
                 "update: "
              << result.errorMessage();
          result = trx.replace("compact", store.slice(), _options);
        } else {
          LOG_TOPIC("a9124", FATAL, Logger::AGENCY)
              << "Failed to persist compacted agency state"
              << result.errorMessage();
          FATAL_ERROR_EXIT();
        }
      }
      res = trx.finish(result.result);
    } catch (std::exception const& e) {
      LOG_TOPIC("41965", FATAL, Logger::AGENCY)
          << "Failed to persist compacted agency state: " << e.what();
      FATAL_ERROR_EXIT();
    }

    if (res.ok()) {
      _lastCompactionAt = cind;
    }

    return res.ok();
  }

  LOG_TOPIC("65d2a", ERR, Logger::AGENCY)
      << "Failed to persist snapshot for compaction!";

  return false;
}

/// @brief restoreLogFromSnapshot, needed in the follower, this erases the
/// complete log and persists the given snapshot. After this operation, the
/// log is empty and something ought to be appended to it rather quickly.
bool State::storeLogFromSnapshot(Store& snapshot, index_t index, term_t term) {
  if (!persistCompactionSnapshot(index, term, snapshot)) {
    LOG_TOPIC("a3f20", ERR, Logger::AGENCY)
        << "Could not persist received log snapshot.";
    return false;
  }

  // Now we need to completely erase our log, both persisted and volatile:
  LOG_TOPIC("acd42", DEBUG, Logger::AGENCY)
      << "Removing complete log because of new snapshot.";

  // persisted logs
  std::string const aql("FOR l IN log REMOVE l IN log");

  TRI_ASSERT(nullptr !=
             _vocbase);  // this check was previously in the Query constructor
  auto origin =
      transaction::OperationOriginInternal{"removing all agency log entries"};
  auto queryFuture = arangodb::aql::runStandaloneAqlQuery(
      *_vocbase, origin, aql::QueryString(aql), nullptr);
  auto queryResult = std::move(queryFuture.waitAndGet());

  // We ignore the result, in the worst case we have some log entries
  // too many.

  // volatile logs
  _log.clear();
  _log_size = 0;
  _clientIdLookupTable.clear();
  _clientIdLookupCount = 0;
  _cur = index;

  // This empty log should soon be rectified!
  return true;
}

void State::persistActiveAgents(query_t const& active, query_t const& pool) {
  TRI_ASSERT(_vocbase != nullptr);

  Builder builder;
  {
    VPackObjectBuilder guard(&builder);
    builder.add(StaticStrings::KeyString, VPackValue("0"));
    builder.add(VPackValue("cfg"));
    {
      VPackObjectBuilder guard2(&builder);
      builder.add("active", active->slice());
      builder.add("pool", pool->slice());
    }
  }

  auto origin =
      transaction::OperationOriginInternal{"updating agency configuration"};
  transaction::StandaloneContext ctx(*_vocbase, origin);

  std::lock_guard guard{_configurationWriteLock};
  SingleCollectionTransaction trx(
      std::shared_ptr<transaction::Context>(
          std::shared_ptr<transaction::Context>(), &ctx),
      "configuration", AccessMode::Type::WRITE);
  Result res = trx.begin();

  if (!res.ok()) {
    THROW_ARANGO_EXCEPTION(res);
  }

  auto result = trx.update("configuration", builder.slice(), _options);

  if (result.fail()) {
    THROW_ARANGO_EXCEPTION(result.result);
  }

  res = trx.finish(result.result);

  if (!res.ok()) {
    THROW_ARANGO_EXCEPTION(res);
  }

  LOG_TOPIC("4c42c", DEBUG, Logger::AGENCY)
      << "Updated persisted agency configuration: " << builder.slice().toJson();
}

query_t State::allLogs() const {
  std::string const comp("FOR c IN compact SORT c._key RETURN c");
  std::string const logs("FOR l IN log SORT l._key RETURN l");

  TRI_ASSERT(nullptr != _vocbase);

  std::lock_guard mutexLocker{_logLock};

  auto origin =
      transaction::OperationOriginInternal{"retrieving all agency logs"};

  auto compQueryFuture = arangodb::aql::runStandaloneAqlQuery(
      *_vocbase, origin, aql::QueryString(comp), nullptr);
  auto compqResult = std::move(compQueryFuture.waitAndGet());
  if (compqResult.result.fail()) {
    THROW_ARANGO_EXCEPTION(compqResult.result);
  }

  auto logsQFuture = arangodb::aql::runStandaloneAqlQuery(
      *_vocbase, origin, aql::QueryString(logs), nullptr);
  auto logsqResult = std::move(logsQFuture.waitAndGet());

  if (logsqResult.result.fail()) {
    THROW_ARANGO_EXCEPTION(logsqResult.result);
  }

  auto everything = std::make_shared<VPackBuilder>();
  {
    VPackObjectBuilder(everything.get());
    try {
      everything->add("compact", compqResult.data->slice());
    } catch (std::exception const&) {
      LOG_TOPIC("1face", ERR, Logger::AGENCY)
          << "Failed to assemble compaction part of everything package";
    }
    try {
      everything->add("logs", logsqResult.data->slice());
    } catch (std::exception const&) {
      LOG_TOPIC("fe816", ERR, Logger::AGENCY)
          << "Failed to assemble remaining part of everything package";
    }
  }
  return everything;
}

std::vector<index_t> State::inquire(velocypack::Slice query) const {
  if (!query.isArray()) {
    THROW_ARANGO_EXCEPTION_MESSAGE(
        TRI_ERROR_AGENCY_MALFORMED_INQUIRE_REQUEST,
        std::string(
            "Inquiry handles a list of string clientIds: [<clientId>] ") +
            ". We got " + query.toJson());
  }

  std::vector<index_t> result;
  size_t pos = 0;

  std::lock_guard mutexLocker{_logLock};  // Cannot be read lock (Compaction)
  for (auto i : VPackArrayIterator(query)) {
    if (!i.isString()) {
      THROW_ARANGO_EXCEPTION_MESSAGE(
          TRI_ERROR_AGENCY_MALFORMED_INQUIRE_REQUEST,
          std::string("ClientIds must be strings. On position ") +
              std::to_string(pos++) + " we got " + i.toJson());
    }

    auto ret = _clientIdLookupTable.equal_range(i.copyString());
    index_t index = 0;
    // Look for the maximum index:
    for (auto it = ret.first; it != ret.second; ++it) {
      if (it->second > index) {
        index = it->second;
      }
    }
    result.push_back(index);
  }

  return result;
}

// Index of last log entry
index_t State::lastIndex() const {
  std::lock_guard mutexLocker{_logLock};
  TRI_ASSERT(!_log.empty());
  return _log.back().index;
}

// Index of last log entry
index_t State::firstIndex() const {
  std::lock_guard mutexLocker{_logLock};
  TRI_ASSERT(!_log.empty());
  return _cur;
}

/// @brief this method is intended for manual recovery only. It only looks
/// at the persisted data structure and tries to recover the latest state.
/// The returned builder has the complete state of the agency and index
/// is set to the index of the last log entry and term is set to the term
/// of the last entry.
std::shared_ptr<VPackBuilder> State::latestAgencyState(TRI_vocbase_t& vocbase,
                                                       index_t& index,
                                                       term_t& term) {
  auto origin = transaction::OperationOriginInternal{
      "querying latest agency snapshot state"};
  // First get the latest snapshot, if there is any:
  std::string aql("FOR c IN compact SORT c._key DESC LIMIT 1 RETURN c");
  auto queryFuture = arangodb::aql::runStandaloneAqlQuery(
      vocbase, origin, aql::QueryString(aql), nullptr);
  auto queryResult = std::move(queryFuture.waitAndGet());

  if (queryResult.result.fail()) {
    THROW_ARANGO_EXCEPTION(queryResult.result);
  }

  VPackSlice result = queryResult.data->slice();
  TRI_ASSERT(result.isArray());

  Store store;
  index = 0;
  term = 0;
  if (result.length() == 1) {
    // Result can only have length 0 or 1.
    VPackSlice s = result[0];
    store.setNodeValue(s);
    index = extractIndexFromKey(s);
    term = s.get("term").getNumber<uint64_t>();
    LOG_TOPIC("d838b", INFO, Logger::AGENCY)
        << "Read snapshot at index " << index << " with term " << term;
  }

  // Now get the rest of the log entries, if there are any:
  aql = "FOR l IN log SORT l._key RETURN l";
  auto query2Future = arangodb::aql::runStandaloneAqlQuery(
      vocbase, origin, aql::QueryString(aql), nullptr);
  auto queryResult2 = std::move(queryFuture.waitAndGet());

  if (queryResult2.result.fail()) {
    THROW_ARANGO_EXCEPTION(queryResult2.result);
  }

  result = queryResult2.data->slice();

  if (result.isArray() && result.length() > 0) {
    VPackBuilder b;
    {
      VPackArrayBuilder bb(&b);
      for (auto const& ii : VPackArrayIterator(result)) {
        buffer_t tmp =
            std::make_shared<arangodb::velocypack::Buffer<uint8_t>>();

        auto req = ii.get("request");
        tmp->append(req.startAs<char const>(), req.byteSize());

        std::string clientId;
        if (auto clientSlice = req.get("clientId"); clientSlice.isString()) {
          clientId = clientSlice.copyString();
        }

        uint64_t epoch_millis = 0;
        if (auto milliSlice = req.get("epoch_millis"); milliSlice.isInteger()) {
          epoch_millis = milliSlice.getNumber<uint64_t>();
        }

        log_t entry(extractIndexFromKey(ii),
                    ii.get("term").getNumber<uint64_t>(), tmp, clientId,
                    epoch_millis);

        if (entry.index <= index) {
          LOG_TOPIC("c8f91", WARN, Logger::AGENCY)
              << "Found unnecessary log entry with index " << entry.index
              << " and term " << entry.term;
        } else {
          b.add(VPackSlice(entry.entry->data()));
          if (entry.index != index + 1) {
            LOG_TOPIC("ae564", WARN, Logger::AGENCY)
                << "Did not find log entries for indexes " << index + 1
                << " to " << entry.index - 1 << ", skipping...";
          }
          index = entry.index;  // they come in ascending order
          term = entry.term;
        }
      }
    }
    store.applyLogEntries(b, index, term);
  }

  auto builder = std::make_shared<VPackBuilder>();
  store.dumpToBuilder(*builder);
  return builder;
}

/// @brief load a compacted snapshot, returns the number of entries read.
uint64_t State::toVelocyPack(index_t lastIndex, VPackBuilder& builder) const {
  TRI_ASSERT(builder.isOpenObject());

  auto bindVars = std::make_shared<VPackBuilder>();
  bindVars->openObject();
  bindVars->add("key", VPackValue(stringify(lastIndex)));
  bindVars->close();

  std::string const logQueryStr(
      "FOR l IN log FILTER l._key <= @key SORT l._key RETURN l");

  TRI_ASSERT(nullptr !=
             _vocbase);  // this check was previously in the Query constructor
  auto origin = transaction::OperationOriginInternal{
      "serializing agency log to velocypack"};
  auto queryFuture = arangodb::aql::runStandaloneAqlQuery(
      *_vocbase, origin, aql::QueryString(logQueryStr), std::move(bindVars));
  auto logQueryResult = std::move(queryFuture.waitAndGet());

  if (logQueryResult.result.fail()) {
    THROW_ARANGO_EXCEPTION(logQueryResult.result);
  }

  VPackSlice result = logQueryResult.data->slice();
  TRI_ASSERT(result.isArray());

  std::string firstIndex;
  uint64_t n = 0;

  auto copyWithoutId = [&](VPackSlice slice, VPackBuilder& builder) {
    // Need to remove custom attribute in _id:
    {
      VPackObjectBuilder guard(&builder);
      for (auto const& p : VPackObjectIterator(slice)) {
        if (!p.key.isEqualString(StaticStrings::IdString)) {
          builder.add(p.key);
          builder.add(p.value);
        }
      }
    }
  };

  builder.add("log", VPackValue(VPackValueType::Array));
  try {
    for (VPackSlice ee : VPackArrayIterator(result)) {
      TRI_ASSERT(ee.isObject());
      copyWithoutId(ee, builder);
    }
    n = result.length();
    if (n > 0) {
      firstIndex = result[0].get(StaticStrings::KeyString).copyString();
    }
  } catch (...) {
    // ...
  }
  builder.close();  // "log"

  if (n > 0) {
    auto bindVars = std::make_shared<VPackBuilder>();
    bindVars->openObject();
    bindVars->add("key", VPackValue(firstIndex));
    bindVars->close();

    std::string const compQueryStr(
        "FOR c in compact FILTER c._key >= @key SORT c._key LIMIT 1 RETURN c");

    auto origin = transaction::OperationOriginInternal{
        "serializing agency snapshot to velocypack"};

    auto compQueryFuture = arangodb::aql::runStandaloneAqlQuery(
        *_vocbase, origin, aql::QueryString(compQueryStr), std::move(bindVars));
    auto compQueryResult = std::move(compQueryFuture.waitAndGet());

    if (compQueryResult.result.fail()) {
      THROW_ARANGO_EXCEPTION(compQueryResult.result);
    }

    result = compQueryResult.data->slice();
    TRI_ASSERT(result.isArray());

    if (result.length() > 0) {
      builder.add(VPackValue("compaction"));
      try {
        VPackSlice c = result[0];
        TRI_ASSERT(c.isObject());
        copyWithoutId(c, builder);
      } catch (...) {
        VPackObjectBuilder a(&builder);
      }
    }
  }

  return n;
}

/// @brief dump the entire in-memory state to velocypack.
/// should be used for testing only
void State::toVelocyPack(velocypack::Builder& builder) const {
  std::lock_guard mutexLocker{_logLock};

  builder.openObject();
  builder.add("current", VPackValue(_cur));
  builder.add("log", VPackValue(VPackValueType::Array));
  for (auto const& it : _log) {
    it.toVelocyPack(builder);
  }
  builder.close();  // log
  builder.close();
}

/*static*/ index_t State::extractIndexFromKey(
    arangodb::velocypack::Slice data) {
  TRI_ASSERT(data.isObject());
  VPackSlice keySlice = data.get(StaticStrings::KeyString);
  TRI_ASSERT(keySlice.isString());

  VPackValueLength keyLength;
  char const* key = keySlice.getString(keyLength);
  return index_t(arangodb::basics::StringUtils::uint64(key, keyLength));
}
