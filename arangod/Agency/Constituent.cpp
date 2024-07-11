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

#include "Constituent.h"

#include "Agency/Agent.h"
#include "ApplicationFeatures/ApplicationServer.h"
#include "Aql/QueryMethods.h"
#include "Aql/QueryResult.h"
#include "Aql/QueryString.h"
#include "Basics/application-exit.h"
#include "Basics/system-functions.h"
#include "Logger/LogMacros.h"
#include "Metrics/GaugeBuilder.h"
#include "Metrics/MetricsFeature.h"
#include "Network/Methods.h"
#include "Network/NetworkFeature.h"
#include "Random/RandomGenerator.h"
#include "Transaction/OperationOrigin.h"
#include "Transaction/StandaloneContext.h"
#include "Utils/OperationOptions.h"
#include "Utils/OperationResult.h"
#include "Utils/SingleCollectionTransaction.h"
#include "VocBase/ticks.h"
#include "VocBase/vocbase.h"

#include <velocypack/Iterator.h>

#include <chrono>
#include <iomanip>
#include <thread>

using namespace arangodb::consensus;
using namespace arangodb::rest;
using namespace arangodb::velocypack;
using namespace arangodb;

DECLARE_GAUGE(arangodb_agency_term, uint64_t, "Agency's term");

/// Raft role names for display purposes
const std::vector<std::string> roleStr({"Follower", "Candidate", "Leader"});

/// Configure with agent's configuration
void Constituent::configure(Agent* agent) {
  std::lock_guard guard{_termVoteLock};

  _agent = agent;
  TRI_ASSERT(_agent != nullptr);
}

// Default ctor
Constituent::Constituent(ArangodServer& server)
    : Thread(server, "Constituent"),
      _vocbase(nullptr),
      _term(0),
      _gterm(server.getFeature<metrics::MetricsFeature>().add(
          arangodb_agency_term{})),
      _leaderID(NO_LEADER),
      _lastHeartbeatSeen(0.0),
      _role(FOLLOWER),
      _agent(nullptr),
      _votedFor(NO_LEADER) {}

/// Shutdown if not already
Constituent::~Constituent() { shutdown(); }

/// Wait for sync
bool Constituent::waitForSync() const { return _agent->config().waitForSync(); }

/// Get my term
term_t Constituent::term() const {
  std::lock_guard guard{_termVoteLock};
  return _term;
}

/// Update my term
void Constituent::term(term_t t, std::string const& votedFor) {
  std::lock_guard guard{_termVoteLock};
  termNoLock(t, votedFor);
}

void Constituent::termNoLock(term_t t, std::string const& votedFor) {
  // Only call this when you have the _termVoteLock

  term_t tmp = _term;
  _term = t;
  _gterm = t;
  std::string tmpVotedFor = _votedFor;
  _votedFor = votedFor;

  if (tmp != t || tmpVotedFor != votedFor) {
    LOG_TOPIC("53541", INFO, Logger::AGENCY)
        << _id << ": changing term or votedFor, "
        << "current role: " << roleStr[_role] << " term " << t
        << " votedFor: " << votedFor;

    Builder body;
    {
      VPackObjectBuilder b(&body);
      std::ostringstream i_str;
      i_str << std::setw(20) << std::setfill('0') << t;
      body.add("_key", Value(i_str.str()));
      body.add("term", Value(t));
      body.add("voted_for", Value(_votedFor));
    }

    TRI_ASSERT(_vocbase != nullptr);
    auto origin =
        transaction::OperationOriginInternal{"storing agency election result"};
    auto ctx = transaction::StandaloneContext::create(*_vocbase, origin);
    SingleCollectionTransaction trx(std::move(ctx), "election",
                                    AccessMode::Type::WRITE);
    Result res = trx.begin();

    if (!res.ok()) {
      THROW_ARANGO_EXCEPTION(res);
    }

    OperationOptions options;

    options.waitForSync = _agent->config().waitForSync();
    options.silent = true;
    try {
      OperationResult result =
          (tmp != t) ? trx.insert("election", body.slice(), options)
                     : trx.replace("election", body.slice(), options);
      res = trx.finish(result.result);
    } catch (std::exception const& e) {
      LOG_TOPIC("334ae", FATAL, Logger::AGENCY)
          << "Failed to " << ((tmp != t) ? "insert" : "replace")
          << " RAFT election ballot: " << e.what() << ". Bailing out.";
      FATAL_ERROR_EXIT();
    }
  }
}

