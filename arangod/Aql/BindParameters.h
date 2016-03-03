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

#ifndef ARANGOD_AQL_BIND_PARAMETERS_H
#define ARANGOD_AQL_BIND_PARAMETERS_H 1

#include "Basics/Common.h"
#include "Basics/json.h"

namespace arangodb {
namespace velocypack {
class Builder;
class Slice;
}
namespace aql {

typedef std::unordered_map<std::string, std::pair<TRI_json_t const*, bool>>
    BindParametersType;

class BindParameters {
 public:
  BindParameters(BindParameters const&) = delete;
  BindParameters& operator=(BindParameters const&) = delete;
  BindParameters() = delete;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief create the parameters
  //////////////////////////////////////////////////////////////////////////////

  explicit BindParameters(TRI_json_t*);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief destroy the parameters
  //////////////////////////////////////////////////////////////////////////////

  ~BindParameters();

 public:
  //////////////////////////////////////////////////////////////////////////////
  /// @brief return all parameters
  //////////////////////////////////////////////////////////////////////////////

  BindParametersType const& operator()() {
    process();
    return _parameters;
  }

  //////////////////////////////////////////////////////////////////////////////
  /// @brief create a hash value for the bind parameters
  //////////////////////////////////////////////////////////////////////////////

  uint64_t hash() const;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief strip collection name prefixes from the parameters
  /// the values must be a VelocyPack array
  //////////////////////////////////////////////////////////////////////////////

  static arangodb::velocypack::Builder StripCollectionNames(
      arangodb::velocypack::Slice const&, char const*);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief strip collection name prefixes from the parameters
  /// the values must be a JSON array. the array is modified in place
  //////////////////////////////////////////////////////////////////////////////

  static void StripCollectionNames(TRI_json_t*, char const*);

 private:
  //////////////////////////////////////////////////////////////////////////////
  /// @brief process the parameters
  //////////////////////////////////////////////////////////////////////////////

  void process();

 private:
  //////////////////////////////////////////////////////////////////////////////
  /// @brief the parameter json
  //////////////////////////////////////////////////////////////////////////////

  TRI_json_t* _json;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief pointer to collection parameters
  //////////////////////////////////////////////////////////////////////////////

  BindParametersType _parameters;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief internal state
  //////////////////////////////////////////////////////////////////////////////

  bool _processed;
};
}
}

#endif
