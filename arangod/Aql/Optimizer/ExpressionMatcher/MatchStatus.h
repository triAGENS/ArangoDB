////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2023 ArangoDB GmbH, Cologne, Germany
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
////////////////////////////////////////////////////////////////////////////////
#pragma once

#include <string>
#include <unordered_map>
#include <variant>
#include <optional>

#include <Aql/AstNode.h>

namespace arangodb::aql::expression_matcher {

struct MatchStatus {
  auto isSuccess() const noexcept -> bool { return _errors.empty(); };
  auto isError() const noexcept -> bool { return not _errors.empty(); }

  explicit MatchStatus() = default;
  MatchStatus(MatchStatus&&) = default;
  MatchStatus(MatchStatus const&) = delete;

  auto addError(std::string error) && -> MatchStatus&& {
    _errors.emplace_back(std::move(error));
    return std::move(*this);
  }

  auto addErrorIfError(std::string error) && -> MatchStatus&& {
    if (isError()) {
      _errors.emplace_back(std::move(error));
    }
    return std::move(*this);
  }

  auto combine(MatchStatus&& rhs) && -> MatchStatus&& {
    _errors.insert(std::end(_errors), std::begin(rhs._errors),
                   std::end(rhs._errors));

    return std::move(*this);
  }

  static auto error(std::string message) -> MatchStatus {
    return MatchStatus().addError(std::move(message));
  }
  static auto match() { return MatchStatus(); }

  auto errors() const noexcept -> std::vector<std::string> const& {
    return _errors;
  }

  friend inline auto operator/(MatchStatus&& lhs, std::string error)
      -> MatchStatus&& {
    lhs._errors.emplace_back(std::move(error));
    return std::move(lhs);
  }

  template<typename Func>
  friend inline auto operator|(MatchStatus&& lhs, Func&& func)
      -> MatchStatus&& {
    if (lhs.isError()) {
      return std::move(lhs);
    }
    return std::move(lhs).combine(std::forward<Func>(func)());
  }

 private:
  std::vector<std::string> _errors;
};

auto toStream(std::ostream&, MatchStatus const& result) -> std::ostream&;
auto toString(MatchStatus const& result) -> std::string;

struct MatchResultSingle {
  MatchResultSingle(MatchStatus status, AstNode const* match)
      : status(std::move(status)), match(match) {}
  MatchStatus status;
  AstNode const* match;
};

struct MatchResultMulti {
  MatchResultMulti(MatchStatus status, std::vector<AstNode const*> matches)
      : status(std::move(status)), matches(std::move(matches)) {}
  MatchStatus status;
  std::vector<AstNode const*> matches;
};

}  // namespace arangodb::aql::expression_matcher
