////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2020 ArangoDB GmbH, Cologne, Germany
/// Copyright 2004-2014 triAGENS GmbH, Cologne, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
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

#include "Manager.h"

#include "ApplicationFeatures/ApplicationServer.h"
#include "Aql/Query.h"
#include "Aql/QueryList.h"
#include "Basics/ReadLocker.h"
#include "Basics/ScopeGuard.h"
#include "Basics/WriteLocker.h"
#include "Basics/system-functions.h"
#include "Cluster/ClusterFeature.h"
#include "Cluster/ClusterInfo.h"
#include "Cluster/ServerState.h"
#include "Futures/Utilities.h"
#include "GeneralServer/AuthenticationFeature.h"
#include "Logger/LogMacros.h"
#include "Network/Methods.h"
#include "Network/NetworkFeature.h"
#include "Network/Utils.h"
#include "RestServer/DatabaseFeature.h"
#include "StorageEngine/EngineSelectorFeature.h"
#include "StorageEngine/StorageEngine.h"
#include "StorageEngine/TransactionState.h"
#include "Transaction/Helpers.h"
#include "Transaction/Methods.h"
#include "Transaction/SmartContext.h"
#include "Transaction/Status.h"
#include "Utils/CollectionNameResolver.h"
#include "Utils/ExecContext.h"

#ifdef USE_ENTERPRISE
#include "Enterprise/VocBase/VirtualCollection.h"
#endif

#include <fuerte/jwt.h>
#include <velocypack/Iterator.h>
#include <velocypack/velocypack-aliases.h>

#include <thread>

namespace {
bool authorized(std::string const& user) {
  auto const& exec = arangodb::ExecContext::current();
  if (exec.isSuperuser()) {
    return true;
  }
  return (user == exec.user());
}

std::string currentUser() { return arangodb::ExecContext::current().user(); }
}  // namespace

namespace arangodb {
namespace transaction {

size_t constexpr Manager::maxTransactionSize;

namespace {
struct MGMethods final : arangodb::transaction::Methods {
  MGMethods(std::shared_ptr<arangodb::transaction::Context> const& ctx,
            arangodb::transaction::Options const& opts)
      : Methods(ctx, opts) {}
};
}  // namespace

Manager::Manager(ManagerFeature& feature)
  : _feature(feature),
    _nrRunning(0),
    _nrReadLocked(0),
    _disallowInserts(false),
    _writeLockHeld(false),
    _streamingLockTimeout(feature.streamingLockTimeout()) {}

void Manager::registerTransaction(TransactionId transactionId,
                                  bool isReadOnlyTransaction,
                                  bool isFollowerTransaction) {
  // If isFollowerTransaction is set then either the transactionId should be
  // an isFollowerTransactionId or it should be a legacy transactionId:
  TRI_ASSERT(!isFollowerTransaction ||
             transactionId.isFollowerTransactionId() ||
             transactionId.isLegacyTransactionId());
  if (!isReadOnlyTransaction && !isFollowerTransaction) {
    LOG_TOPIC("ccdea", TRACE, Logger::TRANSACTIONS) << "Acquiring read lock for tid " << transactionId.id();
    _rwLock.lockRead();
    _nrReadLocked.fetch_add(1, std::memory_order_relaxed);
    LOG_TOPIC("ccdeb", TRACE, Logger::TRANSACTIONS) << "Got read lock for tid " << transactionId.id() << " nrReadLocked: " << _nrReadLocked.load(std::memory_order_relaxed);
  }

  _nrRunning.fetch_add(1, std::memory_order_relaxed);
}

// unregisters a transaction
void Manager::unregisterTransaction(TransactionId transactionId,
                                    bool isReadOnlyTransaction,
                                    bool isFollowerTransaction) {
  // always perform an unlock when we leave this function
  auto guard = scopeGuard([this, transactionId, &isReadOnlyTransaction, &isFollowerTransaction]() {
    if (!isReadOnlyTransaction && !isFollowerTransaction) {
      _rwLock.unlockRead();
      _nrReadLocked.fetch_sub(1, std::memory_order_relaxed);
      LOG_TOPIC("ccded", TRACE, Logger::TRANSACTIONS) << "Released lock for tid " << transactionId.id() << " nrReadLocked: " << _nrReadLocked.load(std::memory_order_relaxed);
    }
  });

  uint64_t r = _nrRunning.fetch_sub(1, std::memory_order_relaxed);
  TRI_ASSERT(r > 0);
}

uint64_t Manager::getActiveTransactionCount() {
  return _nrRunning.load(std::memory_order_relaxed);
}

/*static*/ double Manager::ttlForType(Manager::MetaType type) {
  if (type == Manager::MetaType::Tombstone) {
    return tombstoneTTL;
  }

  auto role = ServerState::instance()->getRole();
  if ((ServerState::isSingleServer(role) || ServerState::isCoordinator(role))) {
    return  idleTTL;
  }
  return idleTTLDBServer;
}

Manager::ManagedTrx::ManagedTrx(MetaType t, double ttl,
                                std::shared_ptr<TransactionState> st)
    : type(t),
      intermediateCommits(false),
      finalStatus(Status::UNDEFINED),
      timeToLive(ttl),
      expiryTime(TRI_microtime() + Manager::ttlForType(t)),
      state(std::move(st)),
      user(::currentUser()),
      rwlock() {}

bool Manager::ManagedTrx::hasPerformedIntermediateCommits() const {
  return this->intermediateCommits;
}

bool Manager::ManagedTrx::expired() const {
  return this->expiryTime < TRI_microtime();
}

void Manager::ManagedTrx::updateExpiry() {
  this->expiryTime = TRI_microtime() + this->timeToLive;
}

Manager::ManagedTrx::~ManagedTrx() {
  if (type == MetaType::StandaloneAQL || state == nullptr) {
    return;  // not managed by us
  }
  if (!state->isRunning()) {
    return;
  }

  try {
    transaction::Options opts;
    auto ctx =
        std::make_shared<transaction::ManagedContext>(TransactionId{2}, state, /*responsibleForCommit*/true);
    MGMethods trx(ctx, opts);  // own state now
    TRI_ASSERT(trx.state()->status() == transaction::Status::RUNNING);
    TRI_ASSERT(trx.isMainTransaction());
    trx.abort();
  } catch (...) {
    // obviously it is not good to consume all exceptions here,
    // but we are in a destructor and must never throw from here
  }
}

using namespace arangodb;

/// @brief register a transaction shard
/// @brief tid global transaction shard
/// @param cid the optional transaction ID (use 0 for a single shard trx)
void Manager::registerAQLTrx(std::shared_ptr<TransactionState> const& state) {
  if (_disallowInserts.load(std::memory_order_acquire)) {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_SHUTTING_DOWN);
  }

