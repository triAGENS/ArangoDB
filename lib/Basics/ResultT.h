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
/// @author Tobias Gödderz
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include <type_traits>

#include <optional>
#include <variant>

#include "Basics/Result.h"
#include "Basics/debugging.h"
#include "Basics/voc-errors.h"
#include "Inspection/Types.h"

#if defined(__GNUC__) && !defined(__clang__) && (__GNUC__ < 11)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif

namespace arangodb {

// @brief Extension of Result which, on success, contains a value of type T.
//
// @details
// A `ResultT x` is expected to hold a value *if and only if* x.ok()!
// So this behaves more like a variant, even if it always contains a Result.
// This is to easily obtain compatibility with existing Result objects.
//
// A successful ResultT can be explicitly created via
//   ResultT::success(value);
// and an erroneous using
//   ResultT::error(TRI_SOME_ERROR)
// or
//   ResultT::error(TRI_ANOTHER_ERROR, "a custom message")
// . Never pass TRI_ERROR_NO_ERROR (i.e. 0) here! Use ::success() for that.
//
// Successful construction is also allowed via the constructor(s)
//  ResultT(T val)
// while error construction is allowed via the constructor(s)
//  ResultT(Result other)
// . Both of those allow implicit conversion, so if you have a function
// returning `ResultT<SomeType>` and a `SomeType value` you can use
//   return value;
// or, having a `Result result` with result.fail() being true,
//   return result;
// .
// Some implicit conversions are disabled when they could cause ambiguity.
template<typename T>
class ResultT {
 public:
  ResultT static success(T const& val) {
    return ResultT(val, TRI_ERROR_NO_ERROR);
  }

  ResultT static success(T&& val) {
    return ResultT(std::move(val), TRI_ERROR_NO_ERROR);
  }

  ResultT static error(ErrorCode errorNumber) {
    return ResultT(std::nullopt, errorNumber);
  }

  ResultT static error(ErrorCode errorNumber,
                       std::string_view const& errorMessage) {
    return ResultT(std::nullopt, errorNumber, errorMessage);
  }

  ResultT static error(ErrorCode errorNumber, std::string&& errorMessage) {
    return ResultT(std::nullopt, errorNumber, std::move(errorMessage));
  }

  ResultT static error(ErrorCode errorNumber, const char* errorMessage) {
    return ResultT(std::nullopt, errorNumber, errorMessage);
  }

  ResultT static error(Result const& other) {
    TRI_ASSERT(other.fail());
    return ResultT(std::nullopt, other);
  }

  ResultT static error(Result&& other) {
    TRI_ASSERT(other.fail());
    return ResultT(std::nullopt, std::move(other));
  }

  // This is disabled if U is implicitly convertible to Result
  // (e.g., if U = int) to avoid ambiguous construction.
  // Use ::success() or ::error() instead in that case.
  template<typename U = T,
           typename = std::enable_if_t<!std::is_convertible<U, Result>::value>>
  // This is not explicit on purpose
  // NOLINTNEXTLINE(google-explicit-constructor)
  // cppcheck-suppress noExplicitConstructor
  /* implicit */ ResultT(Result const& other) : _result(other) {
    // .ok() is not allowed here, as _val should be expected to be initialized
    // iff .ok() is true.
    TRI_ASSERT(_result.fail());
  }

  // This is disabled if U is implicitly convertible to Result
  // (e.g., if U = int) to avoid ambiguous construction.
  // Use ::success() or ::error() instead in that case.
  template<typename U = T,
           typename = std::enable_if_t<!std::is_convertible<U, Result>::value>>
  // This is not explicit on purpose
  // NOLINTNEXTLINE(google-explicit-constructor)
  // cppcheck-suppress noExplicitConstructor
  /* implicit */ ResultT(Result&& other) : _result(std::move(other)) {
    // .ok() is not allowed here, as _val should be expected to be initialized
    // iff .ok() is true.
    TRI_ASSERT(_result.fail());
  }

  // This is disabled if U is implicitly convertible to Result
  // (e.g., if U = int) to avoid ambiguous construction.
  // Use ::success() or ::error() instead in that case.
  template<typename U = T,
           typename = std::enable_if_t<!std::is_convertible<U, Result>::value>>
  // This is not explicit on purpose
  // NOLINTNEXTLINE(google-explicit-constructor)
  // cppcheck-suppress noExplicitConstructor
  /* implicit */ ResultT(T&& val)
      : ResultT(std::move(val), TRI_ERROR_NO_ERROR) {}

  // This is disabled if U is implicitly convertible to Result
  // (e.g., if U = int) to avoid ambiguous construction.
  // Use ::success() or ::error() instead in that case.
  template<typename U = T,
           typename = std::enable_if_t<!std::is_convertible<U, Result>::value>>
  // This is not explicit on purpose
  // NOLINTNEXTLINE(google-explicit-constructor)
  // cppcheck-suppress noExplicitConstructor
  /* implicit */ ResultT(T const& val) : ResultT(val, TRI_ERROR_NO_ERROR) {}

  template<typename U>
  ResultT(U&& val) requires(std::is_convertible_v<U, T> &&
                            !std::is_same_v<std::decay_t<U>, std::nullopt_t> &&
                            !std::is_convertible_v<U, Result>)
      : ResultT(std::forward<U>(val), TRI_ERROR_NO_ERROR) {
    TRI_ASSERT(_val.has_value());
  }

  // Default constructor calls default constructor of T
  // If T is not default constructable, ResultT cannot be default constructable
  ResultT() requires(std::is_nothrow_default_constructible_v<T>) : _val{T{}} {}
  ResultT() requires(!std::is_nothrow_default_constructible_v<T>) = delete;

