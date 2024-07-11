////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2024-2024 ArangoDB GmbH, Cologne, Germany
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
/// @author Michael Hackstein
////////////////////////////////////////////////////////////////////////////////

#include "LeaseManager.h"

#include "Cluster/LeaseManager/AbortLeaseInformation.h"
#include "Cluster/LeaseManager/LeaseManagerNetworkHandler.h"
#include "Cluster/LeaseManager/LeasesReport.h"
#include "Cluster/RebootTracker.h"

#include "Logger/LogMacros.h"
#include "Cluster/ServerState.h"

using namespace arangodb;
using namespace arangodb::cluster;

LeaseManager::LeaseFromRemoteGuard::LeaseFromRemoteGuard(PeerState peer,
                                                         LeaseId id,
                                                         LeaseManager* mgr)
    : _peerState(std::move(peer)), _id{id}, _manager(mgr) {}

LeaseManager::LeaseFromRemoteGuard::~LeaseFromRemoteGuard() {
  if (_manager != nullptr) {
    _manager->returnLeaseFromRemote(_peerState, _id);
    _manager.reset();
  }
}

auto LeaseManager::LeaseFromRemoteGuard::cancel() const noexcept -> void {
  TRI_ASSERT(_manager != nullptr) << "Called Cancel after move";
  if (_manager != nullptr) {
    _manager->cancelLeaseFromRemote(_peerState, _id);
  }
}

LeaseManager::LeaseToRemoteGuard::~LeaseToRemoteGuard() {
  if (_manager != nullptr) {
    _manager->returnLeaseToRemote(_peerState, _id);
    _manager.reset();
  }
}

auto LeaseManager::LeaseToRemoteGuard::cancel() const noexcept -> void {
  TRI_ASSERT(_manager != nullptr) << "Called Cancel after move";
  if (_manager != nullptr) {
    _manager->cancelLeaseToRemote(_peerState, _id);
  }
}

void LeaseManager::OpenHandouts::registerTombstone(PeerState const& server,
                                                   LeaseId id,
                                                   LeaseManager& mgr) {
  if (auto tombIt = graveyard.find(server); tombIt == graveyard.end()) {
    // Server not yet in Graveyard, add it.
    // Note: If the server already rebooted callMeOnChange will be triggered
    // directly.
    auto undertaker = mgr._rebootTracker.callMeOnChange(
        server,
        [&, server = server]() {
          // The server has rebooted make sure we erase all it's entries
          // This needs to call abort methods of all leases.
          mgr._leasedToRemotePeers.doUnderLock(
              [&](auto& guarded) { guarded.graveyard.erase(server); });
        },
        "Let the undertaker clear the graveyard.");
    auto tombIt2 = graveyard.emplace(
        server, GraveyardOfPeer{._serverAbortCallback{std::move(undertaker)},
                                ._list{id}});
    TRI_ASSERT(tombIt2.second) << "Failed to register new peer state";
  } else {
    // Just add the ID to the graveyard.
    tombIt->second._list.emplace(id);
  }
}

LeaseManager::LeaseManager(
    RebootTracker& rebootTracker,
    std::unique_ptr<ILeaseManagerNetworkHandler> networkHandler,
    Scheduler& scheduler)
    : _rebootTracker(rebootTracker),
      _networkHandler(std::move(networkHandler)),
      _scheduler(scheduler),
      _leasedFromRemotePeers(),
      _leasedToRemotePeers() {
  TRI_ASSERT(_networkHandler != nullptr)
      << "Broken setup. We do not have a network handler in LeaseManager.";
}