  TRI_ASSERT(state != nullptr);
  auto const tid = state->id();
  size_t const bucket = getBucket(tid);
  {
    WRITE_LOCKER(writeLocker, _transactions[bucket]._lock);

    auto& buck = _transactions[bucket];

    double ttl = Manager::ttlForType(MetaType::StandaloneAQL);
    auto it = buck._managed.try_emplace(tid, MetaType::StandaloneAQL, ttl, state);
    if (!it.second) {
      THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_TRANSACTION_INTERNAL,
                                     std::string("transaction ID ") + std::to_string(tid.id()) +
                                         "' already used (while registering AQL trx)");
    }
  }
}

void Manager::unregisterAQLTrx(TransactionId tid) noexcept {
  const size_t bucket = getBucket(tid);
  WRITE_LOCKER(writeLocker, _transactions[bucket]._lock);

  auto& buck = _transactions[bucket];
  auto it = buck._managed.find(tid);
  if (it == buck._managed.end()) {
    LOG_TOPIC("92a49", ERR, Logger::TRANSACTIONS)
        << "a registered transaction was not found";
    TRI_ASSERT(false);
    return;
  }
  TRI_ASSERT(it->second.type == MetaType::StandaloneAQL);

  /// we need to make sure no-one else is still using the TransactionState
  if (!it->second.rwlock.lockWrite(/*maxAttempts*/ 256)) {
    LOG_TOPIC("9f7d7", ERR, Logger::TRANSACTIONS)
        << "a transaction is still in use";
    TRI_ASSERT(false);
    return;
  }

  buck._managed.erase(it);  // unlocking not necessary
}

Result Manager::createManagedTrx(TRI_vocbase_t& vocbase, TransactionId tid,
                                 VPackSlice trxOpts) {
  Result res;
  if (_disallowInserts) {
    return res.reset(TRI_ERROR_SHUTTING_DOWN);
  }

  // parse the collections to register
  if (!trxOpts.isObject() || !trxOpts.get("collections").isObject()) {
    return res.reset(TRI_ERROR_BAD_PARAMETER, "missing 'collections'");
  }

  // extract the properties from the object
  transaction::Options options;
  options.fromVelocyPack(trxOpts);
  if (options.lockTimeout < 0.0) {
    return res.reset(TRI_ERROR_BAD_PARAMETER,
                     "<lockTimeout> needs to be positive");
  }

  auto fillColls = [](VPackSlice const& slice, std::vector<std::string>& cols) {
    if (slice.isNone()) {  // ignore nonexistent keys
      return true;
    } else if (slice.isString()) {
      cols.emplace_back(slice.copyString());
      return true;
    }

    if (slice.isArray()) {
      for (VPackSlice val : VPackArrayIterator(slice)) {
        if (!val.isString() || val.getStringLength() == 0) {
          return false;
        }
        cols.emplace_back(val.copyString());
      }
      return true;
    }
    return false;
  };
  std::vector<std::string> reads, writes, exclusives;
  VPackSlice collections = trxOpts.get("collections");
  bool isValid = fillColls(collections.get("read"), reads) &&
                 fillColls(collections.get("write"), writes) &&
                 fillColls(collections.get("exclusive"), exclusives);
  if (!isValid) {
    return res.reset(TRI_ERROR_BAD_PARAMETER,
                     "invalid 'collections' attribute");
  }

  return createManagedTrx(vocbase, tid, reads, writes, exclusives, std::move(options));
}

