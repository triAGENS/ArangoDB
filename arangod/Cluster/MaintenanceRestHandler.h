////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2018 ArangoDB GmbH, Cologne, Germany
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

#ifndef ARANGOD_CLUSTER_MAINTENANCE_REST_HANDLER
#define ARANGOD_CLUSTER_MAINTENANCE_REST_HANDLER 1

#include <velocypack/vpack.h>
#include <velocypack/velocypack-aliases.h>

#include "Cluster/MaintenanceAction.h"
#include "RestHandler/RestBaseHandler.h"

namespace arangodb {

////////////////////////////////////////////////////////////////////////////////
/// @brief Object that directs processing of one user maintenance requests
////////////////////////////////////////////////////////////////////////////////

class MaintenanceRestHandler : public RestBaseHandler {
 public:
  MaintenanceRestHandler(GeneralRequest*, GeneralResponse*);

 public:
  char const* name() const override { return "MaintenanceRestHandler"; }

  bool isDirect() const override;

  /// @brief Performs routing of request to appropriate subroutines
  RestStatus execute() override;


  //
  // Accessors
  //
  /// @brief retrieve parsed action description
  maintenance::ActionDescription_t const & getActionDesc() const {return *_actionDesc;};

  /// @brief retrieve unparsed action properties
  VPackBuilder const & getActionProp() const {return *_actionProp;};

protected:
  /// @brief PUT method adds an Action to the worklist (or executes action directly)
  void putAction();

  /// @brief GET method returns worklist
  void getAction();


  /// @brief internal routine to convert PUT body into _actionDesc and _actionProp
  bool parsePutBody(VPackSlice const & parameters);

protected:
  /// @brief internal method to parse PUT body into decription and properties




protected:
  std::shared_ptr<maintenance::ActionDescription_t> _actionDesc;
  std::shared_ptr<VPackBuilder> _actionProp;


};
}

#endif