bool Constituent::logUpToDate(arangodb::consensus::index_t prevLogIndex,
                              term_t prevLogTerm) const {
  log_t myLastLogEntry = _agent->state().lastLog();
  return (prevLogTerm > myLastLogEntry.term ||
          (prevLogTerm == myLastLogEntry.term &&
           prevLogIndex >= myLastLogEntry.index));
}

bool Constituent::logMatches(arangodb::consensus::index_t prevLogIndex,
                             term_t prevLogTerm) const {
  int res = _agent->state().checkLog(prevLogIndex, prevLogTerm);
  if (res == 1) {
    return true;
  } else if (res == -1) {
    return false;
  } else {
    return true;  // This is important: If we have compacted away this log
                  // entry, then we know that this or a later entry was
                  // already committed by a majority and is therefore
                  // set in stone. Therefore the check must return true
                  // here and this is correct behavior.
                  // The other case in which we do not have the log entry
                  // is if it is so new that we have never heard about it
                  // in this case we can safely return true here as well,
                  // since we will replace our own log anyway in the very
                  // near future.
  }
}

/// My role
role_t Constituent::role() const {
  return _role.load(std::memory_order_relaxed);
}

/// Become follower in term
void Constituent::follow(term_t t, std::string const& votedFor) {
  std::lock_guard guard{_termVoteLock};
  followNoLock(t, votedFor);
}

void Constituent::followNoLock(term_t t, std::string const& votedFor) {
  if (t > 0 && (t != _term || votedFor != _votedFor)) {
    LOG_TOPIC("2fb5f", DEBUG, Logger::AGENCY)
        << "Changing term from " << _term << " to " << t;
    termNoLock(t, votedFor);
  }
  _role = FOLLOWER;

  LOG_TOPIC("f370f", INFO, Logger::AGENCY)
      << "Set _role to FOLLOWER in term " << _term;

  if (_leaderID == _id) {
    _leaderID = NO_LEADER;
    LOG_TOPIC("dfb7a", DEBUG, Logger::AGENCY)
        << "Setting _leaderID to NO_LEADER.";
  } else {
    LOG_TOPIC("d343a", INFO, Logger::AGENCY)
        << _id << ": following '" << _leaderID << "' in term " << _term;
  }

  std::lock_guard guard{_cv.mutex};
  _cv.cv.notify_one();
}

/// Become leader
void Constituent::lead(term_t term) {
  {
    std::lock_guard guard{_termVoteLock};

    // if we already have a higher term, ignore this request
    if (term < _term) {
      followNoLock(0);  // leave _term and _votedFor unchanged
      return;
    }

    // if we already lead, ignore this request
    if (_role.load(std::memory_order_relaxed) == LEADER) {
      TRI_ASSERT(_leaderID == _id);
      return;
    }

    // I'm the leader
    _role = LEADER;

    LOG_TOPIC("0d93b", INFO, Logger::AGENCY)
        << _id << ": leading in term " << _term;
    _leaderID = _id;

    // Keep track of this election time:
    std::lock_guard locker{_recentElectionsMutex};
    _recentElections.push_back(steadyClockToDouble());

    // we need to rebuild spear_head and read_db, but this is done in the
    // main Agent thread:
    _agent->beginPrepareLeadership();
  }

  _agent->wakeupMainLoop();
}

/// Become candidate
void Constituent::candidateNoLock() {
  // Only call this when you have the _termVoteLock
  if (_leaderID != NO_LEADER) {
    _leaderID = NO_LEADER;
    LOG_TOPIC("43711", DEBUG, Logger::AGENCY)
        << "Set _leaderID to NO_LEADER in Constituent::candidate";
  }

  if (_role.load(std::memory_order_relaxed) != CANDIDATE) {
    _role = CANDIDATE;
    LOG_TOPIC("aefab", INFO, Logger::AGENCY)
        << _id << ": candidating in term " << _term;

    // Keep track of this election time:
    std::lock_guard locker{_recentElectionsMutex};
    _recentElections.push_back(steadyClockToDouble());
  }
}