/// @brief create managed transaction
Result Manager::createManagedTrx(TRI_vocbase_t& vocbase, TransactionId tid,
                                 std::vector<std::string> const& readCollections,
                                 std::vector<std::string> const& writeCollections,
                                 std::vector<std::string> const& exclusiveCollections,
                                 transaction::Options options,
                                 double ttl) {
  Result res;
  if (_disallowInserts.load(std::memory_order_acquire)) {
    return res.reset(TRI_ERROR_SHUTTING_DOWN);
  }

  LOG_TOPIC("7bd2d", DEBUG, Logger::TRANSACTIONS) << "managed trx creating: " << tid.id();

  const size_t bucket = getBucket(tid);

  {  // quick check whether ID exists
    WRITE_LOCKER(writeLocker, _transactions[bucket]._lock);

    auto& buck = _transactions[bucket];
    auto it = buck._managed.find(tid);
    if (it != buck._managed.end()) {
      if (ServerState::instance()->isDBServer() && tid.isFollowerTransactionId()) {
        // it is ok for two different leaders to try to create the same
        // follower transaction on a leader.
        // for example, if we have 3 database servers and 2 shards, so that
        // - db server 1 is the leader for shard A
        // - db server 2 is the leader for shard B
        // - db server 3 is the follower for both shard A and B,
        // then db server 1 may try to lazily start a follower transaction
        // on db server 3, and db server 2 may try to do the same. It is 
        // important that both succeed, because otherwise one of the calls
        // would just drop db server 3 as a follower.
        TRI_ASSERT(res.ok());
        return res;
      }

      return res.reset(TRI_ERROR_TRANSACTION_INTERNAL,
                       std::string("transaction id ") + std::to_string(tid.id()) +
                       " already used, (before creating)");
    }
  }

  // enforce size limit per DBServer
  if (ServerState::instance()->isDBServer() && tid.isFollowerTransactionId()) {
    // if we are a follower, we reuse the leader's max transaction size and slightly
    // increase it. this is to ensure that the follower can process at least as many
    // data as the leader, even if the data representation is slightly varied for
    // network transport etc.
    if (options.maxTransactionSize != UINT64_MAX) {
      uint64_t adjust = options.maxTransactionSize / 10;
      if (adjust < UINT64_MAX - options.maxTransactionSize) {
        // now the transaction on the follower should be able to grow to at least the
        // size of the transaction on the leader.
        options.maxTransactionSize += adjust;
      }
    }
    // it is also important that we set this option, so that it is ok for two 
    // different leaders to add "their" shards to the same follower transaction.
    // for example, if we have 3 database servers and 2 shards, so that
    // - db server 1 is the leader for shard A
    // - db server 2 is the leader for shard B
    // - db server 3 is the follower for both shard A and B,
    // then db server 1 may try to lazily start a follower transaction
    // on db server 3 for shard A, and db server 2 may try to do the same for shard B.
    // Both calls will only send data for "their" shards, so effectively we need to 
    // add write collections to the transaction at runtime whenever this happens.
    // It is important that all these calls succeed, because otherwise one of the calls
    // would just drop db server 3 as a follower.
    options.allowImplicitCollectionsForWrite = true;
  } else {
    // for all other transactions, apply a size limitation 
    options.maxTransactionSize =
        std::min<size_t>(options.maxTransactionSize, Manager::maxTransactionSize);
  }
  
  std::shared_ptr<TransactionState> state;
  try {
    // now start our own transaction
    StorageEngine* engine = EngineSelectorFeature::ENGINE;
    state = engine->createTransactionState(vocbase, tid, options);
  } catch (basics::Exception const& e) {
    return res.reset(e.code(), e.message());
  }
  TRI_ASSERT(state != nullptr);
  TRI_ASSERT(state->id() == tid);
  
  // lock collections
  CollectionNameResolver resolver(vocbase);
  auto lockCols = [&](std::vector<std::string> const& cols, AccessMode::Type mode) {
    for (auto const& cname : cols) {
      DataSourceId cid = DataSourceId::none();
      if (state->isCoordinator()) {
        cid = resolver.getCollectionIdCluster(cname);
      } else {  // only support local collections / shards
        cid = resolver.getCollectionIdLocal(cname);
      }

      if (cid.empty()) {
        // not found
        res.reset(TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND,
                  std::string(TRI_errno_string(TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND)) +
                      ": " + cname);
      } else {
#ifdef USE_ENTERPRISE
        if (state->isCoordinator()) {
          try {
            std::shared_ptr<LogicalCollection> col = resolver.getCollection(cname);
            if (col->isSmart() && col->type() == TRI_COL_TYPE_EDGE) {
              auto theEdge = dynamic_cast<arangodb::VirtualSmartEdgeCollection*>(col.get());
              if (theEdge == nullptr) {
                THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL, "cannot cast collection to smart edge collection");
              }
              res.reset(state->addCollection(theEdge->getLocalCid(), "_local_" + cname, mode,  /*lockUsage*/false));
              if (res.fail()) {
                return false;
              }
              res.reset(state->addCollection(theEdge->getFromCid(), "_from_" + cname, mode, /*lockUsage*/false));
              if (res.fail()) {
                return false;
              }
              res.reset(state->addCollection(theEdge->getToCid(), "_to_" + cname, mode,  /*lockUsage*/  false));
              if (res.fail()) {
                return false;
              }
            }
          } catch (basics::Exception const& ex) {
            res.reset(ex.code(), ex.what());
            return false;
          }
        }
#endif
        res.reset(state->addCollection(cid, cname, mode, /*lockUsage*/ false));
      }

      if (res.fail()) {
        return false;
      }
    }
    return true;
  };
  if (!lockCols(exclusiveCollections, AccessMode::Type::EXCLUSIVE) ||
      !lockCols(writeCollections, AccessMode::Type::WRITE) ||
      !lockCols(readCollections, AccessMode::Type::READ)) {
    if (res.fail()) {
      // error already set by callback function
      return res;
    }
    // no error set. so it must be "data source not found"
    return res.reset(TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND);
  }
  
  // start the transaction
  transaction::Hints hints;
  hints.set(transaction::Hints::Hint::GLOBAL_MANAGED);
  if (ServerState::instance()->isDBServer() && tid.isFollowerTransactionId()) {
    // turn on intermediate commits on followers as well. otherwise huge leader
    // transactions could make the follower claim all memory and crash.
    hints.set(transaction::Hints::Hint::INTERMEDIATE_COMMITS);
  }
  try {
    res = state->beginTransaction(hints);  // registers with transaction manager
  } catch (basics::Exception const& ex) {
    res.reset(ex.code(), ex.what());
  }

  if (res.fail()) {
    TRI_ASSERT(!state->isRunning());
    return res;
  }
  
  if (ttl <= 0) {
    ttl = Manager::ttlForType(MetaType::Managed);
  }

  {  // add transaction to bucket
    WRITE_LOCKER(writeLocker, _transactions[bucket]._lock);

    TRI_ASSERT(state->id() == tid);
    auto it = _transactions[bucket]._managed.try_emplace(tid, MetaType::Managed, ttl, std::move(state));
    if (!it.second) {
      return res.reset(TRI_ERROR_TRANSACTION_INTERNAL,
                       std::string("transaction id ") + std::to_string(tid.id()) +
                           " already used, (while creating)");
    }
  }

  LOG_TOPIC("d6806", DEBUG, Logger::TRANSACTIONS) << "created managed trx " << tid;

  return res;
}

