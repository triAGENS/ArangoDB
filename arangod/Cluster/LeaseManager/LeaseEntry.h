////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2024-2024 ArangoDB GmbH, Cologne, Germany
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

#pragma once

#include "Basics/ScopeGuard.h"

namespace arangodb::cluster {

struct LeaseEntry {
  virtual ~LeaseEntry() = default;
  // These are on purpose named differently then ScopeGuard
  // methods to avoid name confusions.
  virtual auto invoke() noexcept -> void = 0;
  virtual auto abort() noexcept -> void = 0;
};

template<typename F>
struct LeaseEntry_Impl : public LeaseEntry, public ScopeGuard<F> {
  static_assert(std::is_nothrow_invocable_r_v<void, F>);
  using ScopeGuard<F>::ScopeGuard;
  auto invoke() noexcept -> void override { ScopeGuard<F>::fire(); }
  auto abort() noexcept -> void override { ScopeGuard<F>::cancel(); }
};

template<class Inspector>
auto inspect(Inspector& f, LeaseEntry& x) {
  return f.object(x).fields();
}
}  // namespace arangodb::cluster