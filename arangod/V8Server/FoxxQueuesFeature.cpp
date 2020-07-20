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

#include "FoxxQueuesFeature.h"

#include "FeaturePhases/ServerFeaturePhase.h"
#include "ProgramOptions/ProgramOptions.h"

using namespace arangodb::application_features;
using namespace arangodb::options;

namespace arangodb {

FoxxQueuesFeature::FoxxQueuesFeature(application_features::ApplicationServer& server)
    : application_features::ApplicationFeature(server, "FoxxQueues"),
      _pollInterval(1.0),
      _enabled(true) {
  setOptional(true);
  startsAfter<ServerFeaturePhase>();
}

void FoxxQueuesFeature::collectOptions(std::shared_ptr<ProgramOptions> options) {
  options->addSection("foxx", "Configure Foxx");

  options->addOldOption("server.foxx-queues", "foxx.queues");
  options->addOldOption("server.foxx-queues-poll-interval",
                        "foxx.queues-poll-interval");

  options->addOption("--foxx.queues", "enable Foxx queues",
                     new BooleanParameter(&_enabled),
                     arangodb::options::makeFlags(
                     arangodb::options::Flags::DefaultNoComponents,
                     arangodb::options::Flags::OnCoordinator,
                     arangodb::options::Flags::OnSingle));

  options->addOption("--foxx.queues-poll-interval",
                     "poll interval (in seconds) for Foxx queue manager",
                     new DoubleParameter(&_pollInterval),
                     arangodb::options::makeFlags(
                     arangodb::options::Flags::DefaultNoComponents,
                     arangodb::options::Flags::OnCoordinator,
                     arangodb::options::Flags::OnSingle));
}

void FoxxQueuesFeature::validateOptions(std::shared_ptr<ProgramOptions> options) {
  // use a minimum for the interval
  if (_pollInterval < 0.1) {
    _pollInterval = 0.1;
  }
}

basics::ReadWriteLock& FoxxQueuesFeature::fileSystemLock() {
  return _fileSystemLock;
}

}  // namespace arangodb
