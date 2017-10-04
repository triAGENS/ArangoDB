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

#ifndef ARANGOD_REPLICATION_REPLICATION_APPLIER_STATE_H
#define ARANGOD_REPLICATION_REPLICATION_APPLIER_STATE_H 1

#include "Basics/Common.h"
#include "VocBase/replication-common.h"
#include "VocBase/voc-types.h"

#include <velocypack/Builder.h>
#include <velocypack/Value.h>
#include <velocypack/velocypack-aliases.h>

namespace arangodb {

/// @brief state information about replication application
struct ReplicationApplierState {
  enum class ActivityState {
    INACTIVE,
    RUNNING,
    SHUTTING_DOWN
  };

  ReplicationApplierState();
  ~ReplicationApplierState();
  
  ReplicationApplierState& operator=(ReplicationApplierState const& other);

  void reset();
  void toVelocyPack(arangodb::velocypack::Builder& result, bool full) const;
  
  TRI_voc_tick_t _lastProcessedContinuousTick;
  TRI_voc_tick_t _lastAppliedContinuousTick;
  TRI_voc_tick_t _lastAvailableContinuousTick;
  TRI_voc_tick_t _safeResumeTick;
  ActivityState _state;
  bool _preventStart;
  bool _stopInitialSynchronization;
  
  std::string _progressMsg;
  char _progressTime[24];
  TRI_server_id_t _serverId;
    
  bool isRunning() const {
    return (_state == ActivityState::RUNNING);
  }

  bool isShuttingDown() const {
    return (_state == ActivityState::SHUTTING_DOWN);
  }

  void setError(int code, std::string const& msg) {
    _lastError.set(code, msg);
  }

  void clearError() {
    _lastError.reset();
  }

  // last error that occurred during replication
  struct LastError {
    LastError() : code(TRI_ERROR_NO_ERROR), message() { 
      time[0] = '\0'; 
    }

    void reset() {
      code = TRI_ERROR_NO_ERROR;
      message.clear();
      time[0] = '\0';
      TRI_GetTimeStampReplication(time, sizeof(time) - 1);
    }

    void set(int errorCode, std::string const& msg) {
      code = errorCode;
      message = msg;
      time[0] = '\0';
      TRI_GetTimeStampReplication(time, sizeof(time) - 1);
    }

    void toVelocyPack(arangodb::velocypack::Builder& result) const {
      result.add(VPackValue(VPackValueType::Object));
      result.add("errorNum", VPackValue(code));

      if (code > 0) {
        result.add("time", VPackValue(time));
        if (!message.empty()) {
          result.add("errorMessage", VPackValue(message));
        }
      }
      result.close();
    }

    int code;
    std::string message;
    char time[24];
  } _lastError;

  // counters
  uint64_t _failedConnects;
  uint64_t _totalRequests;
  uint64_t _totalFailedConnects;
  uint64_t _totalEvents;
  uint64_t _skippedOperations;
};

}

#endif