/// Leading?
bool Constituent::leading() const {
  return _role.load(std::memory_order_relaxed) == LEADER;
}

/// Following?
bool Constituent::following() const {
  return _role.load(std::memory_order_relaxed) == FOLLOWER;
}

/// Running as candidate?
bool Constituent::running() const {
  return _role.load(std::memory_order_relaxed) == CANDIDATE;
}

/// Get current leader's id
std::string Constituent::leaderID() const {
  std::lock_guard guard{_termVoteLock};
  return _leaderID;
}

/// Agency size
size_t Constituent::size() const { return _agent->config().size(); }

/// Get endpoint to an id
std::string Constituent::endpoint(std::string id) const {
  return _agent->config().poolAt(id);
}

/// @brief Check leader
bool Constituent::checkLeader(term_t term, std::string const& id,
                              index_t prevLogIndex, term_t prevLogTerm) {
  TRI_ASSERT(_vocbase != nullptr);

  std::lock_guard guard{_termVoteLock};

  LOG_TOPIC("0c694", TRACE, Logger::AGENCY)
      << "checkLeader(term: " << term << ", leaderId: " << id
      << ", prev-log-index: " << prevLogIndex
      << ", prev-log-term: " << prevLogTerm << ") in term " << _term;

  if (term < _term) {
    return false;
  }

  _lastHeartbeatSeen = TRI_microtime();
  LOG_TOPIC("d893b", TRACE, Logger::AGENCY)
      << "setting last heartbeat: " << _lastHeartbeatSeen;

  if (term > _term) {
    _agent->endPrepareLeadership();
    followNoLock(term, NO_LEADER);
  }

  if ((prevLogIndex != 0 || prevLogTerm != 0) &&
      !logMatches(prevLogIndex, prevLogTerm)) {
    return false;
  }

  if (_leaderID != id) {
    LOG_TOPIC("ec7b8", DEBUG, Logger::AGENCY)
        << "Set _leaderID to '" << id << "' in term " << _term;
    _leaderID = id;

    // Recall time of this leadership change:
    {
      std::lock_guard locker{_recentElectionsMutex};
      _recentElections.push_back(steadyClockToDouble());
    }

    TRI_ASSERT(_leaderID != _id);
    if (_role.load(std::memory_order_relaxed) != FOLLOWER) {
      followNoLock(0);  // do not adjust _term or _votedFor
    }
  }

  return true;
}