/// @brief lease the transaction, increases nesting
std::shared_ptr<transaction::Context> Manager::leaseManagedTrx(TransactionId tid,
                                                               AccessMode::Type mode) {
  TRI_ASSERT(mode != AccessMode::Type::NONE);
  if (_disallowInserts.load(std::memory_order_acquire)) {
    return nullptr;
  }
  
  auto const role = ServerState::instance()->getRole();
  std::chrono::steady_clock::time_point endTime;
  if (!ServerState::isDBServer(role)) { // keep end time as small as possible
    endTime = std::chrono::steady_clock::now() + std::chrono::milliseconds(int64_t(1000 * _streamingLockTimeout));
  }
  // always serialize access on coordinator,
  // TransactionState::_knownServers is modified even for READ
  if (ServerState::isCoordinator(role)) {
    mode = AccessMode::Type::WRITE;
  }

  size_t const bucket = getBucket(tid);
  int i = 0;
  std::shared_ptr<TransactionState> state;
  do {
    READ_LOCKER(locker, _transactions[bucket]._lock);

    auto it = _transactions[bucket]._managed.find(tid);
    if (it == _transactions[bucket]._managed.end()) {
      return nullptr;
    }

    ManagedTrx& mtrx = it->second;
    if (mtrx.type == MetaType::Tombstone || mtrx.expired() ||
        !::authorized(mtrx.user)) {
      return nullptr;  // no need to return anything
    }
    
    if (AccessMode::isWriteOrExclusive(mode)) {
      if (mtrx.type == MetaType::StandaloneAQL && mtrx.state->isReadOnlyTransaction()) {
        THROW_ARANGO_EXCEPTION_MESSAGE(
            TRI_ERROR_TRANSACTION_DISALLOWED_OPERATION,
            "not allowed to write lock an AQL transaction");
      }
      if (mtrx.rwlock.tryLockWrite()) {
        state = mtrx.state;
        break;
      }
    } else {
      if (mtrx.rwlock.tryLockRead()) {
        TRI_ASSERT(mode == AccessMode::Type::READ);
        state = mtrx.state;
        break;
      }
      THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_LOCKED,
                                     std::string("cannot read-lock, transaction ") + std::to_string(tid.id()) +
                                          " is already in use");
    }
    
    // simon: never allow concurrent use of transactions
    // either busy loop until we get the lock or throw an error
    
    LOG_TOPIC("abd72", TRACE, Logger::TRANSACTIONS)
        << "transaction " << tid << " is already in use (RO)";

    locker.unlock();  // failure;
    
    // simon: Two allowed scenarios:
    // 1. User sends concurrent write (CRUD) requests, (which was never intended to be possible)
    //    but now we do have to kind of support it otherwise shitty apps break
    // 2. one does a bulk write within a el-cheapo / V8 transaction into multiple shards
    //    on the same DBServer (still bad design).
    TRI_ASSERT(endTime.time_since_epoch().count() == 0 || !ServerState::instance()->isDBServer());

    if (!ServerState::isDBServer(role) && std::chrono::steady_clock::now() > endTime) {
      THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_LOCKED,
                                     std::string("cannot write-lock, transaction ") + std::to_string(tid.id()) +
                                         " is already in use");
    } else if ((i % 32) == 0) {
      LOG_TOPIC("9e972", DEBUG, Logger::TRANSACTIONS) << "waiting on trx write-lock " << tid;
      i = 0;
      if (_feature.server().isStopping()) {
        return nullptr;  // shutting down
      }
    }
    
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

  } while (true);

  if (state) {
    return std::make_shared<ManagedContext>(tid, std::move(state), /*responsibleForCommit*/false);
  }
  TRI_ASSERT(false);  // should be unreachable
  return nullptr;
}