  ResultT& operator=(T const& val_) requires(std::is_copy_constructible_v<T>) {
    _val = val_;
    return *this;
  }

  ResultT& operator=(T&& val_) {
    _val = std::move(val_);
    return *this;
  }

  // These would be very convenient, but also make it very easy to accidentally
  // use the value of an error-result. So don't add them.
  //
  //  operator T() const { return get(); }
  //  operator T &() { return get(); }

  T* operator->() { return &get(); }

  T const* operator->() const { return &get(); }

  T& operator*() & { return get(); }

  T const& operator*() const& { return get(); }

  T&& operator*() && { return std::move(get()); }

  T const&& operator*() const&& { return get(); }

  // Allow convenient check to allow for code like
  //   if (res) { /* use res.get() */ } else { /* handle error res.result() */ }
  // . Is disabled for bools to avoid accidental confusion of
  //   if (res)
  // with
  //   if (res.get())
  // .
  template<typename U = T,
           typename = std::enable_if_t<!std::is_same<U, bool>::value>>
  explicit operator bool() const {
    return ok();
  }

  T const& get() const { return _val.value(); }

  T& get() { return _val.value(); }

  ResultT map(ResultT<T> (*fun)(T const& val)) const {
    if (ok()) {
      return ResultT<T>::success(fun(get()));
    }

    return *this;
  }

  bool operator==(ResultT<T> const& other) const {
    if (this->ok() && other.ok()) {
      return this->get() == other.get();
    }
    if (this->fail() && other.fail()) {
      return this->errorNumber() == other.errorNumber() &&
             this->errorMessage() == other.errorMessage();
    }

    return false;
  }

  template<typename U>
  bool operator==(ResultT<U> const& other) const {
    if (this->fail() && other.fail()) {
      return this->errorNumber() == other.errorNumber() &&
             this->errorMessage() == other.errorMessage();
    }

    return false;
  }

  // forwarded methods
  bool ok() const { return _result.ok(); }
  bool fail() const { return _result.fail(); }
  bool is(ErrorCode code) { return _result.is(code); }
  ErrorCode errorNumber() const { return _result.errorNumber(); }
  [[nodiscard]] std::string_view errorMessage() const& {
    return _result.errorMessage();
  }
  [[nodiscard]] std::string errorMessage() && {
    return std::move(_result).errorMessage();
  }

  // access methods
  Result const& result() const& { return _result; }
  Result result() && { return std::move(_result); }

 protected:
  Result _result;
  std::optional<T> _val;

  ResultT(std::optional<T>&& val_, ErrorCode errorNumber)
      : _result(errorNumber), _val(std::move(val_)) {}

  ResultT(std::optional<T>&& val_, ErrorCode errorNumber,
          std::string_view const& errorMessage)
      : _result(errorNumber, errorMessage), _val(std::move(val_)) {}

  ResultT(std::optional<T>&& val_, ErrorCode errorNumber,
          std::string&& errorMessage)
      : _result(errorNumber, std::move(errorMessage)), _val(std::move(val_)) {}

  ResultT(std::optional<T>&& val_, ErrorCode errorNumber,
          const char* errorMessage)
      : _result(errorNumber, errorMessage), _val(std::move(val_)) {}

  ResultT(std::optional<T> const& val_, ErrorCode errorNumber)
      : _result(errorNumber), _val(std::move(val_)) {}

  ResultT(std::optional<T> const& val_, ErrorCode errorNumber,
          std::string_view const& errorMessage)
      : _result(errorNumber, errorMessage), _val(val_) {}

  ResultT(std::optional<T> const& val_, ErrorCode errorNumber,
          std::string&& errorMessage)
      : _result(errorNumber, std::move(errorMessage)), _val(val_) {}

  ResultT(std::optional<T>&& val_, Result const& result)
      : _result(result), _val(std::move(val_)) {}

  ResultT(std::optional<T>&& val_, Result&& result)
      : _result(std::move(result)), _val(std::move(val_)) {}
};

namespace detail {
template<typename T>
struct ResultTSerializer
    : std::variant<std::reference_wrapper<const arangodb::Result>,
                   std::reference_wrapper<T>> {};
template<typename Inspector, typename T>
auto inspect(Inspector& f, ResultTSerializer<T>& x) {
  return f.variant(x).unqualified().alternatives(
      arangodb::inspection::type<std::reference_wrapper<T>>("value"),
      arangodb::inspection::type<
          std::reference_wrapper<const arangodb::Result>>("error"));
}
template<typename T>
struct ResultTDeserializer : std::variant<arangodb::Result, T> {};
template<typename Inspector, typename T>
auto inspect(Inspector& f, ResultTDeserializer<T>& x) {
  return f.variant(x).unqualified().alternatives(
      arangodb::inspection::type<T>("value"),
      arangodb::inspection::type<arangodb::Result>("error"));
}
}  // namespace detail
template<typename Inspector, typename T>
auto inspect(Inspector& f, arangodb::ResultT<T>& x) {
  if constexpr (Inspector::isLoading) {
    auto v = detail::ResultTDeserializer<T>{};
    auto res = f.apply(v);
    if (res.ok()) {
      if (std::holds_alternative<arangodb::Result>(v)) {
        x = std::get<arangodb::Result>(std::move(v));
      } else {
        x = std::get<T>(std::move(v));
      }
    }
    return res;
  } else {
    auto variant = [&]() -> detail::ResultTSerializer<T> {
      if (x.fail()) {
        return {std::ref(x.result())};
      }
      return {std::ref(x.get())};
    }();
    return f.apply(variant);
  }
}

}  // namespace arangodb

#if defined(__GNUC__) && !defined(__clang__) && (__GNUC__ < 11)
#pragma GCC diagnostic pop
#endif
