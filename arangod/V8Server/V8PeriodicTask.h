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

#ifndef ARANGOD_V8_SERVER_V8_PERIODIC_TASK_H
#define ARANGOD_V8_SERVER_V8_PERIODIC_TASK_H 1

#include "Basics/Common.h"
#include "Scheduler/PeriodicTask.h"
#include "VocBase/vocbase.h"

namespace arangodb {
namespace rest {
class Dispatcher;
class Scheduler;
}

class ApplicationV8;

class V8PeriodicTask : public rest::PeriodicTask {
 public:
  V8PeriodicTask(std::string const&, std::string const&, TRI_vocbase_t*,
                 ApplicationV8*, rest::Scheduler*, rest::Dispatcher*, double,
                 double, std::string const&,
                 std::shared_ptr<arangodb::velocypack::Builder>, bool);

  ~V8PeriodicTask();

 protected:
  //////////////////////////////////////////////////////////////////////////////
  /// @brief get a task specific description in VelocyPack format
  //////////////////////////////////////////////////////////////////////////////

  void getDescription(arangodb::velocypack::Builder&) const override;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief whether or not the task is user-defined
  //////////////////////////////////////////////////////////////////////////////

  bool isUserDefined() const override { return true; }

 public:
  //////////////////////////////////////////////////////////////////////////////
  /// @brief handles the next tick
  //////////////////////////////////////////////////////////////////////////////

  bool handlePeriod() override;

 private:
  //////////////////////////////////////////////////////////////////////////////
  /// @brief system vocbase
  //////////////////////////////////////////////////////////////////////////////

  TRI_vocbase_t* _vocbase;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief V8 dealer
  //////////////////////////////////////////////////////////////////////////////

  ApplicationV8* _v8Dealer;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief dispatcher
  //////////////////////////////////////////////////////////////////////////////

  rest::Dispatcher* _dispatcher;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief command to execute
  //////////////////////////////////////////////////////////////////////////////

  std::string const _command;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief paramaters
  //////////////////////////////////////////////////////////////////////////////

  std::shared_ptr<arangodb::velocypack::Builder> _parameters;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief creation timestamp
  //////////////////////////////////////////////////////////////////////////////

  double _created;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief whether or not the task is allowed to switch the database
  //////////////////////////////////////////////////////////////////////////////

  bool _allowUseDatabase;
};
}

#endif