void Manager::returnManagedTrx(TransactionId tid) noexcept {
  const size_t bucket = getBucket(tid);
  WRITE_LOCKER(writeLocker, _transactions[bucket]._lock);

  auto it = _transactions[bucket]._managed.find(tid);
  if (it == _transactions[bucket]._managed.end() || !::authorized(it->second.user)) {
    LOG_TOPIC("1d5b0", WARN, Logger::TRANSACTIONS)
        << "managed transaction was not found";
    TRI_ASSERT(false);
    return;
  }

  TRI_ASSERT(it->second.state != nullptr);

  // garbageCollection might soft abort used transactions
  const bool isSoftAborted = it->second.expiryTime == 0;
  if (!isSoftAborted) {
    it->second.updateExpiry();
  }
  
  it->second.rwlock.unlock();

  if (isSoftAborted) {
    abortManagedTrx(tid);
  }
}

/// @brief get the transasction state
transaction::Status Manager::getManagedTrxStatus(TransactionId tid) const {
  size_t bucket = getBucket(tid);
  READ_LOCKER(writeLocker, _transactions[bucket]._lock);

  auto it = _transactions[bucket]._managed.find(tid);
  if (it == _transactions[bucket]._managed.end() || !::authorized(it->second.user)) {
    return transaction::Status::UNDEFINED;
  }

  auto const& mtrx = it->second;

  if (mtrx.type == MetaType::Tombstone) {
    return mtrx.finalStatus;
  } else if (!mtrx.expired() && mtrx.state != nullptr) {
    return transaction::Status::RUNNING;
  } else {
    return transaction::Status::ABORTED;
  }
}

Result Manager::statusChangeWithTimeout(TransactionId tid, transaction::Status status) {
  double startTime = 0.0;
  constexpr double maxWaitTime = 2.0;
  Result res;
  while (true) {
    res = updateTransaction(tid, status, false);
    if (res.ok() || !res.is(TRI_ERROR_LOCKED)) {
      break;
    }
    if (startTime <= 0.0001) {  // fp tolerance
      startTime = TRI_microtime();
    } else if (TRI_microtime() - startTime > maxWaitTime) {
      // timeout
      break;
    }
    std::this_thread::yield();
  }
  return res;
}

Result Manager::commitManagedTrx(TransactionId tid) {
  return statusChangeWithTimeout(tid, transaction::Status::COMMITTED);
}

Result Manager::abortManagedTrx(TransactionId tid) {
  return statusChangeWithTimeout(tid, transaction::Status::ABORTED);
}

