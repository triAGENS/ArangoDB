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
/// @author Andrei Lobov
////////////////////////////////////////////////////////////////////////////////

#pragma once
#include <utils/attributes.hpp>
#include <utils/bit_utils.hpp>
#include <velocypack/Slice.h>

namespace arangodb {
namespace iresearch {

struct VPackTermAttribute final : irs::attribute {
  static constexpr std::string_view type_name() noexcept {
    return "vpack_term_attribute";
  }

  ::arangodb::velocypack::Slice value;
};

}  // namespace iresearch
}  // namespace arangodb