auto LeaseManager::requireLeaseInternal(PeerState const& requestFrom,
                                        std::function<std::string()> details,
                                        std::unique_ptr<LeaseEntry> leaseEntry)
    -> LeaseFromRemoteGuard {
  // NOTE: In theory _nextLeaseId can overflow here, but that should never be a
  // problem. if we ever reach that point without restarting the server, it is
  // highly unlikely that we still have handed out low number leases.

  auto id = LeaseId{_lastUsedLeaseId++};
  _leasedFromRemotePeers.doUnderLock([&](auto& guarded) {
    auto& list = guarded.list;
    if (auto it = list.find(requestFrom); it == list.end()) {
      auto trackerGuard = _rebootTracker.callMeOnChange(
          requestFrom,
          [&, peer = requestFrom]() {
            // The server has rebooted make sure we erase all it's entries
            // This needs to call abort methods of all leases.
            _leasedFromRemotePeers.doUnderLock(
                [&](auto& guarded) { guarded.list.erase(peer); });
          },
          "Abort leases of the LeaseManager.");
      auto it2 = list.emplace(
          requestFrom,
          LeaseListOfPeer{._serverAbortCallback{std::move(trackerGuard)},
                          ._mapping{}});
      TRI_ASSERT(it2.second) << "Failed to register new peer state";
      it2.first->second._mapping.emplace(
          id, std::make_unique<LeaseListOfPeer::Entry>(std::move(leaseEntry),
                                                       std::move(details)));
    } else {
      it->second._mapping.emplace(
          id, std::make_unique<LeaseListOfPeer::Entry>(std::move(leaseEntry),
                                                       std::move(details)));
    }
  });

  return LeaseFromRemoteGuard{requestFrom, std::move(id), this};
}

auto LeaseManager::handoutLeaseInternal(PeerState const& requestedBy,
                                        LeaseId leaseId,
                                        std::function<std::string()> details,
                                        std::unique_ptr<LeaseEntry> leaseEntry)
    -> ResultT<LeaseToRemoteGuard> {
  auto registeredLease = _leasedToRemotePeers.doUnderLock([&](auto& guarded)
                                                              -> Result {
    auto& graveyard = guarded.graveyard;
    if (!graveyard.empty()) {
      // NOTE: In most cases the graveyard will be empty, as we only
      // protect against a very small timeframe.
      // If it is not empty, we need to check if the server is in there.
      if (auto tombIt = graveyard.find(requestedBy);
          tombIt != graveyard.end()) {
        if (tombIt->second._list.contains(leaseId)) {
          // The server is in the graveyard, we need to abort the lease.
          // We do not need to add it to the graveyard again, as the server is
          // already in there. We do not need to add it to the abort list, as
          // the server is already in the graveyard.

          // Abort this lease entry, pretend it never existed.
          leaseEntry->abort();
          return {TRI_ERROR_TRANSACTION_ABORTED,
                  fmt::format("LeaseId {} for server {} is already aborted.",
                              leaseId.id(), requestedBy.serverId)};
        }
      }
    }
    auto& list = guarded.list;
    if (auto it = list.find(requestedBy); it == list.end()) {
      auto trackerGuard = _rebootTracker.callMeOnChange(
          requestedBy,
          [&, peer = requestedBy]() {
            // The server has rebooted make sure we erase all it's entries
            // This needs to call abort methods of all leases.
            _leasedToRemotePeers.doUnderLock(
                [&](auto& guarded) { guarded.list.erase(peer); });
          },
          "Abort leases of the LeaseManager.");
      auto it2 = list.emplace(
          requestedBy,
          LeaseListOfPeer{._serverAbortCallback{std::move(trackerGuard)},
                          ._mapping{}});
      TRI_ASSERT(it2.second) << "Failed to register new peer state";
      TRI_ASSERT(it2.first->second._mapping.empty())
          << "We just added the list for this Peer, it is required to be empty";
      auto insertedElement = it2.first->second._mapping.emplace(
          leaseId, std::make_unique<LeaseListOfPeer::Entry>(
                       std::move(leaseEntry), std::move(details)));
      TRI_ASSERT(insertedElement.second)
          << "We just added the list for this Peer, it is required to be empty";
      if (!insertedElement.second) {
        // Lease with this ID already exists.
        return {TRI_ERROR_ARANGO_DUPLICATE_IDENTIFIER,
                fmt::format("Lease with ID {} already exists.", leaseId.id())};
      }
    } else {
      auto exists = it->second._mapping.contains(leaseId);
      if (!exists) {
        auto insertedElement = it->second._mapping.try_emplace(
            leaseId, std::make_unique<LeaseListOfPeer::Entry>(
                         std::move(leaseEntry), std::move(details)));
        TRI_ASSERT(insertedElement.second)
            << "Failed to add an entry in a map.";
      } else {
        // Abort this lease entry, pretend it never existed.
        leaseEntry->abort();
        // Lease with this ID already exists.
        return {TRI_ERROR_ARANGO_DUPLICATE_IDENTIFIER,
                fmt::format("Lease with ID {} already exists.", leaseId.id())};
      }
    }
    return TRI_ERROR_NO_ERROR;
  });
  if (registeredLease.fail()) {
    return registeredLease;
  }
  auto guard = LeaseToRemoteGuard{requestedBy, std::move(leaseId), this};
  return ResultT<LeaseToRemoteGuard>{std::move(guard)};
}

