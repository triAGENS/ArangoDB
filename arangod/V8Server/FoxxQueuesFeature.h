////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2016 ArangoDB GmbH, Cologne, Germany
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

#ifndef APPLICATION_FEATURES_FOXX_QUEUES_FEATURE_H
#define APPLICATION_FEATURES_FOXX_QUEUES_FEATURE_H 1

#include "ApplicationFeatures/ApplicationFeature.h"

#include "Basics/ReadWriteLock.h"

namespace arangodb {
namespace basics {

template<class LockType>
class WriteLocker;
}

class FoxxQueuesFeature final : public application_features::ApplicationFeature {
 public:
  explicit FoxxQueuesFeature(application_features::ApplicationServer& server);

  void collectOptions(std::shared_ptr<options::ProgramOptions>) override final;
  void validateOptions(std::shared_ptr<options::ProgramOptions>) override final;

  /// @brief return poll interval for foxx queues. returns a negative number if
  /// foxx queues are turned off
  double pollInterval() const {
    if (!_enabled) {
      return -1.0;
    }
    return _pollInterval;
  }

  // Caller can get this lock
  // Caller needs to make sure to release this lock after use.
  arangodb::basics::ReadWriteLock& fileSystemLock();

 private:
  double _pollInterval;
  bool _enabled;

  arangodb::basics::ReadWriteLock _fileSystemLock;
};

}  // namespace arangodb

#endif
