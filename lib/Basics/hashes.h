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
/// @author Dr. Frank Celler
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include <cstdint>
#include <cstddef>
#include <type_traits>

namespace arangodb {

/// @brief computes a FNV hash for blocks
uint64_t FnvHashBlock(uint64_t hash, void const* buffer,
                      size_t length) noexcept;

/// @brief computes a FNV hash for memory blobs
uint64_t FnvHashPointer(void const* buffer, size_t length) noexcept;

/// @brief computes a FNV hash for strings
uint64_t FnvHashString(char const* buffer) noexcept;

/// @brief computes a FNV hash for POD types
template<typename T>
std::enable_if_t<std::has_unique_object_representations_v<T>, uint64_t>
FnvHashPod(T input) noexcept {
  return FnvHashBlock(0xcbf29ce484222325ULL, &input, sizeof(T));
}

/// @brief computes a initial FNV for blocks
inline constexpr uint64_t kFnvHashBlockInitial = 0xcbf29ce484222325ULL;

}  // namespace arangodb
