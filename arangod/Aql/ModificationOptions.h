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
/// @author Max Neunhoeffer
////////////////////////////////////////////////////////////////////////////////

#ifndef ARANGOD_AQL_MODIFICATION_OPTIONS_H
#define ARANGOD_AQL_MODIFICATION_OPTIONS_H 1

#include "Basics/Common.h"
#include "Utils/OperationOptions.h"

namespace arangodb {
namespace velocypack {
class Builder;
class Slice;
}

namespace aql {

/// @brief ModificationOptions
struct ModificationOptions {
  /// @brief constructor, using default values
  explicit ModificationOptions(arangodb::velocypack::Slice const&);

  ModificationOptions()
      : overwriteMode(OperationOptions::OverwriteMode::Replace),
        ignoreErrors(false),
        waitForSync(false),
        validate(true),
        nullMeansRemove(false),
        mergeObjects(true),
        ignoreDocumentNotFound(false),
        readCompleteInput(true),
        useIsRestore(false),
        consultAqlWriteFilter(false),
        exclusive(false),
        overwrite(false),
        ignoreRevs(true) {}

  void toVelocyPack(arangodb::velocypack::Builder&) const;

  OperationOptions::OverwriteMode overwriteMode;

  bool ignoreErrors;
  bool waitForSync;
  bool validate;
  bool nullMeansRemove;
  bool mergeObjects;
  bool ignoreDocumentNotFound;
  bool readCompleteInput;
  bool useIsRestore;
  bool consultAqlWriteFilter;
  bool exclusive;
  bool overwrite;
  bool overwriteModeUpdate;
  bool ignoreRevs;
};

}  // namespace aql
}  // namespace arangodb

#endif
