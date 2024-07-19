////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2024 ArangoDB GmbH, Cologne, Germany
/// Copyright 2004-2014 triAGENS GmbH, Cologne, Germany
///
/// Licensed under the Business Source License 1.1 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     https://github.com/arangodb/arangodb/blob/devel/LICENSE
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

#include "CollectionMutableProperties.h"

#include "Basics/VelocyPackHelper.h"
#include "VocBase/Validators.h"

#include <absl/strings/str_cat.h>

using namespace arangodb;

[[nodiscard]] auto CollectionMutableProperties::Invariants::isJsonSchema(
    std::optional<arangodb::velocypack::Builder> const& value)
    -> inspection::Status {
  if (value.has_value()) {
    auto const& v = value->slice();
    if (!v.isObject()) {
      return {"Schema description is not an object."};
    }
    if (!v.isEmptyObject()) {
      // Empty object is allowed, but needs no further testing
      // For an object try to generate a validator
      // NOTE: This could be more efficient if the Schema is made inspectable.
      // However, this is a non-performance critical API, and it is not
      // extraordinary expensive.
      try {
        // Not needed, just try to generate it.
        ValidatorJsonSchema unused{v};
      } catch (std::exception const& ex) {
        return {absl::StrCat("Error when building schema: ", ex.what())};
      }
    }
  }
  return inspection::Status::Success();
}

bool CollectionMutableProperties::operator==(
    CollectionMutableProperties const& other) const {
  if (!basics::VelocyPackHelper::equalCorrectly(
          computedValues.slice(), other.computedValues.slice(), true)) {
    return false;
  }
  if (schema.has_value() != other.schema.has_value()) {
    // If one has a schema, the other not, they cannot be equal
    return false;
  }
  if (schema.has_value() && !basics::VelocyPackHelper::equalCorrectly(
                                schema->slice(), other.schema->slice(), true)) {
    // If one has a schema, it follows the other has one too,
    // so they need to be equal.
    return false;
  }

  return true;
}