/// @brief Vote
bool Constituent::vote(term_t termOfPeer, std::string const& id,
                       index_t prevLogIndex, term_t prevLogTerm) {
  if (!_agent->ready()) {
    return false;
  }

  TRI_ASSERT(_vocbase != nullptr);

  std::lock_guard guard{_termVoteLock};

  LOG_TOPIC("a5185", TRACE, Logger::AGENCY)
      << "vote(termOfPeer: " << termOfPeer << ", leaderId: " << id
      << ", prev-log-index: " << prevLogIndex
      << ", prev-log-term: " << prevLogTerm << ") in (my) term " << _term;

  if (termOfPeer > _term) {
    if (_role.load(std::memory_order_relaxed) != FOLLOWER) {
      followNoLock(termOfPeer, NO_LEADER);
    } else {
      termNoLock(termOfPeer, NO_LEADER);
    }
    _agent->endPrepareLeadership();
  } else if (termOfPeer < _term) {
    // termOfPeer < _term, simply ignore and do not vote:
    LOG_TOPIC("9a5dd", DEBUG, Logger::AGENCY)
        << "ignoring RequestVoteRPC with old term " << termOfPeer
        << ", we are already at term " << _term;
    return false;
  }

  if (_votedFor != NO_LEADER) {  // already voted in this term
    if (_votedFor == id) {
      LOG_TOPIC("41c49", DEBUG, Logger::AGENCY) << "repeating vote for " << id;
      // Set the last heart beat seen to now, to grant the other guy some time
      // to establish itself as a leader, before we call for another election:
      _lastHeartbeatSeen = TRI_microtime();
      LOG_TOPIC("658ba", TRACE, Logger::AGENCY)
          << "setting last heartbeat time to now, since we repeated a vote "
             "grant: "
          << _lastHeartbeatSeen;
      return true;
    }
    LOG_TOPIC("df508", DEBUG, Logger::AGENCY)
        << "not voting for " << id << " since we have already voted for "
        << _votedFor << " in this term";
    return false;
  }

  // Now decide whether or not we vote for this server, we have to
  // take into account paragraph 5.4.1 in the Raft paper, so we need
  // to check that his log is at least as up to date as ours:
  log_t myLastLogEntry = _agent->state().lastLog();
  if (prevLogTerm > myLastLogEntry.term ||
      (prevLogTerm == myLastLogEntry.term &&
       prevLogIndex >= myLastLogEntry.index)) {
    LOG_TOPIC("8d8da", DEBUG, Logger::AGENCY)
        << "voting for " << id << " in term " << _term;
    // Set the last heart beat seen to now, to grant the other guy some time
    // to establish itself as a leader, before we call for another election:
    _lastHeartbeatSeen = TRI_microtime();
    LOG_TOPIC("ffaac", TRACE, Logger::AGENCY)
        << "setting last heartbeat time to now, since we granted a vote: "
        << _lastHeartbeatSeen;
    termNoLock(_term, id);
    return true;
  }
  LOG_TOPIC("4ca50", DEBUG, Logger::AGENCY)
      << "not voting for " << id << " since his log is not up to date: "
      << "my last log entry: (" << myLastLogEntry.term << ", "
      << myLastLogEntry.index << "), his last log entry: (" << prevLogTerm
      << ", " << prevLogIndex << ")";
  return false;  // do not vote for this uninformed guy!
}

