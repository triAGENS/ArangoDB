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
/// @author Max Neunhoeffer
////////////////////////////////////////////////////////////////////////////////

#include "AutoRebalance.h"
#include "Basics/debugging.h"

#include <algorithm>
#include <queue>
#include <random>
#include <string_view>

#include <velocypack/Iterator.h>
#include <velocypack/velocypack-common.h>

using namespace arangodb::cluster::rebalance;

#ifdef ARANGODB_USE_GOOGLE_TESTS
namespace {

std::string_view const charset{
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"};

std::string randomReadableString(uint32_t len) {
  std::default_random_engine rng(std::random_device{}());
  using Dist = std::uniform_int_distribution<>;
  auto m = static_cast<Dist::result_type>(charset.size() - 1);
  auto dist = Dist(0, m);
  auto randchar = [&dist, &rng]() { return charset[dist(rng)]; };
  std::string s(len, 0);
  std::generate_n(s.begin(), len, randchar);
  return s;
}

}  // namespace
#endif

#ifdef ARANGODB_USE_GOOGLE_TESTS
void AutoRebalanceProblem::createCluster(uint32_t nrDBServer, bool withZones) {
  // Create zones:
  zones.clear();
  for (uint32_t i = 0; i < nrDBServer; ++i) {
    zones.emplace_back(Zone{.id = "ZONE_" + ::randomReadableString(16)});
  }

  // Create dbservers:
  dbServers.clear();
  for (uint32_t i = 0; i < nrDBServer; ++i) {
    dbServers.emplace_back(DBServer{
        .id = "PRMR_" + ::randomReadableString(16),
        .shortName = "DBServer" + std::to_string(i),
        .volumeSize = 1ull << 34,
        .freeDiskSize = 1ull << 34,
        .zone = withZones ? i : 0,
    });
  }
  for (auto const& db : dbServers) {
    serversHealthInfo.insert(db.id);
  }
}
#endif

uint64_t AutoRebalanceProblem::createDatabase(std::string const& name,
                                              double weight) {
  uint64_t dbId = databases.size();
  databases.emplace_back(Database{.name = name,
                                  .id = dbId,
                                  .weight = weight,
                                  .blocked = false,
                                  .ignored = false});
  dbByName.try_emplace(name, dbId);
  return dbId;
}

uint64_t AutoRebalanceProblem::createCollection(std::string const& name,
                                                std::string const& dbName,
                                                uint32_t numberOfShards,
                                                uint32_t replicationFactor,
                                                double weight) {
  // Sort out database name and ID:
  auto it = dbByName.find(dbName);
  if (it == dbByName.end()) {
    return std::numeric_limits<uint64_t>::max();
  }
  uint64_t dbId = it->second;

  auto collId = static_cast<decltype(databases[0].collections)::value_type>(
      collections.size());

  // First create the shards:
  std::vector<uint32_t> positionsNewShards;
  for (uint32_t i = 0; i < numberOfShards; ++i) {
    uint32_t newId = static_cast<uint32_t>(shards.size());
    shards.emplace_back(Shard{
        .id = newId,
        .leader = 0,
        .replicationFactor = replicationFactor,
        .size = 1024 * 1024,
        .collectionId = collId,
        .weight = weight,
        .blocked = false,
        .ignored = false,
        .isSystem = false,
    });
    for (uint32_t j = 1; j < replicationFactor; ++j) {
      shards[newId].followers.push_back(j);
    }
    positionsNewShards.push_back(newId);
  }

  // Now create the collection object:
  collections.emplace_back(Collection{.name = name,
                                      .id = collId,
                                      .dbId = dbId,
                                      .weight = weight,
                                      .blocked = false,
                                      .ignored = false});
  for (auto& i : positionsNewShards) {
    collections.back().shards.push_back(i);
  }
  auto& db = databases[dbId];
  db.collections.push_back(collId);
  dbCollByName.try_emplace(databases[dbId].name + "/" + name, collId);
  return collId;
}

#ifdef ARANGODB_USE_GOOGLE_TESTS
void AutoRebalanceProblem::createRandomDatabasesAndCollections(
    uint32_t nrDBs, uint32_t nrColls, uint32_t minReplFactor,
    uint32_t maxReplFactor) {
  shards.clear();
  collections.clear();
  databases.clear();
  dbCollByName.clear();
  dbByName.clear();

  uint32_t currId = 123452;
  std::default_random_engine rng(std::random_device{}());
  std::uniform_int_distribution<> replRand(minReplFactor, maxReplFactor);

  for (uint32_t i = 0; i < nrDBs; ++i) {
    std::string dbName = "DB_" + randomReadableString(8);
    createDatabase(dbName, currId++);
    std::uniform_int_distribution<> dist(1, 5);
    uint32_t nr = nrColls + dist(rng);
    for (uint32_t j = 0; j < nr; ++j) {
      createCollection("COLL_" + randomReadableString(12), dbName, dist(rng),
                       replRand(rng));
    }
  }
}
#endif

void AutoRebalanceProblem::distributeShardsRandomly(
    std::vector<double> const& probabilities) {
  std::default_random_engine rng(std::random_device{}());
  std::uniform_real_distribution<double> random;
  auto nrDBservers = static_cast<std::uint32_t>(dbServers.size());
  TRI_ASSERT(nrDBservers == probabilities.size());
  for (auto& s : shards) {
    // First the leader:
    uint64_t used = 0;
    double r = random(rng);
    uint32_t i;
    for (i = 0; i + 1 < nrDBservers && r > probabilities[i]; ++i) {
    }
    s.leader = i;
    used |= 1ull << i;
    // Now the followers, we need to exclude already used ones:
    s.followers.clear();
    TRI_ASSERT(s.replicationFactor <=
               nrDBservers);  // otherwise this will never terminate
    for (uint32_t j = 1; j < s.replicationFactor; ++j) {
      do {
        r = random(rng);
        for (i = 0; i + 1 < nrDBservers && r > probabilities[i]; ++i) {
        }
      } while (used & (1ull << i));
      used |= 1ull << i;
      s.followers.push_back(i);
    }
  }
}

ShardImbalance AutoRebalanceProblem::computeShardImbalance() const {
  ShardImbalance res(dbServers.size());

  for (auto const& s : shards) {
    if (s.isSystem) {
      res.totalShardsFromSystemCollections += 1;
    }
    res.numberShards[s.leader] += 1;
    res.sizeUsed[s.leader] += s.size;
    for (uint32_t i = 0; i < s.followers.size(); ++i) {
      res.numberShards[s.followers[i]] += 1;
      res.sizeUsed[s.followers[i]] += s.size;
    }
  }
  double totalVolumes = 0;
  for (size_t i = 0; i < dbServers.size(); ++i) {
    res.totalUsed += res.sizeUsed[i];
    res.totalShards += res.numberShards[i];
    totalVolumes += dbServers[i].volumeSize;
  }
  for (size_t i = 0; i < dbServers.size(); ++i) {
    res.targetSize[i] = dbServers[i].volumeSize / totalVolumes * res.totalUsed;
  }
  for (size_t i = 0; i < dbServers.size(); ++i) {
    res.imbalance += pow(res.sizeUsed[i] - res.targetSize[i], 2.0);
  }

  return res;
}

std::vector<double> AutoRebalanceProblem::piCoefficients(
    Collection const& c) const {
  std::vector<double> leaders;
  leaders.resize(dbServers.size(), 0);
  std::vector<bool> haveShards;
  if (c.shards.size() == 1) {
    return leaders;  // No contribution for 1 shard collections
  }
  haveShards.resize(dbServers.size(), false);
  uint32_t dbServersAffected = 0;
  double sum = 0;
  for (auto const sindex : c.shards) {
    leaders[shards[sindex].leader] += 1.0;
    sum += 1.0;
    if (!haveShards[shards[sindex].leader]) {
      haveShards[shards[sindex].leader] = true;
      ++dbServersAffected;
    }
    for (auto const f : shards[sindex].followers) {
      if (!haveShards[f]) {
        haveShards[f] = true;
        ++dbServersAffected;
      }
    }
  }
  double avg = sum / dbServersAffected;
  for (size_t i = 0; i < dbServers.size(); ++i) {
    if (haveShards[i]) {  // only if affected
      leaders[i] = pow(leaders[i] - avg, 2.0) * _piFactor;
    }
  }
  return leaders;
}

LeaderImbalance AutoRebalanceProblem::computeLeaderImbalance() const {
  LeaderImbalance res(dbServers.size());

  for (auto const& s : shards) {
    res.numberShards[s.leader] += 1;
    res.weightUsed[s.leader] += s.weight;
  }
  double totalCapacity = 0;
  for (size_t i = 0; i < dbServers.size(); ++i) {
    res.leaderDupl[i] = 0;
    res.totalWeight += res.weightUsed[i];
    res.totalShards += res.numberShards[i];
    totalCapacity += dbServers[i].CPUcapacity;
  }
  for (size_t i = 0; i < dbServers.size(); ++i) {
    res.targetWeight[i] =
        res.totalWeight / totalCapacity * dbServers[i].CPUcapacity;
  }
  for (auto const& c : collections) {
    std::vector<double> pis = piCoefficients(c);
    for (size_t i = 0; i < dbServers.size(); ++i) {
      res.leaderDupl[i] += pis[i];
    }
  }
  for (size_t i = 0; i < dbServers.size(); ++i) {
    res.imbalance +=
        pow(res.weightUsed[i] - res.targetWeight[i], 2.0) + res.leaderDupl[i];
  }
  return res;
}

int AutoRebalanceProblem::applyMoveShardJob(MoveShardJob const& job,
                                            bool dryRun,
                                            ShardImbalance* shardImb,
                                            LeaderImbalance* leaderImb) {
  // This method applies a MoveShardJob to the current situation in the
  // object, if the pointers `shardImb` and/or `leaderImb` are non-NULL,
  // it is assumed that both point to structures describing the actually
  // imbalances of the current situation and this method changes them
  // to reflect the new situation after the MoveShardJob. If the `dryRun`
  // flag is set, then the current situation is not actually changed,
  // only the consequences for *shardImb and *leaderImb are computed.

  // Check a few things beforehand:
  if (job.shardId >= shards.size()) {
    return -1;
  }
  if (job.from >= dbServers.size() || job.to >= dbServers.size()) {
    return -2;
  }
  if (job.to == job.from) {
    return -3;
  }
  auto& shard = shards[job.shardId];

  auto adjustShardImbalances = [&]() {
    // This is called from both the leader and the follower case!
    if (shardImb != nullptr) {
      // Take out the contributions of the imbalances for the two servers:
      shardImb->imbalance -= pow(
          shardImb->sizeUsed[job.from] - shardImb->targetSize[job.from], 2.0);
      shardImb->imbalance -=
          pow(shardImb->sizeUsed[job.to] - shardImb->targetSize[job.to], 2.0);
      shardImb->sizeUsed[job.from] -= shard.size;
      shardImb->sizeUsed[job.to] += shard.size;
      shardImb->imbalance += pow(
          shardImb->sizeUsed[job.from] - shardImb->targetSize[job.from], 2.0);
      shardImb->imbalance +=
          pow(shardImb->sizeUsed[job.to] - shardImb->targetSize[job.to], 2.0);
    }
  };

  if (job.isLeader) {
    bool hasToMoveData = false;
    if (shard.leader != job.from) {
      return -4;
    }
    auto it = std::find(shard.followers.begin(), shard.followers.end(), job.to);
    if (it == shard.followers.end()) {
      hasToMoveData = true;
    }

    auto work = [&]() -> void {
      shard.leader = job.to;
      if (!hasToMoveData) {
        *it = job.from;
      }
    };  // will be executed later halfway through the adjustments of the
        // imbalances!
    auto undo = [&]() -> void {
      shard.leader = job.from;
      if (!hasToMoveData) {
        *it = job.to;
      }
    };

    if (leaderImb != nullptr) {
      // Need to adjust imbalances:
      // Note that shard imbalances are not affected at all, since no data is
      // actually being moved. So we only have to take care of the leader
      // distribution, the only change is the moved leader. This affects the
      // leader imbalance and the piCoefficients on the two distinct servers
      // job.from and job.to: First the contribution from the weights:
      leaderImb->imbalance -= pow(
          leaderImb->weightUsed[job.from] - leaderImb->targetWeight[job.from],
          2.0);
      leaderImb->imbalance -= pow(
          leaderImb->weightUsed[job.to] - leaderImb->targetWeight[job.to], 2.0);
      leaderImb->weightUsed[job.from] -= shard.weight;
      leaderImb->weightUsed[job.to] += shard.weight;
      leaderImb->imbalance += pow(
          leaderImb->weightUsed[job.from] - leaderImb->targetWeight[job.from],
          2.0);
      leaderImb->imbalance += pow(
          leaderImb->weightUsed[job.to] - leaderImb->targetWeight[job.to], 2.0);

      // Now the contribution from the piCoefficients, we take the contribution
      // for the collection of this shard out, move the shard, and then put it
      // in again:
      leaderImb->imbalance -= leaderImb->leaderDupl[job.from];
      leaderImb->imbalance -= leaderImb->leaderDupl[job.to];
      Collection const& coll = collections[shard.collectionId];
      std::vector<double> pis = piCoefficients(coll);
      leaderImb->leaderDupl[job.from] -= pis[job.from];
      leaderImb->leaderDupl[job.to] -= pis[job.to];
      work();  // this moves the leader for this particular shard
      pis = piCoefficients(coll);  // recompute for this collection
      leaderImb->leaderDupl[job.from] += pis[job.from];
      leaderImb->leaderDupl[job.to] += pis[job.to];
      leaderImb->imbalance += leaderImb->leaderDupl[job.from];
      leaderImb->imbalance += leaderImb->leaderDupl[job.to];
      if (dryRun) {
        undo();
      }
    } else {
      if (!dryRun) {
        work();
      }
    }
    if (hasToMoveData) {
      adjustShardImbalances();
    }
  } else {
    if (shard.leader == job.from) {
      return -6;
    }
    if (shard.leader == job.to) {
      return -7;
    }
    auto it =
        std::find(shard.followers.begin(), shard.followers.end(), job.from);
    if (it == shard.followers.end()) {
      return -8;
    }
    auto it2 =
        std::find(shard.followers.begin(), shard.followers.end(), job.to);
    if (it2 != shard.followers.end()) {
      return -9;
    }

    if (!dryRun) {
      *it = job.to;
    }

    // Need to adjust imbalances:
    // Note that leader imbalances are not affected at all, since no
    // leaders have actually been changed. So we only have to take care
    // of the shard distribution, the only change is done to the one
    // moved shard. This affects the amount of data stored on the two
    // different servers `job.from` and `job.to`.
    adjustShardImbalances();
  }

  return 0;
}

std::vector<std::vector<MoveShardJob>>
AutoRebalanceProblem::findAllMoveShardJobs(bool considerLeaderChanges,
                                           bool considerFollowerMoves,
                                           bool considerLeaderMoves) const {
  uint32_t nr = static_cast<uint32_t>(dbServers.size());
  std::vector<MoveShardJob> res;
  std::vector<int> scratch;  // 1 if server is in followers
  scratch.resize(nr, 0);
  // First sort the shards by collection:
  std::vector<size_t> shardIndices;
  shardIndices.reserve(shards.size());
  for (size_t i = 0; i < shards.size(); ++i) {
    shardIndices.push_back(i);
  }
  std::sort(shardIndices.begin(), shardIndices.end(),
            [&](size_t a, size_t b) -> bool {
              return shards[a].collectionId < shards[b].collectionId;
            });

  std::vector<std::vector<MoveShardJob>> res2;
  for (size_t j = 0; j < shardIndices.size(); ++j) {
    size_t shardIndex = shardIndices[j];
    auto const& shard = shards[shardIndex];
    for (size_t i = 0; i < nr; ++i) {
      scratch[i] = 0;
    }
    scratch[shard.leader] = 1;
    if (considerLeaderChanges) {
      for (auto const toserver : shard.followers) {
        if (serversHealthInfo.contains(dbServers[toserver].id)) {
          res.emplace_back(shard.id, shard.leader, toserver, true, false, nr);
          scratch[toserver] = 1;
        }
      }
    }
    if (considerLeaderMoves) {
      for (uint32_t i = 0; i < nr; ++i) {
        if (serversHealthInfo.contains(dbServers[i].id)) {
          if (scratch[i] == 0) {
            res.emplace_back(shard.id, shard.leader, i, true, true, nr);
          }
        }
      }
    }
    if (considerFollowerMoves) {
      for (auto const fromserver : shard.followers) {
        for (uint32_t i = 0; i < nr; ++i) {
          if (serversHealthInfo.contains(dbServers[i].id)) {
            if (scratch[i] == 0) {
              res.emplace_back(shard.id, fromserver, i, false, true, nr);
            }
          }
        }
      }
    }
    if (res.size() > 1000 &&  // not too many jobs at a time!
        j > 0 &&
        shards[shardIndices[j - 1]].collectionId !=
            shards[shardIndices[j]].collectionId) {
      res2.emplace_back(std::move(res));
      res.clear();
    }
  }
  if (res.size() != 0) {
    res2.emplace_back(std::move(res));
  }
  return res2;
}

int AutoRebalanceProblem::optimize(bool considerLeaderChanges,
                                   bool considerFollowerMoves,
                                   bool considerLeaderMoves, size_t atMost,
                                   std::vector<MoveShardJob>& movesResult) {
  // moves will be overwritten!
  auto moveGroups = findAllMoveShardJobs(
      considerLeaderChanges, considerFollowerMoves, considerLeaderMoves);

  // Keep a copy of the current shard distribution to restore later:
  std::vector<Shard> shardsCopy{shards};

  ShardImbalance shardImb = computeShardImbalance();
  LeaderImbalance leaderImb = computeLeaderImbalance();
  for (auto& moves : moveGroups) {
    for (auto& job : moves) {
      job.shardImbAfter = shardImb;    // copy
      job.leaderImbAfter = leaderImb;  // copy
      int res = applyMoveShardJob(job, true /* dry run */, &job.shardImbAfter,
                                  &job.leaderImbAfter);
      if (res != 0) {
        // Should not happen!
        return res;
      }
      job.score = shardImb.imbalance - job.shardImbAfter.imbalance +
                  leaderImb.imbalance - job.leaderImbAfter.imbalance;
      // the higher the score, the better, less imbalance means higher score
    }

    auto descSortPred = [](MoveShardJob const& a,
                           MoveShardJob const& b) -> bool {
      return a.score > b.score;
    };
    std::sort(moves.begin(), moves.end(), descSortPred);

    // Remove those with non-positive score:
    auto descSortCompValue = [](MoveShardJob const& a, double s) -> bool {
      return a.score > s;
    };
    auto it =
        std::lower_bound(moves.begin(), moves.end(), 0, descSortCompValue);
    moves.erase(it, moves.end());

    // Keep at most `atMost` jobs:
    if (moves.size() > atMost) {
      moves.erase(moves.begin() + atMost, moves.end());
    }

    // Now successively apply jobs, reevaluate the rest:
    for (size_t i = 1; i < moves.size(); ++i) {
      // First apply job i-1 and then reevaluate all jobs from 1 on:
      int res = applyMoveShardJob(moves[i - 1], false, &shardImb, &leaderImb);
      if (res != 0) {
        // This should not happen, let's undo all and give up:
        shards = shardsCopy;
        return res;
      }
      // Now check all other jobs:
      size_t k = i;  // write position to move jobs down
      for (size_t j = i; j < moves.size(); ++j) {
        auto& job = moves[j];
        job.shardImbAfter = shardImb;    // copy
        job.leaderImbAfter = leaderImb;  // copy
        int res = applyMoveShardJob(job, true /* dry run */, &job.shardImbAfter,
                                    &job.leaderImbAfter);
        if (res != 0) {
          // if this happens, then this moveShard has become obsolete or plain
          // wrong by previously applied good ones, let's just get rid of it:
          continue;
        }
        job.score = shardImb.imbalance - job.shardImbAfter.imbalance +
                    leaderImb.imbalance - job.leaderImbAfter.imbalance;
        if (j > k) {
          moves[k] = std::move(moves[j]);
        }
        ++k;
      }
      // And erase everything from k to the end:
      moves.erase(moves.begin() + k, moves.end());
      if (k == i) {
        // All remaining jobs gone.
        break;
      }
      // Sort again:
      std::sort(moves.begin() + i, moves.end(), descSortPred);
      // Now the new top job could have a higher score than the previously
      // selected one. However, we must execute jobs in the computed order. To
      // be able to merge jobs later on, we want scores to be descending, so we
      // lower the score of the top job artificially to be equal to the previous
      // one, if this happens:
      if (moves[i - 1].score < moves[i].score) {
        // This is a lie, but leads to the fact that the jobs have descending
        // scores in the end and can be executed in the given order!
        moves[i].score = moves[i - 1].score;
      }
      // And remove bad ones again:
      auto it = std::lower_bound(moves.begin() + i, moves.end(), 0,
                                 descSortCompValue);
      moves.erase(it, moves.end());
    }
  }
  shards = shardsCopy;

  for (auto const& group : moveGroups) {
    for (size_t i = 0; i + 1 < group.size(); ++i) {
      if (group[i].score < group[i + 1].score) {
        std::abort();
      }
    }
  }

  // Now merge the result from the moves of each shard:
  if (moveGroups.size() == 1) {
    movesResult.swap(moveGroups[0]);
    return 0;
  }

  struct Pair {
    size_t group;
    size_t index;
  };
  auto compare = [&moveGroups](Pair const& a, Pair const& b) -> bool {
    return moveGroups[a.group][a.index].score <
           moveGroups[b.group][b.index].score;
  };
  std::priority_queue<Pair, std::vector<Pair>, decltype(compare)> prio(compare);
  size_t sum = 0;
  // Load queue:
  for (size_t i = 0; i < moveGroups.size(); ++i) {
    if (moveGroups[i].size() != 0) {
      TRI_ASSERT(moveGroups[i].size() != 0);
      sum += moveGroups[i].size();
      prio.emplace(Pair{.group = i, .index = 0});
    }
  }
  std::vector<size_t> pos(moveGroups.size(), 1);
  movesResult.clear();
  sum = std::min(atMost, sum);
  movesResult.reserve(sum);
  for (size_t i = 0; i < sum; ++i) {
    Pair top = prio.top();  // intentionally copy!
    prio.pop();
    movesResult.emplace_back(std::move(moveGroups[top.group][top.index]));
    if (pos[top.group] < moveGroups[top.group].size()) {
      prio.emplace(Pair{.group = top.group, .index = pos[top.group]++});
    }
  }
  for (size_t i = 0; i + 1 < movesResult.size(); ++i) {
    if (movesResult[i].score < movesResult[i + 1].score) {
      std::abort();
    }
  }

  return 0;
}

void AutoRebalanceProblem::moveToBuilder(MoveShardJob const& m,
                                         VPackBuilder& mb) const {
  {
    VPackObjectBuilder o(&mb);
    mb.add(
        "database",
        VPackValue(
            databases[collections[shards[m.shardId].collectionId].dbId].name));
    mb.add("collection",
           VPackValue(collections[shards[m.shardId].collectionId].name));
    mb.add("shard", VPackValue(shards[m.shardId].name));
    mb.add("fromServer", VPackValue(dbServers[m.from].id));
    mb.add("toServer", VPackValue(dbServers[m.to].id));
  }
}
