////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2022 ArangoDB GmbH, Cologne, Germany
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
/// @author Markus Pfeiffer
////////////////////////////////////////////////////////////////////////////////
#pragma once

namespace arangodb::aql::optimizer2::types {

struct Variable {
  size_t id;
  std::string name;

  bool isFullDocumentFromCollection;
  bool isDataFromCollection;
};

template<typename Inspector>
auto inspect(Inspector& f, Variable& x) {
  return f.object(x).fields(
      f.field("id", x.id), f.field("name", x.name),
      f.field("isFullDocumentFromCollection", x.isFullDocumentFromCollection),
      f.field("isDataFromCollection", x.isDataFromCollection));
}

}  // namespace arangodb::aql::optimizer2::types
