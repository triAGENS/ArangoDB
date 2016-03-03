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

#ifndef ARANGOD_V8_SERVER_V8_JOB_H
#define ARANGOD_V8_SERVER_V8_JOB_H 1

#include "Basics/Common.h"
#include "Basics/json.h"
#include "Dispatcher/Job.h"

struct TRI_vocbase_t;

namespace arangodb {
class ApplicationV8;

class V8Job : public rest::Job {
 private:
  V8Job(V8Job const&) = delete;
  V8Job& operator=(V8Job const&) = delete;

 public:
  //////////////////////////////////////////////////////////////////////////////
  /// @brief constructs a new V8 job
  //////////////////////////////////////////////////////////////////////////////

  V8Job(TRI_vocbase_t*, ApplicationV8*, std::string const&,
        std::shared_ptr<arangodb::velocypack::Builder> const, bool);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief destroys a V8 job
  //////////////////////////////////////////////////////////////////////////////

  ~V8Job();

 public:
  void work() override;

  bool cancel() override;

  void cleanup(rest::DispatcherQueue*) override;

  void handleError(basics::Exception const& ex) override;

  virtual std::string const& getName() const override { return _command; }

 private:
  //////////////////////////////////////////////////////////////////////////////
  /// @brief vocbase
  //////////////////////////////////////////////////////////////////////////////

  TRI_vocbase_t* _vocbase;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief V8 dealer
  //////////////////////////////////////////////////////////////////////////////

  ApplicationV8* _v8Dealer;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief the command to execute
  //////////////////////////////////////////////////////////////////////////////

  std::string const _command;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief paramaters
  //////////////////////////////////////////////////////////////////////////////

  std::shared_ptr<arangodb::velocypack::Builder> _parameters;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief cancel flag
  //////////////////////////////////////////////////////////////////////////////

  std::atomic<bool> _canceled;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief whether or not the job is allowed to switch the database
  //////////////////////////////////////////////////////////////////////////////

  bool const _allowUseDatabase;
};
}

#endif