Result Manager::updateTransaction(TransactionId tid, transaction::Status status,
                                  bool clearServers) {
  TRI_ASSERT(status == transaction::Status::COMMITTED ||
             status == transaction::Status::ABORTED);

  LOG_TOPIC("7bd2f", DEBUG, Logger::TRANSACTIONS)
      << "managed trx " << tid << " updating to '" << (status == transaction::Status::COMMITTED ? "COMMITED" : "ABORTED") << "'";

  Result res;
  size_t const bucket = getBucket(tid);
  bool wasExpired = false;

  std::shared_ptr<TransactionState> state;
  {
    WRITE_LOCKER(writeLocker, _transactions[bucket]._lock);

    auto& buck = _transactions[bucket];
    auto it = buck._managed.find(tid);
    if (it == buck._managed.end() || !::authorized(it->second.user)) {
      return res.reset(TRI_ERROR_TRANSACTION_NOT_FOUND,
                       std::string("transaction '") + std::to_string(tid.id()) +
                           "' not found");
    }

    ManagedTrx& mtrx = it->second;
    TRY_WRITE_LOCKER(tryGuard, mtrx.rwlock);
    if (!tryGuard.isLocked()) {
      LOG_TOPIC("dfc30", DEBUG, Logger::TRANSACTIONS) << "transaction " << tid << " is in use";
      return res.reset(TRI_ERROR_LOCKED, std::string("read lock failed, transaction ") + std::to_string(tid.id()) +
                                             " is in use");
    }
    TRI_ASSERT(tryGuard.isLocked());

    if (mtrx.type == MetaType::StandaloneAQL) {
      return res.reset(TRI_ERROR_TRANSACTION_DISALLOWED_OPERATION,
                       "not allowed to change an AQL transaction");
    } else if (mtrx.type == MetaType::Tombstone) {
      TRI_ASSERT(mtrx.state == nullptr);
      // make sure everyone who asks gets the updated timestamp
      mtrx.updateExpiry();
      if (mtrx.finalStatus == status) {
        if (ServerState::instance()->isDBServer() &&
            tid.isFollowerTransactionId() &&
            mtrx.finalStatus == transaction::Status::ABORTED && 
            mtrx.intermediateCommits) {
          // we are trying to abort a follower transaction (again) that had intermediate
          // commits already. in this case we return a special error code, which makes
          // the leader drop us as a follower for all shards in the transaction.
          return res.reset(TRI_ERROR_CLUSTER_FOLLOWER_TRANSACTION_COMMIT_PERFORMED);
        }
        return res;  // all good
      } else {
        std::string msg("transaction was already ");
        msg.append(statusString(mtrx.finalStatus));
        return res.reset(TRI_ERROR_TRANSACTION_DISALLOWED_OPERATION, std::move(msg));
      }
    }
    TRI_ASSERT(mtrx.type == MetaType::Managed);

    if (mtrx.expired() && status != transaction::Status::ABORTED) {
      status = transaction::Status::ABORTED;
      wasExpired = true;
    }

    std::swap(state, mtrx.state);
    TRI_ASSERT(mtrx.state == nullptr);
    mtrx.type = MetaType::Tombstone;
    mtrx.updateExpiry();
    mtrx.finalStatus = status;
    if (state->numCommits() > 0) {
      // note that we have performed a commit or an intermediate commit.
      // this is necessary for follower transactions
      mtrx.intermediateCommits = true;
    }
    // it is sufficient to pretend that the operation already succeeded
  }

  TRI_ASSERT(state);
  if (!state) {  // this should never happen
    return res.reset(TRI_ERROR_INTERNAL, "managed trx in an invalid state");
  }

  auto abortTombstone = [&] {  // set tombstone entry to aborted
    WRITE_LOCKER(writeLocker, _transactions[bucket]._lock);
    auto& buck = _transactions[bucket];
    auto it = buck._managed.find(tid);
    if (it != buck._managed.end()) {
      it->second.finalStatus = Status::ABORTED;
    }
  };
  if (!state->isRunning()) {  // this also should not happen
    abortTombstone();
    return res.reset(TRI_ERROR_TRANSACTION_ABORTED,
                     "transaction was not running");
  }

  bool const isCoordinator = state->isCoordinator();
  bool const intermediateCommits = state->numCommits() > 0;

  transaction::Options trxOpts;
  MGMethods trx(std::make_shared<ManagedContext>(tid, std::move(state),
                                                 /*responsibleForCommit*/true), trxOpts);
  TRI_ASSERT(trx.state()->isRunning());
  TRI_ASSERT(trx.isMainTransaction());
  if (clearServers && !isCoordinator) {
    trx.state()->clearKnownServers();
  }
  if (status == transaction::Status::COMMITTED) {
    res = trx.commit();
    if (res.fail()) {  // set final status to aborted
      abortTombstone();
    }
  } else {
    res = trx.abort();
    if (intermediateCommits &&
        ServerState::instance()->isDBServer() &&
        tid.isFollowerTransactionId()) {
      // we are trying to abort a follower transaction that had intermediate
      // commits already. in this case we return a special error code, which makes
      // the leader drop us as a follower for all shards in the transaction.
      res.reset(TRI_ERROR_CLUSTER_FOLLOWER_TRANSACTION_COMMIT_PERFORMED);
    } else if (res.ok() && wasExpired) {
      res.reset(TRI_ERROR_TRANSACTION_ABORTED);
    } 
  }
  TRI_ASSERT(!trx.state()->isRunning());

  return res;
}

/// @brief calls the callback function for each managed transaction
void Manager::iterateManagedTrx(std::function<void(TransactionId, ManagedTrx const&)> const& callback) const {

  // iterate over all active transactions
  for (size_t bucket = 0; bucket < numBuckets; ++bucket) {
    READ_LOCKER(locker, _transactions[bucket]._lock);

    auto& buck = _transactions[bucket];
    for (auto const& it : buck._managed) {
      if (it.second.type == MetaType::Managed) {
        // we only care about managed transactions here
        callback(it.first, it.second);
      }
    }
  }
}

