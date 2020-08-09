////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2020 ArangoDB GmbH, Cologne, Germany
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
/// @author Tobias Gödderz
////////////////////////////////////////////////////////////////////////////////

#include "Aql/AqlCall.h"
#include "Aql/AqlCallStack.h"
#include "Aql/AqlExecuteResult.h"
#include "Aql/AqlItemBlockManager.h"

#include "AqlItemBlockHelper.h"

#include "gtest/gtest.h"

#include <velocypack/Builder.h>

using namespace arangodb;
using namespace arangodb::aql;

namespace arangodb::aql {

auto operator==(AqlCallStack const& leftC, AqlCallStack const& rightC) -> bool {
  auto left = leftC;
  auto right = rightC;

  while (!left.empty() && !right.empty()) {
    auto const l = left.popCall();
    auto const r = right.popCall();
    if (!(l == r)) {
      return false;
    }
  }

  return left.empty() && right.empty();
}

auto operator==(AqlExecuteResult const& left, AqlExecuteResult const& right) -> bool {
  return left.state() == right.state() && left.skipped() == right.skipped() &&
         (left.block() == nullptr) == (right.block() == nullptr) &&
         (left.block() == nullptr || *left.block() == *right.block());
}

}  // namespace arangodb::aql

namespace arangodb::tests::aql {

auto blockToString(SharedAqlItemBlockPtr const& block) -> std::string {
  velocypack::Builder blockBuilder;
  block->toSimpleVPack(&velocypack::Options::Defaults, blockBuilder);
  return blockBuilder.toJson();
}

class DeSerializeAqlCallTest : public ::testing::TestWithParam<AqlCall> {
 public:
  DeSerializeAqlCallTest() = default;

  void SetUp() override { aqlCall = GetParam(); }

 protected:
  AqlCall aqlCall{};
};

auto const testingAqlCalls = ::testing::ValuesIn(std::array{
    AqlCall{0, false, AqlCall::Infinity{}},
    AqlCall{3, false, AqlCall::Infinity{}},
    AqlCall{0, false, 7, AqlCall::LimitType::SOFT},
    AqlCall{0, false, 7, AqlCall::LimitType::HARD},
    AqlCall{0, true, 7, AqlCall::LimitType::HARD},
    AqlCall{3, false, 7, AqlCall::LimitType::SOFT},
    AqlCall{3, false, 7, AqlCall::LimitType::HARD},
    AqlCall{3, true, 7, AqlCall::LimitType::HARD},
});

TEST_P(DeSerializeAqlCallTest, testSuite) {
  auto builder = velocypack::Builder{};
  aqlCall.toVelocyPack(builder);

  ASSERT_TRUE(builder.isClosed());

  auto const maybeDeSerializedCall = std::invoke([&]() {
    try {
      return AqlCall::fromVelocyPack(builder.slice());
    } catch (std::exception const& ex) {
      EXPECT_TRUE(false) << ex.what();
    }
    return ResultT<AqlCall>::error(-1);
  });

  ASSERT_TRUE(maybeDeSerializedCall.ok()) << maybeDeSerializedCall.errorMessage();

  auto const deSerializedCall = *maybeDeSerializedCall;

  ASSERT_EQ(aqlCall, deSerializedCall);
}

INSTANTIATE_TEST_CASE_P(DeSerializeAqlCallTestVariations, DeSerializeAqlCallTest, testingAqlCalls);

class DeSerializeAqlCallStackTest : public ::testing::TestWithParam<AqlCallStack> {
 public:
  DeSerializeAqlCallStackTest() = default;

  void SetUp() override {
    aqlCallStack = GetParam();
    queryStack = aqlCallStack;
    clientCall = queryStack.popCall();
    TRI_ASSERT(queryStack.subqueryLevel() + 1 == aqlCallStack.subqueryLevel());
  }

