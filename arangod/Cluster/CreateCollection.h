////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2023 ArangoDB GmbH, Cologne, Germany
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
/// @author Kaveh Vahedipour
/// @author Matthew Von-Maszewski
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include "ActionBase.h"
#include "ActionDescription.h"
#include "Cluster/ClusterTypes.h"
#include "Replication2/ReplicatedLog/LogId.h"
#include "VocBase/voc-types.h"

#include <chrono>

struct TRI_vocbase_t;

namespace arangodb {

struct ShardID;

namespace replication2::agency {
struct CollectionGroupPlanSpecification;
}  // namespace replication2::agency

namespace maintenance {

class CreateCollection : public ActionBase, public ShardDefinition {
 public:
  CreateCollection(MaintenanceFeature&, ActionDescription const& d);

  virtual ~CreateCollection();

  bool first() override final;

  void setState(ActionState state) override final;

 private:
  bool _doNotIncrement =
      false;  // indicate that `setState` shall not increment the version

  static Result createCollectionReplication2(
      TRI_vocbase_t& vocbase, replication2::LogId logId, ShardID const& shard,
      TRI_col_type_e collectionType, velocypack::SharedSlice properties);

  static Result createCollectionReplication1(TRI_vocbase_t& vocbase,
                                             ShardID const& shard,
                                             TRI_col_type_e collectionType,
                                             VPackSlice properties,
                                             std::string const& leader);

  std::shared_ptr<replication2::agency::CollectionGroupPlanSpecification const>
  getCollectionGroup(VPackSlice props) const;

  static void fillGroupProperties(
      std::shared_ptr<
          replication2::agency::CollectionGroupPlanSpecification const> const&
          group,
      VPackBuilder& builder);

  static replication2::LogId getReplicatedLogId(
      ShardID const& shard,
      std::shared_ptr<
          replication2::agency::CollectionGroupPlanSpecification const> const&
          group,
      VPackSlice props);
};

}  // namespace maintenance
}  // namespace arangodb