/// @brief Call to election
void Constituent::callElection() {
  using namespace std::chrono;

  // simon: whats the minimum timeout here ?
  network::Timeout timeout(0.9 * _agent->config().minPing() *
                           _agent->config().timeoutMult());
  auto endTime =
      steady_clock::now() + duration<double>(_agent->config().minPing() *
                                             _agent->config().timeoutMult());

  std::vector<std::string> active = _agent->config().active();

  term_t savedTerm;
  {
    std::lock_guard locker{_termVoteLock};

    this->termNoLock(_term + 1, _id);  // raise my term, vote for us

    _agent->endPrepareLeadership();
    savedTerm = _term;
    LOG_TOPIC("67c5f", DEBUG, Logger::AGENCY) << "Set _leaderID to NO_LEADER"
                                              << " in term " << _term;
    _leaderID = NO_LEADER;
  }

  int64_t electionEventCount = countRecentElectionEvents(3600);
  if (electionEventCount <= 0) {
    electionEventCount = 1;
  } else if (electionEventCount > 10) {
    electionEventCount = 10;
  }
  if (_agent->getTimeoutMult() != electionEventCount) {
    _agent->adjustTimeoutMult(electionEventCount);
    LOG_TOPIC("f4568", WARN, Logger::AGENCY)
        << "Candidate: setting timeout multiplier "
           "to "
        << electionEventCount << " for next term...";
  }

  auto const& nf = _agent->server().getFeature<arangodb::NetworkFeature>();
  network::ConnectionPool* cp = nf.pool();

  network::RequestOptions reqOpts;
  // never compress requests to the agency, so that we do not spend too much
  // CPU on compression/decompression. some agent instances run with a very
  // low number of cores (even fractions of physical cores), so we cannot
  // waste too much CPU resources there.
  reqOpts.allowCompression = false;
  reqOpts.timeout = timeout;
  reqOpts.param("term", std::to_string(savedTerm))
      .param("candidateId", _id)
      .param("prevLogIndex", std::to_string(_agent->lastLog().index))
      .param("prevLogTerm", std::to_string(_agent->lastLog().term))
      .param("timeoutMult", std::to_string(electionEventCount));

  // Ask everyone for their vote
  // Collect ballots. I vote for myself.
  auto yea = std::make_shared<std::atomic<size_t>>(1);
  auto nay = std::make_shared<std::atomic<size_t>>(0);
  auto maxTermReceived = std::make_shared<std::atomic<term_t>>(savedTerm);
  const size_t majority = size() / 2 + 1;

  for (auto const& i : active) {
    if (i == _id) {
      continue;
    }
    std::ignore =
        network::sendRequest(
            cp, _agent->config().poolAt(i), fuerte::RestVerb::Get,
            "/_api/agency_priv/requestVote", VPackBuffer<uint8_t>(), reqOpts)
            .thenValue([=](network::Response r) {
              if (r.ok() && r.statusCode() == 200) {
                VPackSlice slc = r.slice();

                // Got ballot
                if (slc.isObject() && slc.hasKey("term") &&
                    slc.hasKey("voteGranted")) {
                  // Follow right away?
                  term_t receivedT = slc.get("term").getUInt();
                  if (receivedT > savedTerm) {  // only count vote if term is
                                                // equal or smaller
                    term_t expectedT = maxTermReceived->load();
                    maxTermReceived->compare_exchange_strong(expectedT,
                                                             receivedT);
                  } else {
                    // Check result and counts
                    if (slc.get("voteGranted")
                            .getBool()) {  // majority in favor?
                      yea->fetch_add(1);
                      // Vote is counted as yea
                      return;
                    }
                  }
                }
              }
              // Count the vote as a nay
              nay->fetch_add(1);
            });
  }

  // We collect votes, we leave the following loop when one of the following
  // conditions is met:
  //   (1) A majority of nay votes have been received
  //   (2) A majority of yea votes (including ourselves) have been received
  //   (3) At least yyy time has passed, in this case we give up without
  //       a conclusive vote.
  while (!isStopping()) {
    if (steady_clock::now() >= endTime) {  // Timeout.
      std::lock_guard locker{_termVoteLock};
      followNoLock(0);  // do not adjust _term or _votedFor
      break;
    }

    term_t t = maxTermReceived->load();
    {
      std::lock_guard locker{_termVoteLock};
      if (t > _term) {
        followNoLock(t, NO_LEADER);
        break;
      }
    }

    if (yea->load() >= majority) {
      lead(savedTerm);
      break;
    }

    if (nay->load() > size() - majority) {  // Network: majority against?
      follow(0);                            // do not adjust _term or _votedFor
      break;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  LOG_TOPIC("b5061", DEBUG, Logger::AGENCY)
      << "Election: Have received " << yea->load() << " yeas and "
      << nay->load() << " nays, the "
      << (yea->load() >= majority ? "yeas" : "nays") << " have it.";
}

void Constituent::update(std::string const& leaderID, term_t t) {
  std::lock_guard guard{_termVoteLock};
  _term = t;
  _gterm = t;
  if (_leaderID != leaderID) {
    LOG_TOPIC("fe299", INFO, Logger::AGENCY)
        << "Constituent::update: setting _leaderID to '" << leaderID
        << "' in term " << _term;
    _leaderID = leaderID;
    _role = FOLLOWER;
  }
}

/// Start clean shutdown
void Constituent::beginShutdown() {
  Thread::beginShutdown();

  std::lock_guard guard{_cv.mutex};
  _cv.cv.notify_all();
}

/// Start operation
bool Constituent::start(TRI_vocbase_t* vocbase) {
  TRI_ASSERT(vocbase != nullptr);
  _vocbase = vocbase;

  return Thread::start();
}

/// Get persisted information and run election process
void Constituent::run() {
  // single instance
  _id = _agent->config().id();

  TRI_ASSERT(_vocbase != nullptr);

  // Most recent vote
  {
    std::string const aql(
        "FOR l IN election SORT l._key DESC LIMIT 1 RETURN l");
    auto origin = transaction::OperationOriginInternal{
        "querying most recent agency election vote"};

    auto queryFuture = arangodb::aql::runStandaloneAqlQuery(
        *_vocbase, origin, aql::QueryString(aql), nullptr);
    auto queryResult = std::move(queryFuture.waitAndGet());

    if (queryResult.result.fail()) {
      THROW_ARANGO_EXCEPTION(queryResult.result);
    }

    VPackSlice result = queryResult.data->slice();

    if (result.isArray()) {
      for (auto const& i : VPackArrayIterator(result)) {
        auto ii = i.resolveExternals();
        try {
          std::lock_guard locker{_termVoteLock};
          termNoLock(ii.get("term").getUInt(),
                     ii.get("voted_for").copyString());
        } catch (std::exception const&) {
          LOG_TOPIC("404be", ERR, Logger::AGENCY)
              << "Persisted election entries corrupt! Defaulting term,vote "
                 "(0,0)";
        }
      }
    }
  }

  std::vector<std::string> act = _agent->config().active();

  while (!this->isStopping()  // Obvious
         && (!_agent->ready() ||
             find(act.begin(), act.end(), _id) == act.end())) {  // Active agent
    std::unique_lock guardv{_cv.mutex};
    _cv.cv.wait_for(guardv, std::chrono::milliseconds{50});
    act = _agent->config().active();
  }

  {
    std::lock_guard guard{_termVoteLock};
    LOG_TOPIC("29175", INFO, Logger::AGENCY)
        << "Setting role to follower in term " << _term;
    _role = FOLLOWER;
  }

  std::chrono::steady_clock::time_point constituentLoopStart;

  while (!this->isStopping()) {
    constituentLoopStart = std::chrono::steady_clock::now();

    role_t role = _role.load(std::memory_order_relaxed);

    if (role == FOLLOWER) {
      static double const M = 1.0e6;
      int64_t a = static_cast<int64_t>(M * _agent->config().minPing() *
                                       _agent->config().timeoutMult());
      int64_t b = static_cast<int64_t>(M * _agent->config().maxPing() *
                                       _agent->config().timeoutMult());
      int64_t randTimeout = RandomGenerator::interval(a, b);
      int64_t randWait = randTimeout;

      // Speedup for size() == 1 - do not wait at all. Start election
      // immediately.
      if (size() == 1) {
        LOG_TOPIC("31ed7", INFO, Logger::AGENCY)
            << "Immediately start election because of single agency";
        randWait = 0;
      }

      {
        std::lock_guard guard{_termVoteLock};

        // in the beginning, pure random, after that, we might have to
        // wait for less than planned, since the last heartbeat we have
        // seen is already some time ago, note that this waiting time
        // can become negative:
        if (_lastHeartbeatSeen > 0.0) {
          double now = TRI_microtime();
          randWait -= static_cast<int64_t>(M * (now - _lastHeartbeatSeen));
          if (randWait < a) {
            randWait = a;
          } else if (randWait > b) {
            randWait = b;
          }
        }
      }

      LOG_TOPIC("9a750", TRACE, Logger::AGENCY)
          << "Random timeout: " << randTimeout << ", wait: " << randWait;

      if (randWait > 0) {
        std::unique_lock guardv{_cv.mutex};
        _cv.cv.wait_for(guardv, std::chrono::microseconds{randWait});
      }

      {
        std::lock_guard guard{_termVoteLock};
        bool isTimeout = false;

        if (_lastHeartbeatSeen <= 0.0) {
          LOG_TOPIC("456d2", TRACE, Logger::AGENCY) << "no heartbeat seen";
          isTimeout = true;
        } else {
          double diff = TRI_microtime() - _lastHeartbeatSeen;
          LOG_TOPIC("f475b", TRACE, Logger::AGENCY)
              << "last heartbeat: " << diff << "sec ago";

          isTimeout = (static_cast<int64_t>(M * diff) > randTimeout);
        }

        if (isTimeout) {
          LOG_TOPIC("18b71", TRACE, Logger::AGENCY)
              << "timeout, calling an election";
          candidateNoLock();
          _agent->endPrepareLeadership();
        }
      }
    } else if (role == CANDIDATE) {
      callElection();  // Run for office
      // Now we take this point of time as the next base point for a
      // potential next random timeout, since we have just cast a vote for
      // ourselves:
      _lastHeartbeatSeen = TRI_microtime();
      LOG_TOPIC("aeaef", TRACE, Logger::AGENCY)
          << "setting last heartbeat because we voted for us: "
          << _lastHeartbeatSeen;

    } else {
      double interval =
          0.25 * _agent->config().minPing() * _agent->config().timeoutMult();

      double now = TRI_microtime();
      double nextWakeup = interval;  // might be lowered below

      std::string const myid = _agent->id();
      for (auto const& followerId : _agent->config().active()) {
        if (followerId != myid) {
          bool needed = false;
          {
            std::lock_guard guard{_heartBeatMutex};
            auto it = _lastHeartbeatSent.find(followerId);
            if (it == _lastHeartbeatSent.end()) {
              needed = true;
            } else {
              double diff = now - it->second;
              if (diff >= interval) {
                needed = true;
                nextWakeup = 0;
              } else {
                // diff < interval, so only needed again in interval-diff s
                double waitOnly = interval - diff;
                if (nextWakeup > waitOnly) {
                  nextWakeup = waitOnly;
                }
                LOG_TOPIC("25d6b", DEBUG, Logger::AGENCY)
                    << "No need for empty AppendEntriesRPC: " << diff;
              }
            }
          }
          LOG_TOPIC("ddeea", DEBUG, Logger::AGENCY)
              << "Considering empty AppendEntriesRPC for follower "
              << followerId << " needed: " << needed;
          if (needed) {
            auto startTime = std::chrono::steady_clock::now();
            _agent->sendEmptyAppendEntriesRPC(followerId);
            auto endTime = std::chrono::steady_clock::now();
            if (endTime - startTime > std::chrono::milliseconds(100)) {
              LOG_TOPIC("ddeeb", DEBUG, Logger::AGENCY)
                  << "Call to sendEmptyAppendEntriesRPC took longer than 0.1 "
                     "s: time needed: "
                  << std::chrono::duration<double>(endTime - startTime).count();
            }
          }
        }
      }

      auto beforeWaitTime = std::chrono::steady_clock::now();

      // This is the smallest time until any of the followers need a
      // new empty heartbeat:
      uint64_t timeout = static_cast<uint64_t>(1000000.0 * nextWakeup);
      if (timeout > 0) {
        std::unique_lock guardv{_cv.mutex};
        _cv.cv.wait_for(guardv, std::chrono::microseconds{timeout});
      }
      auto afterWaitTime = std::chrono::steady_clock::now();
      if (afterWaitTime - constituentLoopStart >
          std::chrono::seconds(1) * _agent->config().minPing() *
              _agent->config().timeoutMult()) {
        LOG_TOPIC("aa123", WARN, Logger::AGENCY)
            << "Constituent loop delayed: First part took "
            << (beforeWaitTime - constituentLoopStart).count()
            << ", second part took " << (afterWaitTime - beforeWaitTime).count()
            << ", which is together more than minPing="
            << _agent->config().minPing() * _agent->config().timeoutMult();
      }
    }
  }
}

int64_t Constituent::countRecentElectionEvents(double threshold) {
  // This discards all election events that are older than `threshold`
  // seconds and returns the number of more recent ones.

  auto now = steadyClockToDouble();
  std::lock_guard locker{_recentElectionsMutex};
  int64_t count = 0;
  for (auto iter = _recentElections.begin(); iter != _recentElections.end();) {
    if (now - *iter > threshold) {
      // If event is more then 15 minutes ago, discard
      iter = _recentElections.erase(iter);  // moves to the next one in list
    } else {
      ++count;
      ++iter;
    }
  }
  return count;
}

// Notify about heartbeat being sent out:
void Constituent::notifyHeartbeatSent(std::string const& followerId) {
  double now = TRI_microtime();
  std::lock_guard guard{_heartBeatMutex};
  _lastHeartbeatSent[followerId] = now;
}