// TODO: We can make this co_await
auto LeaseManager::reportLeases(GetType type,
                                std::optional<ServerID> forServer) const
    -> ManyServersLeasesReport {
  TRI_ASSERT(forServer.has_value() || type != FOR_SERVER)
      << "We can only filter for a Server, if we use the FOR_SERVER type";
  switch (type) {
    case LOCAL: {
      ManyServersLeasesReport report;
      report.serverLeases.emplace(ServerState::instance()->getId(),
                                  prepareLocalReport(std::nullopt));
      return report;
    }
    case ALL: {
      auto report = _networkHandler->collectFullLeaseReport().waitAndGet();
      report.serverLeases.emplace(ServerState::instance()->getId(),
                                  prepareLocalReport(std::nullopt));
      return report;
    }
    case MINE: {
      ManyServersLeasesReport report;
      report.serverLeases.emplace(ServerState::instance()->getId(),
                                  prepareLocalReport(std::nullopt));
      return report;
    }
    case FOR_SERVER: {
      ManyServersLeasesReport report;
      report.serverLeases.emplace(ServerState::instance()->getId(),
                                  prepareLocalReport(std::move(forServer)));
      return report;
    }
  }
  ADB_PROD_ASSERT(false) << "Reached unreachable code";
}

auto LeaseManager::returnLeaseFromRemote(PeerState const& peerState,
                                         LeaseId const& leaseId) noexcept
    -> void {
  bool addLeaseToAbort = false;
  _leasedFromRemotePeers.doUnderLock([&](auto& guard) {
    auto& list = guard.list;
    if (auto it = list.find(peerState); it != list.end()) {
      // The lease may already be removed, e.g. by RebootTracker.
      // So we do not really care if it is removed from this list here or not.
      if (auto lease = it->second._mapping.find(leaseId);
          lease != it->second._mapping.end()) {
        // We should abort the Guard here.
        // We returned our lease no need to tell us to abort;
        lease->second->entry->abort();
        addLeaseToAbort = true;
        // Now delete the lease from the list.
        it->second._mapping.erase(lease);
      }
    }
    // else nothing to do, lease already gone.
  });
  if (addLeaseToAbort) {
    _leasesToAbort.doUnderLock([&](auto& guard) {
      if (auto toAbortServer = guard.abortList.find(peerState.serverId);
          toAbortServer != guard.abortList.end()) {
        // Flag this leaseId to be erased in next run.
        toAbortServer->second.leasedFrom.emplace_back(leaseId);
      } else {
        // Have nothing to erase for this server, schedule it.
        guard.abortList.emplace(
            peerState.serverId,
            LeasesToAbort::LeasePair{std::vector<LeaseId>{leaseId},
                                     std::vector<LeaseId>{}});
      }
    });
  }
  // TODO: Run this in Background
  sendAbortRequestsForAbandonedLeases();
}

