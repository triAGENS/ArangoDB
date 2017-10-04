////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2016 ArangoDB GmbH, Cologne, Germany
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

#ifndef ARANGOD_REPLICATION_GLOBAL_REPLICATION_APPLIER_H
#define ARANGOD_REPLICATION_GLOBAL_REPLICATION_APPLIER_H 1

#include "Basics/Common.h"
#include "Replication/ReplicationApplier.h"

namespace arangodb {

/// @brief server-global replication applier for all databases
class GlobalReplicationApplier final : public ReplicationApplier {
  friend class GlobalTailingSyncer;
 public:
  explicit GlobalReplicationApplier(ReplicationApplierConfiguration const& configuration);
  
  ~GlobalReplicationApplier();
  
  /// @brief execute the check condition
  bool applies() const override { return true; }

  /// @brief stop the applier and "forget" everything
  void forget() override;

  /// @brief configure the replication applier
  void reconfigure(ReplicationApplierConfiguration const& configuration) override;
  
  /// @brief save the replication application configuration to a file
  void storeConfiguration(bool doSync) override;

  /// @brief remove the replication application state file
  void removeState() override;
  
  /// @brief load the applier state from persistent storage
  /// returns whether a previous state was found
  bool loadState() override;
  
  /// @brief store the applier state in persistent storage
  void persistState(bool doSync) override;
 
  /// @brief store the current applier state in the passed vpack builder 
  void toVelocyPack(arangodb::velocypack::Builder& result) const override;
  
 protected:
  std::unique_ptr<TailingSyncer> buildSyncer(TRI_voc_tick_t initialTick, bool useTick, TRI_voc_tick_t barrierId) override;

  std::string getStateFilename() const override;
};

}

#endif
