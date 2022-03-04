////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2022 ArangoDB GmbH, Cologne, Germany
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
/// @author Julia Volmer
////////////////////////////////////////////////////////////////////////////////

#include <gtest/gtest.h>
#include "WasmServer/WasmCommon.h"
#include "velocypack/Builder.h"
#include "velocypack/Parser.h"
#include "Basics/ResultT.h"

using namespace arangodb;
using namespace arangodb::wasm;

struct WasmFunctionCreation : public ::testing::Test {
  void expectWasmFunction(std::string&& string, WasmFunction&& wasmFunction) {
    auto result =
        WasmFunction::fromVelocyPack(VPackParser::fromJson(string)->slice());
    EXPECT_EQ(result, ResultT<WasmFunction>{wasmFunction});
  }

  void expectError(std::string&& string) {
    auto result =
        WasmFunction::fromVelocyPack(VPackParser::fromJson(string)->slice());
    EXPECT_TRUE(result.fail());
    EXPECT_EQ(result.errorNumber(), TRI_ERROR_BAD_PARAMETER);
  }
};

TEST_F(WasmFunctionCreation, WasmFunction_is_created_from_velocypack) {
  expectWasmFunction(
      R"({"name": "Anne", "code": "ABC", "isDeterministic": true})",
      WasmFunction{"Anne", "ABC", true});
}

TEST_F(WasmFunctionCreation, uses_false_as_isDeterministic_default) {
  expectWasmFunction(R"({"name": "Anne", "code": "ABC"})",
                     WasmFunction{"Anne", "ABC", false});
}

TEST_F(WasmFunctionCreation, requires_name_field) {
  expectError(R"({"code": "ABC"})");
}

TEST_F(WasmFunctionCreation, requires_code_field) {
  expectError(R"({"name": "test"})");
}

TEST_F(WasmFunctionCreation, requires_json_object) { expectError(R"([])"); }

TEST_F(WasmFunctionCreation, gives_error_for_unknown_key) {
  expectError(R"({"name": "test", "code": "ABC", "banane": 5})");
}

TEST_F(WasmFunctionCreation, expects_name_as_string) {
  expectError(R"({"name": 1, "code": "ysww"})");
}

TEST_F(WasmFunctionCreation, expects_code_as_string) {
  expectError(R"({"name": "some_function", "code": 1})");
}

TEST_F(WasmFunctionCreation, expects_isDeterministic_as_string) {
  expectError(
      R"({"name": "some_function", "code": "some code", "isDeterministic": "ABC"})");
}

TEST(WasmFunctionConversion, converts_to_velocypack) {
  auto velocypack = VPackBuilder();
  WasmFunction{"function_name", "test code", false}.toVelocyPack(velocypack);
  EXPECT_TRUE(velocypack.slice().binaryEquals(
      VPackParser::fromJson(
          R"({"name": "function_name", "code": "test code", "isDeterministic": false})")
          ->slice()));
}
