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
#include <memory>

#include <Aql/AstNode.h>

namespace arangodb::aql::expression_matcher {
// struct MatchResult;
// using SubMatch = std::unique_ptr<MatchResult>;

struct MatchResult {
  auto isSuccess() const noexcept -> bool { return _errors.empty(); };
  auto isError() const noexcept -> bool { return not _errors.empty(); }

  struct ErrorTag {};
  struct ValueTag {};
  explicit MatchResult() {}
  explicit MatchResult(ErrorTag, std::string message) : _errors({message}) {}
  explicit MatchResult(ValueTag, std::string key, AstNode* value)
      : _matches({{key, value}}) {}

  explicit MatchResult(ErrorTag, MatchResult&& res, std::string message)
      : MatchResult(std::move(res)) {
    _errors.emplace_back(message);
  }
  explicit MatchResult(ValueTag, MatchResult&& res, std::string key,
                       AstNode* value)
      : MatchResult(std::move(res)) {
    _matches.emplace(key, value);
  }

  explicit MatchResult(MatchResult&& left, MatchResult&& other)
      : MatchResult(std::move(left)) {
    _errors.insert(std::end(_errors), std::begin(other._errors),
                   std::end(other._errors));
    _matches.merge(other._matches);
  }

  MatchResult(MatchResult&&) = default;
  MatchResult(MatchResult const&) = delete;

  static auto error(std::string message) -> MatchResult {
    return MatchResult(ErrorTag{}, message);
  }
  static auto error(MatchResult&& res, std::string message) -> MatchResult {
    return MatchResult(ErrorTag{}, std::move(res), message);
  }
  static auto match(std::string key, AstNode* value) {
    return MatchResult(ValueTag{}, key, value);
  }
  static auto match() { return MatchResult(); }

  auto errors() const noexcept -> std::vector<std::string> const& {
    return _errors;
  }
  auto matches() const noexcept
      -> std::unordered_map<std::string, AstNode*> const& {
    return _matches;
  }

  auto combine(MatchResult&& other) -> MatchResult&& {
    _errors.insert(std::end(_errors), std::begin(other._errors),
                   std::end(other._errors));
    _matches.merge(other._matches);
    return std::move(*this);
  }

 private:
  std::unordered_map<std::string, AstNode*> _matches;
  std::vector<std::string> _errors;
  //  std::vector<SubMatch> submatches;
};

auto toStream(std::ostream&, MatchResult const& result) -> std::ostream&;
auto toString(MatchResult const& result) -> std::string;
}  // namespace arangodb::aql::expression_matcher