/// @brief collect forgotten transactions
bool Manager::garbageCollect(bool abortAll) {
  bool didWork = false;
  ::arangodb::containers::SmallVector<TransactionId, 64>::allocator_type::arena_type a1;
  ::arangodb::containers::SmallVector<TransactionId, 64> toAbort{a1};
  ::arangodb::containers::SmallVector<TransactionId, 64>::allocator_type::arena_type a2;
  ::arangodb::containers::SmallVector<TransactionId, 64> toErase{a2};

  for (size_t bucket = 0; bucket < numBuckets; ++bucket) {
    if (abortAll) {
      _transactions[bucket]._lock.lockWrite();
    } else {
      _transactions[bucket]._lock.lockRead();
    }
    auto scope = scopeGuard([&] {
      _transactions[bucket]._lock.unlock();
    });

    for (auto& it : _transactions[bucket]._managed) {
      ManagedTrx& mtrx = it.second;

      if (mtrx.type == MetaType::Managed) {
        TRI_ASSERT(mtrx.state != nullptr);
        if (abortAll || mtrx.expired()) {
          TRY_WRITE_LOCKER(tryGuard, mtrx.rwlock);  // needs lock to access state

          if (tryGuard.isLocked()) {
            TRI_ASSERT(mtrx.state->isRunning());
            TRI_ASSERT(it.first == mtrx.state->id());
            toAbort.emplace_back(mtrx.state->id());
            LOG_TOPIC("7ad3f", INFO, Logger::TRANSACTIONS)
            << "aborting expired transaction '" << it.first << "'";
          } else if (abortAll) {  // transaction is in use but we want to abort
            mtrx.expiryTime = 0;  // soft-abort transaction
            didWork = true;
            LOG_TOPIC("7ad4f", INFO, Logger::TRANSACTIONS)
            << "soft aborting transaction '" << it.first << "'";
          }
        }
      } else if (mtrx.type == MetaType::StandaloneAQL && mtrx.expired()) {
        LOG_TOPIC("7ad5f", INFO, Logger::TRANSACTIONS)
        << "expired AQL query transaction '" << it.first << "'";
      } else if (mtrx.type == MetaType::Tombstone && mtrx.expired()) {
        TRI_ASSERT(mtrx.state == nullptr);
        TRI_ASSERT(mtrx.finalStatus != transaction::Status::UNDEFINED);
        toErase.emplace_back(it.first);
      }
    }
  }

  for (TransactionId tid : toAbort) {
    LOG_TOPIC("6fbaf", INFO, Logger::TRANSACTIONS) << "garbage collecting "
                                                   << "transaction " << tid;
    Result res = updateTransaction(tid, Status::ABORTED, /*clearSrvs*/ true);
    // updateTransaction can return TRI_ERROR_TRANSACTION_ABORTED when it
    // successfully aborts, so ignore this error.
    // we can also get the TRI_ERROR_LOCKED error in case we cannot
    // immediately acquire the lock on the transaction. this _can_ happen
    // infrequently, but is not an error
    if (res.fail() && 
        !res.is(TRI_ERROR_TRANSACTION_ABORTED) && 
        !res.is(TRI_ERROR_CLUSTER_FOLLOWER_TRANSACTION_COMMIT_PERFORMED) && 
        !res.is(TRI_ERROR_LOCKED)) {
      LOG_TOPIC("0a07f", INFO, Logger::TRANSACTIONS) << "error while aborting "
                                                        "transaction: "
                                                     << res.errorMessage();
    }
    didWork = true;
  }
  
  for (TransactionId tid : toErase) {
    size_t const bucket = getBucket(tid);
    WRITE_LOCKER(locker, _transactions[bucket]._lock);
    _transactions[bucket]._managed.erase(tid);
  }

  if (didWork) {
    LOG_TOPIC("e5b31", INFO, Logger::TRANSACTIONS)
        << "aborted expired transactions";
  }

  return didWork;
}

/// @brief abort all transactions matching
bool Manager::abortManagedTrx(std::function<bool(TransactionState const&, std::string const&)> cb) {
  ::arangodb::containers::SmallVector<TransactionId, 64>::allocator_type::arena_type arena;
  ::arangodb::containers::SmallVector<TransactionId, 64> toAbort{arena};

  for (size_t bucket = 0; bucket < numBuckets; ++bucket) {
    READ_LOCKER(locker, _transactions[bucket]._lock);

    auto it = _transactions[bucket]._managed.begin();
    while (it != _transactions[bucket]._managed.end()) {
      ManagedTrx& mtrx = it->second;
      if (mtrx.type == MetaType::Managed) {
        TRI_ASSERT(mtrx.state != nullptr);
        TRY_READ_LOCKER(tryGuard, mtrx.rwlock);  // needs lock to access state
        if (tryGuard.isLocked() && cb(*mtrx.state, mtrx.user)) {
          toAbort.emplace_back(it->first);
        }
      }

      ++it;  // next
    }
  }

  for (TransactionId tid : toAbort) {
    Result res = updateTransaction(tid, Status::ABORTED, /*clearSrvs*/ true);
    if (res.fail() &&
        !res.is(TRI_ERROR_CLUSTER_FOLLOWER_TRANSACTION_COMMIT_PERFORMED)) {
      LOG_TOPIC("2bf48", INFO, Logger::TRANSACTIONS) << "error aborting "
                                                        "transaction: "
                                                     << res.errorMessage();
    }
  }
  return !toAbort.empty();
}

