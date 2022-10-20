////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2020 ArangoDB GmbH, Cologne, Germany
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
#include "gtest/gtest.h"

#include "Basics/VelocyPackStringLiteral.h"
#include <Inspection/VPackInspection.h>
#include <Inspection/StatusT.h>

#include <fmt/core.h>

using namespace arangodb::velocypack;
using namespace arangodb::inspection;

TEST(Inspection, statust_test) {
  {
    auto s = StatusT<int>::ok(15);
    EXPECT_TRUE(s.ok());
    EXPECT_EQ(s.get(), 15);
  }

  {
    auto s = StatusT<int>::error(Status("error"));

    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.error(), "error");
  }
}

struct Dummy {
  std::string type;
  size_t id;
};

template<typename Inspector>
auto inspect(Inspector& f, Dummy& x) {
  return f.object(x).fields(f.field("type", x.type), f.field("id", x.id));
}

TEST(Inspection, statust_test_deserialize) {
  auto testSlice = R"({
    "type": "ReturnNode",
    "id": 3
  })"_vpack;

  auto res = deserializeWithStatus<Dummy>(testSlice);

  ASSERT_TRUE(res.ok()) << fmt::format("Something went wrong: {}", res.error());

  EXPECT_EQ(res->type, "ReturnNode");
  EXPECT_EQ(res->id, 3u);
}

TEST(Inspection, statust_test_deserialize_fail) {
  auto testSlice = R"({
    "type": "ReturnNode",
    "id": 3,
    "fehler": 2
  })"_vpack;

  auto res = deserializeWithStatus<Dummy>(testSlice);

  ASSERT_FALSE(res.ok()) << fmt::format("Did not detect the error we exepct");

  EXPECT_EQ(res.error(), "Found unexpected attribute 'fehler'");
}
