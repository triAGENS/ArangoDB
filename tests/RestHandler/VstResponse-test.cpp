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
/// @author Ignacio Rodriguez
////////////////////////////////////////////////////////////////////////////////

#include "gtest/gtest.h"
#include "Rest/VstResponse.h"
#include "velocypack/Parser.h"
#include "Basics/encoding.h"

#include <iostream>

#define ASSERT_ARANGO_OK(x) \
  { ASSERT_TRUE(x.ok()) << x.errorMessage(); }

// -----------------------------------------------------------------------------
// --SECTION--                                                 setup / tear-down
// -----------------------------------------------------------------------------

class VstResponseTest
    : public ::testing::Test {
 protected:

  VstResponseTest() {
  }

};

// -----------------------------------------------------------------------------
// --SECTION--                                                        test suite
// -----------------------------------------------------------------------------

TEST_F(VstResponseTest, add_payload_slice_json) {
  std::cerr << "starting test!\n";
  arangodb::VstResponse resp{arangodb::rest::ResponseCode::OK, 0};
  resp.setContentTypeRequested(arangodb::rest::ContentType::JSON);

  // create a builder
  arangodb::velocypack::Builder builder;

  // use the builder to create the value I want
  uint8_t* p = builder.add(arangodb::StaticStrings::IdString,
                           arangodb::velocypack::ValuePair(9ULL, arangodb::velocypack::ValueType::Custom));

  *p++ = 0xf3;  // custom type for _id
  arangodb::encoding::storeNumber<uint64_t>(p, 12345, sizeof(uint64_t));

  // get a slice, from the builder
  auto const slice {builder.slice()};

  // add the payload
  resp.addPayload(slice, nullptr, true);

  // retrieve the payload
  auto const payload {resp.payload()};

  EXPECT_EQ(payload.length(), 1);
  EXPECT_EQ(payload.byteSize(), 1);
  EXPECT_EQ(payload[0], '3');
}