void Manager::toVelocyPack(VPackBuilder& builder, std::string const& database,
                           std::string const& username, bool fanout) const {
  TRI_ASSERT(!builder.isClosed());

  if (fanout) {
    TRI_ASSERT(ServerState::instance()->isCoordinator());
    auto& ci = _feature.server().getFeature<ClusterFeature>().clusterInfo();

    NetworkFeature const& nf = _feature.server().getFeature<NetworkFeature>();
    network::ConnectionPool* pool = nf.pool();
    if (pool == nullptr) {
      THROW_ARANGO_EXCEPTION(TRI_ERROR_SHUTTING_DOWN);
    }

    std::vector<network::FutureRes> futures;
    auto auth = AuthenticationFeature::instance();

    network::RequestOptions options;
    options.database = database;
    options.timeout = network::Timeout(30.0);
    options.param("local", "true");

    VPackBuffer<uint8_t> body;

    for (auto const& coordinator : ci.getCurrentCoordinators()) {
      if (coordinator == ServerState::instance()->getId()) {
        // ourselves!
        continue;
      }

      network::Headers headers;
      if (auth != nullptr && auth->isActive()) {
        if (!username.empty()) {
          headers.try_emplace(StaticStrings::Authorization,
                              "bearer " + fuerte::jwt::generateUserToken(
                                              auth->tokenCache().jwtSecret(), username));
        } else {
          headers.try_emplace(StaticStrings::Authorization,
                              "bearer " + auth->tokenCache().jwtToken());
        }
      }

      auto f = network::sendRequest(pool, "server:" + coordinator,
                                    fuerte::RestVerb::Get, "/_api/transaction",
                                    body, options, std::move(headers));
      futures.emplace_back(std::move(f));
    }

    if (!futures.empty()) {
      auto responses = futures::collectAll(futures).get();
      for (auto const& it : responses) {
        if (!it.hasValue()) {
          THROW_ARANGO_EXCEPTION(TRI_ERROR_CLUSTER_BACKEND_UNAVAILABLE);
        }
        auto& res = it.get();
        if (res.response && res.response->statusCode() == fuerte::StatusOK) {
          VPackSlice slice = res.slice();
          if (slice.isObject()) {
            slice = slice.get("transactions");
            if (slice.isArray()) {
              builder.add(VPackArrayIterator(slice));
            }
          }
        }
      }
    }
  }

  // merge with local transactions
  iterateManagedTrx([&builder](TransactionId tid, ManagedTrx const& trx) {
    if (::authorized(trx.user)) {
      builder.openObject(true);
      builder.add("id", VPackValue(std::to_string(tid.id())));
      builder.add("state", VPackValue(transaction::statusString(trx.state->status())));
      builder.close();
    }
  });
}

Result Manager::abortAllManagedWriteTrx(std::string const& username, bool fanout) {
  LOG_TOPIC("bba16", INFO, Logger::QUERIES)
      << "aborting all " << (fanout ? "" : "local ") << "write transactions";
  Result res;

  DatabaseFeature& databaseFeature = _feature.server().getFeature<DatabaseFeature>();
  databaseFeature.enumerate([](TRI_vocbase_t* vocbase) {
    auto queryList = vocbase->queryList();
    TRI_ASSERT(queryList != nullptr);
    // we are only interested in killed write queries
    queryList->kill([](aql::Query& query) {
      return query.isModificationQuery();
    }, false);
  });

  // abort local transactions
  abortManagedTrx([](TransactionState const& state, std::string const& user) {
    return ::authorized(user) && !state.isReadOnlyTransaction();
  });

  if (fanout && ServerState::instance()->isCoordinator()) {
    auto& ci = _feature.server().getFeature<ClusterFeature>().clusterInfo();

    NetworkFeature const& nf = _feature.server().getFeature<NetworkFeature>();
    network::ConnectionPool* pool = nf.pool();
    if (pool == nullptr) {
      THROW_ARANGO_EXCEPTION(TRI_ERROR_SHUTTING_DOWN);
    }

    std::vector<network::FutureRes> futures;
    auto auth = AuthenticationFeature::instance();

    network::RequestOptions reqOpts;
    reqOpts.timeout = network::Timeout(30.0);
    reqOpts.param("local", "true");

    VPackBuffer<uint8_t> body;

    for (auto const& coordinator : ci.getCurrentCoordinators()) {
      if (coordinator == ServerState::instance()->getId()) {
        // ourselves!
        continue;
      }

      network::Headers headers;
      if (auth != nullptr && auth->isActive()) {
        if (!username.empty()) {
          headers.try_emplace(StaticStrings::Authorization,
                              "bearer " + fuerte::jwt::generateUserToken(
                                              auth->tokenCache().jwtSecret(), username));
        } else {
          headers.try_emplace(StaticStrings::Authorization,
                              "bearer " + auth->tokenCache().jwtToken());
        }
      }

      auto f = network::sendRequest(pool, "server:" + coordinator, fuerte::RestVerb::Delete,
                                    "_api/transaction/write", body, reqOpts,
                                    std::move(headers));
      futures.emplace_back(std::move(f));
    }

    for (auto& f : futures) {
      network::Response& resp = f.get();
      
      if (resp.response && resp.response->statusCode() != fuerte::StatusOK) {
        auto slices = resp.response->slices();
        if (!slices.empty()) {
          VPackSlice slice = slices[0];
          res.reset(network::resultFromBody(slice, TRI_ERROR_FAILED));
        }
      }
    }
  }

  return res;
}

}  // namespace transaction
}  // namespace arangodb
