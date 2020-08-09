////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2019 ArangoDB GmbH, Cologne, Germany
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

#include "AqlCallStack.h"

#include <velocypack/Builder.h>
#include <velocypack/Iterator.h>
#include <velocypack/Slice.h>
#include <velocypack/velocypack-aliases.h>

// TODO: This class is not yet memory efficient or optimized in any way.
// it might be reimplemented soon to have the above features, Focus now is on
// the API we want to use.

using namespace arangodb;
using namespace arangodb::aql;

AqlCallStack AqlCallStack::EmptyStack{false};


AqlCallStack const& AqlCallStack::emptyStack() {
  return AqlCallStack::EmptyStack;
}

AqlCallStack::AqlCallStack(bool compatibilityMode3_6)
    : _operations{}, _compatibilityMode3_6(compatibilityMode3_6) {}

AqlCallStack::AqlCallStack(AqlCallStack const& other, AqlCallList call)
    : _operations{other._operations}, _compatibilityMode3_6{other._compatibilityMode3_6} {
  // We can only use this constructor on relevant levels
  // All others need to use passThrough constructor
  _operations.emplace_back(std::move(call));
  _compatibilityMode3_6 = other._compatibilityMode3_6;
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
  validateNoCallHasSkippedRows();
#endif
}

AqlCallStack::AqlCallStack(std::vector<AqlCallList>&& operations)
    : _operations(std::move(operations)) {
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
  validateNoCallHasSkippedRows();
#endif
}

#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
auto AqlCallStack::validateNoCallHasSkippedRows() -> void {
  for (auto const& list : _operations) {
    TRI_ASSERT(list.peekNextCall().getSkipCount() == 0);
  }
}
#endif

auto AqlCallStack::popCall() -> AqlCallList {
  TRI_ASSERT(_compatibilityMode3_6 || !_operations.empty());
  if (_compatibilityMode3_6 && _operations.empty()) {
    // This is only for compatibility with 3.6
    // there we do not have the stack being passed-through
    // in AQL, we only have a single call.
    // We can only get into this state in the abscence of
    // LIMIT => we always do an unlimted softLimit call
    // to the upwards subquery.
    // => Simply put another fetchAll Call on the stack.
    // This code is to be removed in the next version after 3.7
    _operations.emplace_back(AqlCall{});
  }
  auto call = std::move(_operations.back());
  _operations.pop_back();
  return call;
}

auto AqlCallStack::peek() const -> AqlCall const& {
  TRI_ASSERT(_compatibilityMode3_6 || !_operations.empty());
  if (is36Compatible() && _operations.empty()) {
    // This is only for compatibility with 3.6
    // there we do not have the stack being passed-through
    // in AQL, we only have a single call.
    // We can only get into this state in the abscence of
    // LIMIT => we always do an unlimted softLimit call
    // to the upwards subquery.
    // => Simply put another fetchAll Call on the stack.
    // This code is to be removed in the next version after 3.7
    _operations.emplace_back(AqlCall{});
  }
  TRI_ASSERT(!_operations.empty());
  return _operations.back().peekNextCall();
}

void AqlCallStack::pushCall(AqlCallList&& call) {
  _operations.emplace_back(std::move(call));
}

void AqlCallStack::pushCall(AqlCallList const& call) {
  _operations.emplace_back(call);
}

auto AqlCallStack::fromVelocyPack(velocypack::Slice const slice) -> ResultT<AqlCallStack> {
  if (ADB_UNLIKELY(!slice.isArray())) {
    using namespace std::string_literals;
    return Result(TRI_ERROR_TYPE_ERROR,
                  "When deserializing AqlCallStack: expected array, got "s +
                      slice.typeName());
  }
  if (ADB_UNLIKELY(slice.isEmptyArray())) {
    return Result(TRI_ERROR_TYPE_ERROR,
                  "When deserializing AqlCallStack: stack is empty");
  }

  auto stack = std::vector<AqlCallList>{};
  auto i = std::size_t{0};
  stack.reserve(slice.length());
  for (auto const entry : VPackArrayIterator(slice)) {
    auto maybeAqlCall = AqlCallList::fromVelocyPack(entry);

    if (ADB_UNLIKELY(maybeAqlCall.fail())) {
      auto message = std::string{"When deserializing AqlCallStack: entry "};
      message += std::to_string(i);
      message += ": ";
      message += std::move(maybeAqlCall).errorMessage();
      return Result(TRI_ERROR_TYPE_ERROR, std::move(message));
    }

    stack.emplace_back(maybeAqlCall.get());

    ++i;
  }

  TRI_ASSERT(i > 0);

  return AqlCallStack{std::move(stack)};
}

