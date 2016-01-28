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
/// @author Dr. Frank Celler
////////////////////////////////////////////////////////////////////////////////

#ifndef ARANGOD_ACTIONS_REST_ACTION_HANDLER_H
#define ARANGOD_ACTIONS_REST_ACTION_HANDLER_H 1

#include "Basics/Common.h"
#include "Actions/actions.h"
#include "RestHandler/RestVocbaseBaseHandler.h"


class TRI_action_t;


namespace arangodb {

////////////////////////////////////////////////////////////////////////////////
/// @brief action request handler
////////////////////////////////////////////////////////////////////////////////

class RestActionHandler : public RestVocbaseBaseHandler {
  
 public:
  //////////////////////////////////////////////////////////////////////////////
  /// @brief constructor options
  //////////////////////////////////////////////////////////////////////////////

  struct action_options_t {
    TRI_vocbase_t* _vocbase;
  };

  
 public:

  RestActionHandler(rest::GeneralRequest*, action_options_t*);

  
 public:

  bool isDirect() const override;


  status_t execute() override;


  bool cancel() override;

  
 private:
  //////////////////////////////////////////////////////////////////////////////
  /// @brief executes an action
  //////////////////////////////////////////////////////////////////////////////

  TRI_action_result_t executeAction();

  
 protected:
  //////////////////////////////////////////////////////////////////////////////
  /// @brief action
  //////////////////////////////////////////////////////////////////////////////

  TRI_action_t* _action;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief data lock
  //////////////////////////////////////////////////////////////////////////////

  basics::Mutex _dataLock;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief data for cancelation
  //////////////////////////////////////////////////////////////////////////////

  void* _data;
};
}

#endif