auto LeaseManager::cancelLeaseFromRemote(PeerState const& peerState,
                                         LeaseId const& leaseId) noexcept
    -> void {
  _leasedFromRemotePeers.doUnderLock([&](auto& guard) {
    auto& list = guard.list;
    if (auto it = list.find(peerState); it != list.end()) {
      // The lease may already be removed, e.g. by RebootTracker.
      // So we do not really care if it is removed from this list here or not.
      if (auto it2 = it->second._mapping.find(leaseId);
          it2 != it->second._mapping.end()) {
        it2->second->entry->abort();
        // Now delete the lease from the list.
        // To avoid calling remote about abortion.
        it->second._mapping.erase(it2);
      }
    }
    // else nothing to do, lease already gone.
  });
}

auto LeaseManager::returnLeaseToRemote(PeerState const& peerState,
                                       LeaseId const& leaseId) noexcept
    -> void {
  bool addLeaseToAbort = false;
  _leasedToRemotePeers.doUnderLock([&](auto& guard) {
    auto& list = guard.list;
    if (auto it = list.find(peerState); it != list.end()) {
      // The lease may already be removed, e.g. by RebootTracker.
      // So we do not really care if it is removed from this list here or not.
      if (auto lease = it->second._mapping.find(leaseId);
          lease != it->second._mapping.end()) {
        // We should abort the Guard here.
        // We returned our lease no need to tell us to abort;
        lease->second->entry->abort();
        addLeaseToAbort = true;
        // Now delete the lease from the list.
        it->second._mapping.erase(lease);
      }
    }
    // else nothing to do, lease already gone.
  });
  if (addLeaseToAbort) {
    _leasesToAbort.doUnderLock([&](auto& guard) {
      if (auto toAbortServer = guard.abortList.find(peerState.serverId);
          toAbortServer != guard.abortList.end()) {
        // Flag this leaseId to be erased in next run.
        toAbortServer->second.leasedTo.emplace_back(leaseId);
      } else {
        // Have nothing to erase for this server, schedule it.
        guard.abortList.emplace(
            peerState.serverId,
            LeasesToAbort::LeasePair{std::vector<LeaseId>{},
                                     std::vector<LeaseId>{leaseId}});
      }
    });
  }
  // TODO: Run this in Background
  sendAbortRequestsForAbandonedLeases();
}

auto LeaseManager::cancelLeaseToRemote(PeerState const& peerState,
                                       LeaseId const& leaseId) noexcept
    -> void {
  _leasedToRemotePeers.doUnderLock([&](auto& guard) {
    auto& list = guard.list;
    if (auto it = list.find(peerState); it != list.end()) {
      // The lease may already be removed, e.g. by RebootTracker.
      // So we do not really care if it is removed from this list here or not.
      if (auto it2 = it->second._mapping.find(leaseId);
          it2 != it->second._mapping.end()) {
        it2->second->entry->abort();
        // Now delete the lease from the list.
        // To avoid calling remote about abortion.
        it->second._mapping.erase(it2);
      }
    }
    // else nothing to do, lease already gone.
  });
}

auto LeaseManager::sendAbortRequestsForAbandonedLeases() noexcept -> void {
  std::vector<futures::Future<futures::Unit>> futures;
  std::unordered_map<ServerID, LeasesToAbort::LeasePair> abortList{};
  // Steal the list from the guarded structure.
  // Now other can register again while we abort the open items.
  _leasesToAbort.doUnderLock(
      [&](auto& guarded) { std::swap(guarded.abortList, abortList); });
  for (auto const& [serverId, leasePair] : abortList) {
    // NOTE: We intentionally copy the leaseIds list here, as we probably need
    // to add it again locally it in case of an error.
    futures.emplace_back(
        _networkHandler
            ->abortIds(serverId, leasePair.leasedFrom, leasePair.leasedTo)
            .thenValue([&](Result res) {
              if (!res.ok()) {
                // TODO: Abort if server is permanently gone!
                // TODO: Log?
                // We failed to send the abort request, we should try again
                // later. So let us push the leases back into the list.
                _leasesToAbort.doUnderLock([&](auto& guarded) {
                  auto& insertPair = guarded.abortList[serverId];
                  insertPair.leasedFrom.insert(insertPair.leasedFrom.end(),
                                               leasePair.leasedFrom.begin(),
                                               leasePair.leasedFrom.end());
                  insertPair.leasedTo.insert(insertPair.leasedTo.end(),
                                             leasePair.leasedTo.begin(),
                                             leasePair.leasedTo.end());
                });
              }
              // else: We successfully aborted the open IDs. Forget about them
              // now.
            }));
  }
  // Wait on futures outside the lock, as the futures themselves will lock the
  // guarded structure.
  futures::collectAll(futures.begin(), futures.end()).waitAndGet();
}