void AqlCallStack::toVelocyPack(velocypack::Builder& builder) const {
  builder.openArray();
  for (auto const& call : _operations) {
    call.toVelocyPack(builder);
  }
  builder.close();
}

auto AqlCallStack::toString() const -> std::string {
  auto result = std::string{};
  result += "[";
  bool isFirst = true;
  for (auto const& op : _operations) {
    if (!isFirst) {
      result += ",";
    }
    isFirst = false;
    result += " ";
    result += op.toString();
  }
  result += " ]";
  return result;
}

auto AqlCallStack::createEquivalentFetchAllShadowRowsStack() const -> AqlCallStack {
  AqlCallStack res{*this};
  // We can always overfetch the next subquery here.
  // We gonna need all data anyways.
  for (auto& op : res._operations) {
    op.createEquivalentFetchAllRowsCall();
  }
  return res;
}

auto AqlCallStack::needToCountSubquery() const noexcept -> bool {
  return std::any_of(_operations.begin(), _operations.end(), [](AqlCallList const& call) -> bool {
    auto const& nextCall = call.peekNextCall();
    return nextCall.needSkipMore() || nextCall.hasLimit();
  });
}

auto AqlCallStack::needToSkipSubquery() const noexcept -> bool {
  return std::any_of(_operations.begin(), _operations.end(), [](AqlCallList const& call) -> bool {
    auto const& nextCall = call.peekNextCall();
    return nextCall.needSkipMore() || nextCall.hardLimit == 0;
  });
}

auto AqlCallStack::shadowRowDepthToSkip() const -> size_t {
  TRI_ASSERT(needToCountSubquery());
  size_t const n = _operations.size();
  for (size_t i = 0; i < n; ++i) {
    auto& call = _operations[i];
    auto const& nextCall = call.peekNextCall();
     if (nextCall.needSkipMore() || nextCall.getLimit() == 0) {
      return n - i - 1;
    }
  }
  return 0;
}

auto AqlCallStack::modifyCallAtDepth(size_t depth) -> AqlCall& {
  // depth 0 is back of vector
  TRI_ASSERT(_operations.size() > depth);
  // Take the depth-most from top of the vector.
  auto& callList = _operations.at(_operations.size() - 1 - depth);
  return callList.modifyNextCall();
}

auto AqlCallStack::modifyCallListAtDepth(size_t depth) -> AqlCallList& {
  TRI_ASSERT(_operations.size() > depth);
  return _operations.at(_operations.size() - 1 - depth);
}

auto AqlCallStack::getCallListAtDepth(size_t depth) const -> AqlCallList const& {
  TRI_ASSERT(_operations.size() > depth);
  return _operations.at(_operations.size() - 1 - depth);
}

auto AqlCallStack::getCallAtDepth(size_t depth) const -> AqlCall const& {
  // depth 0 is back of vector
  TRI_ASSERT(_operations.size() > depth);
  // Take the depth-most from top of the vector.
  auto& callList = _operations.at(_operations.size() - 1 - depth);
  return callList.peekNextCall();
}

auto AqlCallStack::modifyTopCall() -> AqlCall& {
  TRI_ASSERT(_compatibilityMode3_6 || !_operations.empty());
  if (is36Compatible() && _operations.empty()) {
    // This is only for compatibility with 3.6
    // there we do not have the stack passed-through
    // in AQL, we only have a single call.
    // We can only get into this state in the abscence of
    // LIMIT => we always do an unlimted softLimit call
    // to the upwards subquery.
    // => Simply put another fetchAll Call on the stack.
    // This code is to be removed in the next version after 3.7
    _operations.emplace_back(AqlCall{});
  }
  TRI_ASSERT(!_operations.empty());
  return modifyCallAtDepth(0);
}

auto AqlCallStack::hasAllValidCalls() const noexcept -> bool {
  return std::all_of(_operations.begin(), _operations.end(), [](AqlCallList const& list) {
    if (!list.hasMoreCalls()) {
      return false;
    }
    auto const& nextCall = list.peekNextCall();
    // We cannot continue if any of our calls has a softLimit reached.
    return !(nextCall.hasSoftLimit() && nextCall.getLimit() == 0 && nextCall.getOffset() == 0);
  });
}

auto AqlCallStack::requestLessDataThan(AqlCallStack const& other) const noexcept -> bool {
  if (_operations.size() != other._operations.size()) {
    return false;
  }
  for (size_t i = 0; i < _operations.size(); ++i) {
    if (!_operations[i].requestLessDataThan(other._operations[i])) {
      return false;
    }
  }
  return true;
}
