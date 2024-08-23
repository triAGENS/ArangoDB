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
/// @author Michael Hackstein
////////////////////////////////////////////////////////////////////////////////

#include "PlanCollectionEntryReplication2.h"
#include "Inspection/VPack.h"
#include "Replication2/AgencyCollectionSpecificationInspectors.h"
#include "Replication2/ReplicatedLog/AgencyLogSpecification.h"
#include "VocBase/Properties/CreateCollectionBody.h"
#include "VocBase/LogicalCollection.h"

#include <velocypack/Builder.h>
#include <velocypack/Collection.h>

#include "Logger/LogMacros.h"

using namespace arangodb;

namespace {
auto transform(UserInputCollectionProperties col)
    -> replication2::agency::CollectionTargetSpecification {
  // TODO Maybe we can find a better way than this for transformation.
  replication2::agency::CollectionTargetSpecification spec;
  spec.groupId = col.groupId.value();
  spec.mutableProperties = {.computedValues = std::move(col.computedValues),
                            .schema = std::move(col.schema),
                            .cacheEnabled = col.cacheEnabled};
  spec.immutableProperties = {col,
                              col.name,
                              col.isSystem,
                              col.type,
                              col.keyOptions,
                              col.isSmart,
                              col.isDisjoint,
                              col.shardingStrategy.value(),
                              col.shardKeys.value(),
                              col.smartJoinAttribute,
                              col.smartGraphAttribute,
                              col.shadowCollections};
  spec.indexes = CollectionIndexesProperties::defaultIndexesForCollectionType(
      col.getType());
  return spec;
}
}  // namespace

PlanCollectionEntryReplication2::PlanCollectionEntryReplication2(
    UserInputCollectionProperties col)
    : _properties{::transform(std::move(col))} {}

std::string PlanCollectionEntryReplication2::getCID() const {
  TRI_ASSERT(!_properties.immutableProperties.id.empty());
  return std::to_string(_properties.immutableProperties.id.id());
}

std::string const& PlanCollectionEntryReplication2::getName() const {
  TRI_ASSERT(!_properties.immutableProperties.name.empty());
  return {_properties.immutableProperties.name};
}