auto LeaseManager::abortLeasesForServer(AbortLeaseInformation info) noexcept
    -> void {
  _leasedToRemotePeers.doUnderLock([&](auto& list) -> void {
    auto it = list.list.find(info.server);
    if (it != list.list.end()) {
      for (auto const& id : info.leasedTo) {
        // Try to erase the ID from the list.
        // Do not put the ID on the abort list, the remote server just told us
        // to remove it.
        auto entry = it->second._mapping.find(id);
        if (entry != it->second._mapping.end()) {
          _scheduler.queue(RequestLane::CLUSTER_INTERNAL,
                           [lease = std::move(entry->second->entry)]() {
                             // This Callback is okay to be empty, we only need
                             // the lease to go out of scope and call the
                             // destructor.
                           });
          it->second._mapping.erase(entry);
        } else {
          // Rare case: Element Aborted that does not yet exist.
          // Add a Tombstone for it
          list.registerTombstone(info.server, id, *this);
        }
      }
    } else {
      for (auto const& id : info.leasedTo) {
        // Rare case: The server has not yet registered anything
        // Or was already removed by the RebootTracker.
        // And then we handle the Abort call.
        list.registerTombstone(info.server, id, *this);
      }
    }
  });
  _leasedFromRemotePeers.doUnderLock([&](auto& list) {
    auto it = list.list.find(info.server);
    if (it != list.list.end()) {
      for (auto const& id : info.leasedFrom) {
        // Try to erase the ID from the list.
        // Do not put the ID on the abort list, the remote server just told us
        // to remove it.
        auto entry = it->second._mapping.find(id);
        if (entry != it->second._mapping.end()) {
          _scheduler.queue(RequestLane::CLUSTER_INTERNAL,
                           [lease = std::move(entry->second->entry)]() {
                             // This Callback is okay to be empty, we only need
                             // the lease to go out of scope and call the
                             // destructor.
                           });
          it->second._mapping.erase(entry);
        }
        // NOTE: We do not need tombstone handling here.
        // This server is generating the IDs, so it cannot abort them before.
      }
    }
  });
}

auto LeaseManager::prepareLocalReport(
    std::optional<ServerID> onlyForServer) const noexcept -> LeasesReport {
  LeasesReport report;
  {
    _leasedFromRemotePeers.doUnderLock([&report](auto& guarded) {
      for (auto const& [peerState, leaseEntry] : guarded.list) {
        std::vector<std::string> entries;
        for (auto const& [id, entry] : leaseEntry._mapping) {
          entries.emplace_back(
              fmt::format("{} -> {}", id.id(), entry->details()));
        }
        std::string peerString =
            fmt::format("{}:{}", peerState.serverId,
                        basics::StringUtils::itoa(peerState.rebootId.value()));
        report.leasesFromRemote.emplace(std::move(peerString),
                                        std::move(entries));
      }
    });
  }
  {
    _leasedToRemotePeers.doUnderLock([&report](auto& guarded) {
      for (auto const& [peerState, leaseEntry] : guarded.list) {
        std::vector<std::string> entries;
        for (auto const& [id, entry] : leaseEntry._mapping) {
          entries.emplace_back(
              fmt::format("{} -> {}", id.id(), entry->details()));
        }
        std::string peerString =
            fmt::format("{}:{}", peerState.serverId,
                        basics::StringUtils::itoa(peerState.rebootId.value()));
        report.leasesToRemote.emplace(std::move(peerString),
                                      std::move(entries));
      }
    });
  }
  return report;
}