 protected:
  AqlCallStack aqlCallStack{};
  AqlCallStack queryStack{};
  AqlCallList clientCall{AqlCall{}};
};

auto const testingAqlCallStacks = ::testing::ValuesIn(std::array{
    AqlCallStack{AqlCallStack::emptyStack(), AqlCallList{AqlCall{}}},
    AqlCallStack{AqlCallStack::emptyStack(), AqlCallList{AqlCall{3, false, AqlCall::Infinity{}}}},
    AqlCallStack{AqlCallStack{AqlCallStack::emptyStack(), AqlCallList{AqlCall{}}},
                 AqlCallList{AqlCall{3, false, AqlCall::Infinity{}}}},
    AqlCallStack{AqlCallStack{AqlCallStack{AqlCallStack::emptyStack(), AqlCallList{AqlCall{1}}}, AqlCallList{AqlCall{2}}},
                 AqlCallList{AqlCall{3}}},
    AqlCallStack{AqlCallStack{AqlCallStack{AqlCallStack::emptyStack(), AqlCallList{AqlCall{3}}}, AqlCallList{AqlCall{2}}},
                 AqlCallList{AqlCall{1}}},
    AqlCallStack{AqlCallStack::emptyStack(), AqlCallList{AqlCall{3, false, AqlCall::Infinity{}}, AqlCall{}}},
    AqlCallStack{AqlCallStack{AqlCallStack::emptyStack(), AqlCallList{AqlCall{}, AqlCall{3, false, AqlCall::Infinity{}}}},
                 AqlCallList{AqlCall{3, false, AqlCall::Infinity{}}, AqlCall{}}}});

TEST_P(DeSerializeAqlCallStackTest, testSuite) {
  auto builder = velocypack::Builder{};
  aqlCallStack.toVelocyPack(builder);

  ASSERT_TRUE(builder.isClosed());
  auto const maybeDeSerializedCallStack = std::invoke([&]() {
    try {
      return AqlCallStack::fromVelocyPack(builder.slice());
    } catch (std::exception const& ex) {
      EXPECT_TRUE(false) << ex.what();
    }
    return ResultT<AqlCallStack>::error(-1);
  });

  ASSERT_TRUE(maybeDeSerializedCallStack.ok())
      << maybeDeSerializedCallStack.errorMessage();

  auto const deSerializedCallStack = *maybeDeSerializedCallStack;

  ASSERT_EQ(aqlCallStack, deSerializedCallStack);
}

INSTANTIATE_TEST_CASE_P(DeSerializeAqlCallStackTestVariations,
                        DeSerializeAqlCallStackTest, testingAqlCallStacks);

class DeSerializeAqlExecuteResultTest : public ::testing::TestWithParam<AqlExecuteResult> {
 public:
  DeSerializeAqlExecuteResultTest() = default;

  void SetUp() override { aqlExecuteResult = GetParam(); }

 protected:
  AqlExecuteResult aqlExecuteResult{ExecutionState::DONE, SkipResult{}, nullptr};
};

auto MakeSkipResult(size_t const i) -> SkipResult {
  SkipResult res{};
  res.didSkip(i);
  return res;
}

TEST(DeSerializeAqlExecuteResultTest, test) {
  
  ResourceMonitor resourceMonitor{};
  AqlItemBlockManager manager{&resourceMonitor, SerializationFormat::SHADOWROWS};

  auto const testingAqlExecuteResults = std::array{
      AqlExecuteResult{ExecutionState::DONE, MakeSkipResult(0), nullptr},
      AqlExecuteResult{ExecutionState::HASMORE, MakeSkipResult(4), nullptr},
      AqlExecuteResult{ExecutionState::DONE, MakeSkipResult(0), buildBlock<1>(manager, {{42}})},
      AqlExecuteResult{ExecutionState::HASMORE, MakeSkipResult(3),
                       buildBlock<2>(manager, {{3, 42}, {4, 41}})},
  };
  
  for (AqlExecuteResult const& aqlExecuteResult : testingAqlExecuteResults) {
    
    velocypack::Builder builder;
    aqlExecuteResult.toVelocyPack(builder, &velocypack::Options::Defaults);

    ASSERT_TRUE(builder.isClosed());

    auto const maybeAqlExecuteResult = std::invoke([&]() {
      try {
        return AqlExecuteResult::fromVelocyPack(builder.slice(), manager);
      } catch (std::exception const& ex) {
        EXPECT_TRUE(false) << ex.what();
      }
      return ResultT<AqlExecuteResult>::error(-1);
    });

    ASSERT_TRUE(maybeAqlExecuteResult.ok()) << maybeAqlExecuteResult.errorMessage();

    auto const deSerializedAqlExecuteResult = *maybeAqlExecuteResult;

    ASSERT_EQ(aqlExecuteResult.state(), deSerializedAqlExecuteResult.state());
    ASSERT_EQ(aqlExecuteResult.skipped(), deSerializedAqlExecuteResult.skipped());
    ASSERT_EQ(aqlExecuteResult.block() == nullptr,
              deSerializedAqlExecuteResult.block() == nullptr);
    if (aqlExecuteResult.block() != nullptr) {
      ASSERT_EQ(*aqlExecuteResult.block(), *deSerializedAqlExecuteResult.block())
          << "left: " << blockToString(aqlExecuteResult.block())
          << "; right: " << blockToString(deSerializedAqlExecuteResult.block());
    }
    ASSERT_EQ(aqlExecuteResult, deSerializedAqlExecuteResult);
  }
}
}  // namespace arangodb::tests::aql
