////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2018 ArangoDB GmbH, Cologne, Germany
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

#include "AqlItemBlockHelper.h"
#include "gtest/gtest.h"

#include "Aql/AqlItemBlockManager.h"
#include "Aql/InputAqlItemRow.h"
#include "Aql/OutputAqlItemRow.h"
#include "Aql/ShadowAqlItemRow.h"
#include "Basics/VelocyPackHelper.h"

#include <velocypack/Builder.h>
#include <velocypack/Slice.h>
#include <velocypack/velocypack-aliases.h>

using namespace arangodb;
using namespace arangodb::aql;
using namespace arangodb::basics;

namespace arangodb {
namespace tests {
namespace aql {

class AqlShadowItemRowTest : public ::testing::Test {
 protected:
  ResourceMonitor monitor;
  AqlItemBlockManager itemBlockManager{&monitor};

  void AssertResultRow(InputAqlItemRow const& input, VPackSlice result,
                       std::unordered_set<RegisterId> const& regsToIgnore = {}) {
    ASSERT_TRUE(result.isArray());
    ASSERT_TRUE(input.isInitialized());
    ASSERT_EQ(input.getNrRegisters(), static_cast<size_t>(result.length()));
    for (RegisterId i = 0; i < input.getNrRegisters(); ++i) {
      if (regsToIgnore.find(i) == regsToIgnore.end()) {
        auto val = input.getValue(i);
        ASSERT_TRUE(VelocyPackHelper::equal(val.slice(), result.at(i), false))
            << "Comparing failed on entry " << i << " reason: " << val.slice().toJson()
            << " is not equal to: " << result.at(i).toJson();
      }
    }
  }
};

TEST_F(AqlShadowItemRowTest, inject_new_shadow_rows) {
  // Make sure this data is cleared before the assertions
  auto inputBlock =
      buildBlock<3>(itemBlockManager, {{{{1}, {2}, {3}}},
                                       {{{4}, {5}, {6}}},
                                       {{{"\"a\""}, {"\"b\""}, {"\"c\""}}}});
  SharedAqlItemBlockPtr outputBlock{new AqlItemBlock(itemBlockManager, 6, 3)};
  // We do not add or remove anything, just move
  auto outputRegisters = std::make_shared<const std::unordered_set<RegisterId>>(
      std::initializer_list<RegisterId>{});
  auto registersToKeep = std::make_shared<const std::unordered_set<RegisterId>>(
      std::initializer_list<RegisterId>{0, 1, 2});
  auto registersToClear = std::make_shared<const std::unordered_set<RegisterId>>(
      std::initializer_list<RegisterId>{});
  OutputAqlItemRow testee(std::move(outputBlock), outputRegisters,
                          registersToKeep, registersToClear);
  {
    // Let this go out of scope before assertions, to make sure no references are bound here.
    for (size_t rowIdx = 0; rowIdx < inputBlock->size(); ++rowIdx) {
      ASSERT_FALSE(testee.isFull());
      // simply copy over every row, and insert a shadowRow after it
      InputAqlItemRow source{inputBlock, rowIdx};
      testee.copyRow(source);
      ASSERT_TRUE(testee.produced());
      ASSERT_FALSE(testee.isFull());
      testee.advanceRow();
      testee.createShadowRow(source);
      ASSERT_TRUE(testee.produced());
      testee.advanceRow();
    }
    ASSERT_TRUE(testee.isFull());
  }
  ASSERT_EQ(testee.numRowsWritten(), 6);
  outputBlock = testee.stealBlock();
  ASSERT_EQ(outputBlock->size(), 6);
  auto expected =
      VPackParser::fromJson("[[1,2,3],[4,5,6],[\"a\",\"b\",\"c\"]]");
  for (size_t rowIdx = 0; rowIdx < outputBlock->size(); ++rowIdx) {
    if (rowIdx % 2 == 0) {
      // Data Row Case
      ASSERT_FALSE(outputBlock->isShadowRow(rowIdx));
      InputAqlItemRow testResult{outputBlock, rowIdx};
      AssertResultRow(testResult, expected->slice().at(rowIdx / 2));
    } else {
      // ShadowRowCase
      ASSERT_TRUE(outputBlock->isShadowRow(rowIdx));
      ShadowAqlItemRow testResult{outputBlock, rowIdx};
      ASSERT_TRUE(testResult.isRelevant());
    }
  }
}

}  // namespace aql
}  // namespace tests
}  // namespace arangodb