////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright $YEAR-2021 ArangoDB GmbH, Cologne, Germany
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
/// @author Lars Maier
////////////////////////////////////////////////////////////////////////////////

#pragma once
#include <memory>
#include <ostream>

#include <immer/flex_vector.hpp>
#include <immer/box.hpp>

#include "Logger/LogMacros.h"

namespace arangodb {

struct LoggableValue {
  virtual ~LoggableValue() = default;
  virtual auto operator<<(std::ostream& os) const noexcept -> std::ostream& = 0;
};

template<typename T, const char *N>
struct LogNameValuePair : LoggableValue {
  explicit LogNameValuePair(T t) : value(std::move(t)) {}
  T value;
  auto operator<<(std::ostream& os) const noexcept -> std::ostream& override {
    return os << N << "=" << value;
  }
};

struct LogContext {
  explicit LogContext(LogTopic topic) : topic(std::move(topic)) {}

  template<const char N[], typename T>
  auto with(T&& t) const -> LogContext {
    using S = std::decay_t<T>;
    auto pair = std::make_shared<LogNameValuePair<S, N>>(std::forward<T>(t));
    return LogContext(values.push_back(std::move(pair)), topic);
  }

  auto withTopic(LogTopic newTopic) const {
    return LogContext(values, std::move(newTopic));
  }

  friend auto operator<<(std::ostream& os, LogContext const& ctx) -> std::ostream& {
    os << "[";
    bool first = true;
    for (auto const& v : ctx.values) {
      if (!first) {
        os << ", ";
      }
      v->operator<<(os);
      first = false;
    }
    os << "]";
    return os;
  }

  LogTopic const topic;
  immer::flex_vector<std::shared_ptr<LoggableValue>> const values = {};
 private:
  LogContext(immer::flex_vector<std::shared_ptr<LoggableValue>> values,
                         LogTopic topic)
      : topic(std::move(topic)), values(std::move(values)) {}

};
}

#define LOG_CTX(id, level, ctx) LOG_TOPIC(id, level, (ctx).topic) << (ctx) << " "
#define LOG_CTX_IF(id, level, ctx, cond) LOG_TOPIC_IF(id, level, (ctx).topic, cond) << (ctx) << " "
