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
/// @author Alexey Bakharew
////////////////////////////////////////////////////////////////////////////////

#include "gtest/gtest.h"

#include "fakeit.hpp"

#include <vector>

#include "Aql/AstNode.h"
#include "Aql/ExpressionContext.h"
#include "Aql/Function.h"
#include "Aql/Functions.h"
#include "Containers/SmallVector.h"
#include "Transaction/Context.h"
#include "Transaction/Methods.h"
#include "IResearch/IResearchQueryCommon.h"
#include <velocypack/Builder.h>
#include <velocypack/Iterator.h>
#include <velocypack/Parser.h>
#include <velocypack/Slice.h>
#include <velocypack/velocypack-aliases.h>

using namespace arangodb;
using namespace arangodb::aql;
using namespace arangodb::containers;

namespace {


// helper function
SmallVector<AqlValue> create_arg_vec(const VPackSlice slice) {

  SmallVector<AqlValue>::allocator_type::arena_type arena;
  SmallVector<AqlValue> params{arena};

  for (const auto arg : VPackArrayIterator(slice)) {
    params.emplace_back(AqlValue(arg));
  }

  return params;
}

void expect_eq_slices(const VPackSlice actual_slice, const VPackSlice expected_slice) {
  if (actual_slice.isArray() && expected_slice.isArray()) {
    VPackValueLength actual_size = actual_slice.length();
    VPackValueLength expected_size = actual_slice.length();
    ASSERT_EQ(actual_size, expected_size);

    double lhs, rhs;
    for(VPackValueLength i = 0; i < actual_size; ++i) {
      lhs = actual_slice.at(i).getNumber<decltype (lhs)>();
      rhs = actual_slice.at(i).getNumber<decltype (rhs)>();
      ASSERT_DOUBLE_EQ(lhs, rhs);
    }
  } else if (actual_slice.isNumber() && expected_slice.isNumber()) {
    double lhs = actual_slice.getNumber<decltype (lhs)>();
    double rhs = expected_slice.getNumber<decltype (rhs)>();
    ASSERT_DOUBLE_EQ(lhs, rhs);
  } else {
    ASSERT_TRUE(false);
  }

  return;
}

AqlValue evaluate_gauss(const SmallVector<AqlValue> params) {
  fakeit::Mock<ExpressionContext> expressionContextMock;
  ExpressionContext& expressionContext = expressionContextMock.get();
  fakeit::When(Method(expressionContextMock, registerWarning)).AlwaysDo([](ErrorCode, char const*){ });
  
  VPackOptions options;
  fakeit::Mock<transaction::Context> trxCtxMock;
  fakeit::When(Method(trxCtxMock, getVPackOptions)).AlwaysReturn(&options);
  transaction::Context& trxCtx = trxCtxMock.get();

  fakeit::Mock<transaction::Methods> trxMock;
  fakeit::When(Method(trxMock, transactionContextPtr)).AlwaysReturn(&trxCtx);
  fakeit::When(Method(trxMock, vpackOptions)).AlwaysReturn(options);
  transaction::Methods& trx = trxMock.get();
  
  fakeit::When(Method(expressionContextMock, trx)).AlwaysDo([&trx]() -> transaction::Methods& {
    return trx;
  });

  arangodb::aql::Function f("GAUSS_DECAY", &Functions::GaussDecay);
  arangodb::aql::AstNode node(NODE_TYPE_FCALL);
  node.setData(static_cast<void const*>(&f));

  return Functions::GaussDecay(&expressionContext, node, params);
}

void assertGauss(char const* expected, char const* args) {

  // get slice for expected value
  auto const expected_json = VPackParser::fromJson(expected);
  auto const expected_slice = expected_json->slice();
  ASSERT_TRUE(expected_slice.isArray() || expected_slice.isNumber());

  // get slice for args value
  auto const args_json = VPackParser::fromJson(args);
  auto const args_slice = args_json->slice();
  ASSERT_TRUE(args_slice.isArray());

  // create params vector from args slice
  SmallVector<AqlValue> params = create_arg_vec(args_slice);

  // evaluate
  auto const actual_value = evaluate_gauss(params);
  ASSERT_TRUE(actual_value.isNumber() || actual_value.isArray());

  // check equality
  expect_eq_slices(actual_value.slice(), expected_slice);
  return;
}

void assertGaussFail(char const* args) {
  // get slice for args value
  auto const args_json = VPackParser::fromJson(args);
  auto const args_slice = args_json->slice();
  ASSERT_TRUE(args_slice.isArray());

  // create params vector from args slice
  SmallVector<AqlValue> params = create_arg_vec(args_slice);

  ASSERT_TRUE(evaluate_gauss(params).isNull(false));
}

TEST(GaussDecayFunctionTest, test) {
  assertGauss("0.5", "[20, 40, 5, 5, 0.5]");
  assertGauss("1", "[41, 40, 5, 5, 0.5]");
  assertGauss("[0.5, 1.0]", "[[20.0, 41], 40, 5, 5, 0.5]");
  assertGauss("1.0", "[40, 40, 5, 5, 0.5]");
  assertGauss("1.0", "[49.987, 49.987, 0.001, 0.001, 0.2]");
  assertGauss("0.2715403018822964", "[49.9889, 49.987, 0.001, 0.001, 0.2]");
  assertGauss("0.1", "[-10, 40, 5, 0, 0.1]");
  assertGaussFail("[30, 40, 5]");
  assertGaussFail("[30, 40, 5, 100]");
  assertGaussFail("[30, 40, 5, 100, -100]");
}

} // namespase
