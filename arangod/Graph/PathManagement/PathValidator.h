////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2020-2021 ArangoDB GmbH, Cologne, Germany
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
/// @author Michael Hackstein
////////////////////////////////////////////////////////////////////////////////

#ifndef ARANGOD_GRAPH_PATH_VALIDATOR_H
#define ARANGOD_GRAPH_PATH_VALIDATOR_H 1

namespace arangodb {
namespace graph {

template <class PathStore>
class PathValidator {
 public:
  PathValidator(PathStore const& store);

  auto track(typename PathStore::Step const& step) -> void;
  auto validatePath(typename PathStore::Step const& step) -> ValidationResult;

 private:
  PathStore const& _store;

  // Only for applied vertex uniqueness
  // TODO: Figure out if we can make this Member template dependend
  //       e.g. std::enable_if<vertexUniqueness != NONE>
  std::unordered_set<typename Step::Vertex> _uniqueVertices;
};
}  // namespace graph
}  // namespace arangodb

#endif