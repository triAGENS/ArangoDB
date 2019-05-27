////////////////////////////////////////////////////////////////////////////////
/// @brief test suite for arangodb::cache::FrequencyBuffer
///
/// @file
///
/// DISCLAIMER
///
/// Copyright 2017 ArangoDB GmbH, Cologne, Germany
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
/// @author Daniel H. Larkin
/// @author Copyright 2017, ArangoDB GmbH, Cologne, Germany
////////////////////////////////////////////////////////////////////////////////

#include "Basics/Common.h"
#include "Cache/FrequencyBuffer.h"

#include "gtest/gtest.h"

#include <stdint.h>
#include <memory>

using namespace arangodb::cache;

TEST(CacheFrequencyBufferTest, test_buffer_with_uint8_entries) {
  uint8_t zero = 0;
  uint8_t one = 1;
  uint8_t two = 2;

  // check that default construction is as expected
  ASSERT_TRUE(uint8_t() == zero);

  FrequencyBuffer<uint8_t> buffer(1024);
  ASSERT_TRUE(buffer.memoryUsage() == sizeof(FrequencyBuffer<uint8_t>) + 1024);

  for (size_t i = 0; i < 512; i++) {
    buffer.insertRecord(two);
  }
  for (size_t i = 0; i < 256; i++) {
    buffer.insertRecord(one);
  }

  auto frequencies = buffer.getFrequencies();
  ASSERT_TRUE(static_cast<uint64_t>(2) == frequencies.size());
  ASSERT_TRUE(one == frequencies[0].first);
  ASSERT_TRUE(static_cast<uint64_t>(150) <= frequencies[0].second);
  ASSERT_TRUE(static_cast<uint64_t>(256) >= frequencies[0].second);
  ASSERT_TRUE(two == frequencies[1].first);
  ASSERT_TRUE(static_cast<uint64_t>(300) <= frequencies[1].second);
  ASSERT_TRUE(static_cast<uint64_t>(512) >= frequencies[1].second);

  for (size_t i = 0; i < 8192; i++) {
    buffer.insertRecord(one);
  }

  frequencies = buffer.getFrequencies();
  if (frequencies.size() == 1) {
    ASSERT_TRUE(static_cast<size_t>(1) == frequencies.size());
    ASSERT_TRUE(one == frequencies[0].first);
    ASSERT_TRUE(static_cast<uint64_t>(800) <= frequencies[0].second);
  } else {
    ASSERT_TRUE(static_cast<uint64_t>(2) == frequencies.size());
    ASSERT_TRUE(two == frequencies[0].first);
    ASSERT_TRUE(static_cast<uint64_t>(100) >= frequencies[0].second);
    ASSERT_TRUE(one == frequencies[1].first);
    ASSERT_TRUE(static_cast<uint64_t>(800) <= frequencies[1].second);
  }
}
