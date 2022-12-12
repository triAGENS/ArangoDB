////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2022-2022 ArangoDB GmbH, Cologne, Germany
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

#include "gtest/gtest.h"

#include "Basics/Exceptions.h"
#include "Cluster/ServerDefaults.h"
#include "Logger/LogMacros.h"
#include "VocBase/Properties/PlanCollection.h"
#include <velocypack/Builder.h>

namespace arangodb::tests {

/**********************
 * MACRO SECTION
 *
 * Here are some helper Makros to fill some of the below test code
 * which is highly overlapping
 *********************/

#define GenerateFailsOnBool(attributeName)                                  \
  assertParsingThrows(createMinimumBodyWithOneValue(#attributeName, true)); \
  assertParsingThrows(createMinimumBodyWithOneValue(#attributeName, false));

#define GenerateFailsOnInteger(attributeName)                             \
  assertParsingThrows(createMinimumBodyWithOneValue(#attributeName, 1));  \
  assertParsingThrows(createMinimumBodyWithOneValue(#attributeName, 0));  \
  assertParsingThrows(createMinimumBodyWithOneValue(#attributeName, 42)); \
  assertParsingThrows(createMinimumBodyWithOneValue(#attributeName, -2));

#define GenerateFailsOnDouble(attributeName)                               \
  assertParsingThrows(createMinimumBodyWithOneValue(#attributeName, 4.5)); \
  assertParsingThrows(createMinimumBodyWithOneValue(#attributeName, 0.2)); \
  assertParsingThrows(createMinimumBodyWithOneValue(#attributeName, -0.3));

#define GenerateFailsOnNonEmptyString(attributeName)                          \
  assertParsingThrows(createMinimumBodyWithOneValue(#attributeName, "test")); \
  assertParsingThrows(                                                        \
      createMinimumBodyWithOneValue(#attributeName, "dogfather"));

#define GenerateFailsOnString(attributeName)                              \
  assertParsingThrows(createMinimumBodyWithOneValue(#attributeName, "")); \
  GenerateFailsOnNonEmptyString(attribtueName)

#define GenerateFailsOnArray(attributeName)          \
  assertParsingThrows(createMinimumBodyWithOneValue( \
      #attributeName, VPackSlice::emptyArraySlice()));

#define GenerateFailsOnObject(attributeName)         \
  assertParsingThrows(createMinimumBodyWithOneValue( \
      #attributeName, VPackSlice::emptyObjectSlice()));

// This macro generates a basic bool value test, checking if we get true/false
// through and other basic types are rejected.

#define GenerateBoolAttributeTest(attributeName)                              \
  TEST_F(PlanCollectionUserAPITest, test_##attributeName) {                   \
    auto shouldBeEvaluatedTo = [&](VPackBuilder const& body, bool expected) { \
      auto testee = PlanCollection::fromCreateAPIBody(body.slice(), {});      \
      EXPECT_EQ(testee->attributeName, expected)                              \
          << "Parsing error in " << body.toJson();                            \
    };                                                                        \
    shouldBeEvaluatedTo(createMinimumBodyWithOneValue(#attributeName, true),  \
                        true);                                                \
    shouldBeEvaluatedTo(createMinimumBodyWithOneValue(#attributeName, false), \
                        false);                                               \
    GenerateFailsOnInteger(attributeName);                                    \
    GenerateFailsOnDouble(attributeName);                                     \
    GenerateFailsOnString(attributeName);                                     \
    GenerateFailsOnArray(attributeName);                                      \
    GenerateFailsOnObject(attributeName);                                     \
  }
// This macro generates a basic integer value test, checking if we get 2 and 42
// through and other basic types are rejected.
// NOTE: we also test 4.5 (double) right now this passes the validator, need to
// discuss if this is correct

#define GeneratePositiveIntegerAttributeTestInternal(attributeName, valueName) \
  TEST_F(PlanCollectionUserAPITest, test_##attributeName) {                    \
    auto shouldBeEvaluatedTo = [&](VPackBuilder const& body,                   \
                                   uint64_t expected) {                        \
      auto testee = PlanCollection::fromCreateAPIBody(body.slice(), {});       \
      EXPECT_EQ(testee->valueName, expected)                                   \
          << "Parsing error in " << body.toJson();                             \
    };                                                                         \
    shouldBeEvaluatedTo(createMinimumBodyWithOneValue(#attributeName, 2), 2);  \
    shouldBeEvaluatedTo(createMinimumBodyWithOneValue(#attributeName, 42),     \
                        42);                                                   \
    shouldBeEvaluatedTo(createMinimumBodyWithOneValue(#attributeName, 4.5),    \
                        4);                                                    \
    assertParsingThrows(createMinimumBodyWithOneValue(#attributeName, -1));    \
    assertParsingThrows(createMinimumBodyWithOneValue(#attributeName, 0));     \
    assertParsingThrows(createMinimumBodyWithOneValue(#attributeName, -4.5));  \
    GenerateFailsOnBool(attributeName);                                        \
    GenerateFailsOnString(attributeName);                                      \
    GenerateFailsOnArray(attributeName);                                       \
    GenerateFailsOnObject(attributeName);                                      \
  }

#define GeneratePositiveIntegerAttributeTest(attributeName) \
  GeneratePositiveIntegerAttributeTestInternal(attributeName, attributeName)

#define GenerateStringAttributeTest(attributeName)                             \
  TEST_F(PlanCollectionUserAPITest, test_##attributeName) {                    \
    auto shouldBeEvaluatedTo = [&](VPackBuilder const& body,                   \
                                   std::string const& expected) {              \
      auto testee = PlanCollection::fromCreateAPIBody(body.slice(), {});       \
      EXPECT_EQ(testee->attributeName, expected)                               \
          << "Parsing error in " << body.toJson();                             \
    };                                                                         \
    shouldBeEvaluatedTo(createMinimumBodyWithOneValue(#attributeName, "test"), \
                        "test");                                               \
    shouldBeEvaluatedTo(                                                       \
        createMinimumBodyWithOneValue(#attributeName, "unknown"), "unknown");  \
    GenerateFailsOnBool(attributeName);                                        \
    GenerateFailsOnInteger(attributeName);                                     \
    GenerateFailsOnDouble(attributeName);                                      \
    GenerateFailsOnArray(attributeName);                                       \
    GenerateFailsOnObject(attributeName);                                      \
  }

#define GenerateOptionalStringAttributeTest(attributeName)                     \
  TEST_F(PlanCollectionUserAPITest, test_##attributeName) {                    \
    auto shouldBeEvaluatedTo = [&](VPackBuilder const& body,                   \
                                   std::string const& expected) {              \
      auto testee = PlanCollection::fromCreateAPIBody(body.slice(), {});       \
      ASSERT_TRUE(testee->attributeName.has_value())                           \
          << "Parsing error in " << body.toJson();                             \
      EXPECT_EQ(testee->attributeName.value(), expected)                       \
          << "Parsing error in " << body.toJson();                             \
    };                                                                         \
    shouldBeEvaluatedTo(createMinimumBodyWithOneValue(#attributeName, "test"), \
                        "test");                                               \
    shouldBeEvaluatedTo(                                                       \
        createMinimumBodyWithOneValue(#attributeName, "unknown"), "unknown");  \
    GenerateFailsOnBool(attributeName);                                        \
    GenerateFailsOnInteger(attributeName);                                     \
    GenerateFailsOnDouble(attributeName);                                      \
    GenerateFailsOnArray(attributeName);                                       \
    GenerateFailsOnObject(attributeName);                                      \
  }

/**********************
 * TEST SECTION
 *********************/

class PlanCollectionUserAPITest : public ::testing::Test {
 protected:
  /// @brief this generates the minimal required body, only exchanging one
  /// attribute with the given value should work on all basic types
  /// if your attribute is "name" you will get a body back only with the name,
  /// otherwise there will be a body with a valid name + your given value.
  template<typename T>
  VPackBuilder createMinimumBodyWithOneValue(std::string const& attributeName,
                                             T const& attributeValue) {
    std::string colName = "test";
    VPackBuilder body;
    {
      VPackObjectBuilder guard(&body);
      if (attributeName != "name") {
        body.add("name", VPackValue(colName));
      }
      if constexpr (std::is_same_v<T, VPackSlice>) {
        body.add(attributeName, attributeValue);
      } else if constexpr (std::is_same_v<T, std::vector<std::string>>) {
        body.add(VPackValue(attributeName));
        VPackArrayBuilder arrayGuard(&body);
        for (auto const& val : attributeValue) {
          body.add(VPackValue(val));
        }
      } else {
        body.add(attributeName, VPackValue(attributeValue));
      }
    }
    return body;
  }

  static void assertParsingThrows(VPackBuilder const& body) {
    auto p = PlanCollection::fromCreateAPIBody(body.slice(), {});
    EXPECT_TRUE(p.fail()) << " On body " << body.toJson();
  }
};

TEST_F(PlanCollectionUserAPITest, test_requires_some_input) {
  VPackBuilder body;
  { VPackObjectBuilder guard(&body); }
  assertParsingThrows(body);
}

TEST_F(PlanCollectionUserAPITest, test_minimal_user_input) {
  std::string colName = "test";
  VPackBuilder body;
  {
    VPackObjectBuilder guard(&body);
    body.add("name", VPackValue(colName));
  }
  auto testee = PlanCollection::fromCreateAPIBody(body.slice(), {});
  ASSERT_TRUE(testee.ok());
  EXPECT_EQ(testee->name, colName);
  // Test Default values
  EXPECT_FALSE(testee->waitForSync);
  EXPECT_FALSE(testee->isSystem);
  EXPECT_FALSE(testee->doCompact);
  EXPECT_FALSE(testee->isVolatile);
  EXPECT_FALSE(testee->cacheEnabled);
  EXPECT_EQ(testee->type, TRI_col_type_e::TRI_COL_TYPE_DOCUMENT);

  EXPECT_EQ(testee->numberOfShards, 1);
  EXPECT_EQ(testee->replicationFactor, 1);
  EXPECT_EQ(testee->writeConcern, 1);

  EXPECT_EQ(testee->distributeShardsLike, "");
  EXPECT_FALSE(testee->smartJoinAttribute.has_value());
  EXPECT_EQ(testee->globallyUniqueId, "");
  EXPECT_EQ(testee->shardingStrategy, "");

  // TODO: We only test defaults here, not all possible options
  ASSERT_EQ(testee->shardKeys.size(), 1);
  EXPECT_EQ(testee->shardKeys.at(0), StaticStrings::KeyString);

  // TODO: this is just rudimentary
  // does not test internals yet
  EXPECT_TRUE(testee->computedValues.slice().isEmptyArray());
  EXPECT_TRUE(testee->schema.slice().isEmptyObject());
  EXPECT_TRUE(testee->keyOptions.slice().isEmptyObject());

  // This covers only non-documented APIS
  EXPECT_TRUE(testee->syncByRevision);
  EXPECT_TRUE(testee->usesRevisionsAsDocumentIds);
  EXPECT_FALSE(testee->isSmart);
  EXPECT_FALSE(testee->isDisjoint);
  EXPECT_EQ(testee->id, "");
  EXPECT_EQ(testee->smartGraphAttribute, "");
  EXPECT_TRUE(testee->avoidServers.empty());
  EXPECT_FALSE(testee->isSmartChild);
}

TEST_F(PlanCollectionUserAPITest, test_illegal_names) {
  // The empty string
  assertParsingThrows(createMinimumBodyWithOneValue("name", ""));

  // Non String types
  assertParsingThrows(createMinimumBodyWithOneValue("name", 0));
  assertParsingThrows(
      createMinimumBodyWithOneValue("name", VPackSlice::emptyObjectSlice()));
  assertParsingThrows(
      createMinimumBodyWithOneValue("name", VPackSlice::emptyArraySlice()));

  GenerateFailsOnBool(name);
  GenerateFailsOnInteger(name);
  GenerateFailsOnDouble(name);
  GenerateFailsOnArray(name);
  GenerateFailsOnObject(name);
}

TEST_F(PlanCollectionUserAPITest, test_collection_type) {
  auto shouldBeEvaluatedToType = [&](VPackBuilder const& body,
                                     TRI_col_type_e type) {
    auto testee = PlanCollection::fromCreateAPIBody(body.slice(), {});
    EXPECT_EQ(testee->type, type) << "Parsing error in " << body.toJson();
  };

  // Edge types, we only have two valid ways to get edges
  shouldBeEvaluatedToType(createMinimumBodyWithOneValue("type", 3),
                          TRI_col_type_e::TRI_COL_TYPE_EDGE);

  shouldBeEvaluatedToType(createMinimumBodyWithOneValue("type", 2),
                          TRI_col_type_e::TRI_COL_TYPE_DOCUMENT);

  /// The following defaulted to edge before (not mentioned in doc since 3.3)
  assertParsingThrows(createMinimumBodyWithOneValue("type", "edge"));
  /// The following defaulted to document before (not mentioned in doc
  /// since 3.3):
  assertParsingThrows(createMinimumBodyWithOneValue("type", 0));
  assertParsingThrows(createMinimumBodyWithOneValue("type", 1));
  assertParsingThrows(createMinimumBodyWithOneValue("type", 4));
  assertParsingThrows(createMinimumBodyWithOneValue("type", "document"));

  assertParsingThrows(createMinimumBodyWithOneValue("type", "dogfather"));
  assertParsingThrows(
      createMinimumBodyWithOneValue("type", VPackSlice::emptyObjectSlice()));
  assertParsingThrows(
      createMinimumBodyWithOneValue("type", VPackSlice::emptyArraySlice()));
}

TEST_F(PlanCollectionUserAPITest, test_shardingStrategy) {
  auto shouldBeEvaluatedTo = [&](VPackBuilder const& body,
                                 std::string const& expected) {
    auto testee = PlanCollection::fromCreateAPIBody(body.slice(), {});
    EXPECT_EQ(testee->shardingStrategy, expected)
        << "Parsing error in " << body.toJson();
  };
  std::vector<std::string> allowedStrategies{"",
                                             "hash",
                                             "enterprise-hash-smart-edge",
                                             "community-compat",
                                             "enterprise-compat",
                                             "enterprise-smart-edge-compat"};

  for (auto const& strategy : allowedStrategies) {
    shouldBeEvaluatedTo(
        createMinimumBodyWithOneValue("shardingStrategy", strategy), strategy);
  }

  GenerateFailsOnBool(shardingStrategy);
  GenerateFailsOnNonEmptyString(shardingStrategy);
  GenerateFailsOnInteger(shardingStrategy);
  GenerateFailsOnDouble(shardingStrategy);
  GenerateFailsOnArray(shardingStrategy);
  GenerateFailsOnObject(shardingStrategy);
}

TEST_F(PlanCollectionUserAPITest,
       test_writeConcernWinsVersusminReplicationFactor) {
  std::string colName = "test";
  {
    VPackBuilder body;
    {
      VPackObjectBuilder guard(&body);
      body.add("name", VPackValue(colName));
      body.add("writeConcern", VPackValue(3));
      body.add("minReplicationFactor", VPackValue(5));
      // has to be greater or equal to used writeConcern
      body.add("replicationFactor", VPackValue(4));
    }

    auto testee = PlanCollection::fromCreateAPIBody(body.slice(), {});
    ASSERT_TRUE(testee.ok()) << testee.result().errorNumber() << " -> "
                             << testee.result().errorMessage();
    EXPECT_EQ(testee->writeConcern, 3);
  }
  {
    // We change order of attributes in the input vpack
    // to ensure ordering in json does not have a subtle impact
    VPackBuilder body;
    {
      VPackObjectBuilder guard(&body);
      // has to be greater or equal to used writeConcern
      body.add("name", VPackValue(colName));
      body.add("replicationFactor", VPackValue(4));
      body.add("minReplicationFactor", VPackValue(5));
      body.add("writeConcern", VPackValue(3));
    }

    auto testee = PlanCollection::fromCreateAPIBody(body.slice(), {});
    ASSERT_TRUE(testee.ok()) << testee.result().errorNumber() << " -> "
                             << testee.result().errorMessage();
    EXPECT_EQ(testee->writeConcern, 3);
  }
}

TEST_F(PlanCollectionUserAPITest, test_satelliteReplicationFactor) {
  auto shouldBeEvaluatedTo = [&](VPackBuilder const& body, uint64_t number) {
    auto testee = PlanCollection::fromCreateAPIBody(body.slice(), {});
    ASSERT_TRUE(testee.ok()) << testee.result().errorMessage();
    EXPECT_EQ(testee->replicationFactor, number)
        << "Parsing error in " << body.toJson();
  };

  // Special handling for "satellite" string
  shouldBeEvaluatedTo(
      createMinimumBodyWithOneValue("replicationFactor", "satellite"), 0);
}

TEST_F(PlanCollectionUserAPITest, test_configureMaxNumberOfShards) {
  auto body = createMinimumBodyWithOneValue("numberOfShards", 1024);

  // First Step of parsing has to pass
  auto testee = PlanCollection::fromCreateAPIBody(body.slice(), {});
  ASSERT_TRUE(testee.ok()) << testee.result().errorMessage();
  EXPECT_EQ(testee->numberOfShards, 1024)
      << "Parsing error in " << body.toJson();

  PlanCollection::DatabaseConfiguration config{};
  EXPECT_EQ(config.maxNumberOfShards, 0);
  EXPECT_EQ(config.shouldValidateClusterSettings, false);

  {
    config.shouldValidateClusterSettings = false;
    // If shouldValidateClusterSettings is false, numberOfShards should not have
    // effect
    for (uint32_t maxShards : std::vector<uint32_t>{0, 16, 1023, 1024, 1025}) {
      config.maxNumberOfShards = maxShards;
      auto res = testee->validateDatabaseConfiguration(config);
      EXPECT_TRUE(res.ok()) << res.errorMessage();
    }
  }
  {
    config.shouldValidateClusterSettings = true;
    // If shouldValidateClusterSettings is true, numberOfShards should be
    // checked
    {
      // 0 := unlimited shards, 1024 should be okay
      config.maxNumberOfShards = 0;
      auto res = testee->validateDatabaseConfiguration(config);
      EXPECT_TRUE(res.ok()) << res.errorMessage();
    }
    {
      // 1024 == 1024 should be okay
      config.maxNumberOfShards = 1024;
      auto res = testee->validateDatabaseConfiguration(config);
      EXPECT_TRUE(res.ok()) << res.errorMessage();
    }
    {
      // 1025 >= 1024 should be okay
      config.maxNumberOfShards = 1025;
      auto res = testee->validateDatabaseConfiguration(config);
      EXPECT_TRUE(res.ok()) << res.errorMessage();
    }

    {
      // 16 < 1024 should fail
      config.maxNumberOfShards = 16;
      auto res = testee->validateDatabaseConfiguration(config);
      EXPECT_FALSE(res.ok()) << "Configured " << config.maxNumberOfShards
                             << " but " << testee->numberOfShards << "passed.";
    }
  }
}

TEST_F(PlanCollectionUserAPITest, test_isSmartCannotBeSatellite) {
  VPackBuilder body;
  {
    VPackObjectBuilder guard(&body);
    // has to be greater or equal to used writeConcern
    body.add("name", VPackValue("test"));
    body.add("isSmart", VPackValue(true));
    body.add("replicationFactor", VPackValue("satellite"));
  }

  // Note: We can also make this parsing fail in the first place.
  auto testee = PlanCollection::fromCreateAPIBody(body.slice(), {});
  ASSERT_TRUE(testee.ok()) << testee.result().errorNumber() << " -> "
                           << testee.result().errorMessage();
  EXPECT_EQ(testee->isSmart, true);
  EXPECT_EQ(testee->replicationFactor, 0);

  // No special config required, this always fails
  PlanCollection::DatabaseConfiguration config{};
  auto res = testee->validateDatabaseConfiguration(config);
  EXPECT_FALSE(res.ok()) << "Configured smartCollection as 'satellite'.";
}

TEST_F(PlanCollectionUserAPITest, test_isSmartChildCannotBeSatellite) {
  VPackBuilder body;
  {
    VPackObjectBuilder guard(&body);
    // has to be greater or equal to used writeConcern
    body.add("name", VPackValue("test"));
    body.add("isSmartChild", VPackValue(true));
    body.add("replicationFactor", VPackValue("satellite"));
  }

  // Note: We can also make this parsing fail in the first place.
  auto testee = PlanCollection::fromCreateAPIBody(body.slice(), {});
  ASSERT_TRUE(testee.ok()) << testee.result().errorNumber() << " -> "
                           << testee.result().errorMessage();
  EXPECT_EQ(testee->isSmartChild, true);
  EXPECT_EQ(testee->replicationFactor, 0);

  // No special config required, this always fails
  PlanCollection::DatabaseConfiguration config{};
  auto res = testee->validateDatabaseConfiguration(config);
  EXPECT_FALSE(res.ok()) << "Configured smartChild collection as 'satellite'.";
}

TEST_F(PlanCollectionUserAPITest, test_oneShardDBCannotBeSatellite) {
  VPackBuilder body;
  {
    VPackObjectBuilder guard(&body);
    // has to be greater or equal to used writeConcern
    body.add("name", VPackValue("test"));
    body.add("replicationFactor", VPackValue("satellite"));
  }

  // Note: We can also make this parsing fail in the first place.
  auto testee = PlanCollection::fromCreateAPIBody(body.slice(), {});
  ASSERT_TRUE(testee.ok()) << testee.result().errorNumber() << " -> "
                           << testee.result().errorMessage();
  EXPECT_EQ(testee->replicationFactor, 0);

  // No special config required, this always fails
  PlanCollection::DatabaseConfiguration config{};
  config.isOneShardDB = true;
  auto res = testee->validateDatabaseConfiguration(config);
  EXPECT_FALSE(res.ok()) << "Configured a oneShardDB collection as 'satellite'.";
}



TEST_F(PlanCollectionUserAPITest, test_atMost8ShardKeys) {
  // We split this string up into characters, to use those as shardKey
  // attributes Just for simplicity reasons, and to avoid having duplicates
  std::string shardKeySelection = "abcdefghijklm";

  std::vector<std::string> shardKeysToTest{};
  for (size_t i = 0; i < 8; ++i) {
    // Always add one character from above string, no character is used twice
    shardKeysToTest.emplace_back(shardKeySelection.substr(i, 1));
    auto body = createMinimumBodyWithOneValue("shardKeys", shardKeysToTest);

    // The first 8 have to be allowed
    auto testee = PlanCollection::fromCreateAPIBody(body.slice(), {});
    ASSERT_TRUE(testee.ok()) << testee.result().errorMessage();
    EXPECT_EQ(testee->shardKeys, shardKeysToTest)
        << "Parsing error in " << body.toJson();
  }

  for (size_t i = 8; i < 10; ++i) {
    // Always add one character from above string, no character is used twice
    shardKeysToTest.emplace_back(shardKeySelection.substr(i, 1));
    auto body = createMinimumBodyWithOneValue("shardKeys", shardKeysToTest);

    // The first 8 have to be allowed
    auto testee = PlanCollection::fromCreateAPIBody(body.slice(), {});
    EXPECT_FALSE(testee.ok())
        << "Created too many shard keys: " << shardKeysToTest.size();
  }
}

TEST_F(PlanCollectionUserAPITest, test_shardKeyOnSatellites) {
  // We do not need any special configuration
  // default is good enough
  PlanCollection::DatabaseConfiguration config{};

  // Sharding by specific shardKey, or prefix/postfix of _key is not allowed
  for (auto const& key : {"testKey", "a", ":_key", "_key:"}) {
    // Specific shardKey is disallowed
    VPackBuilder body;
    {
      VPackObjectBuilder guard(&body);
      // has to be greater or equal to used writeConcern
      body.add("name", VPackValue("test"));
      body.add("replicationFactor", VPackValue("satellite"));
      body.add(VPackValue("shardKeys"));
      {
        VPackArrayBuilder arrayGuard{&body};
        body.add(VPackValue("testKey"));
      }
    }
    auto testee = PlanCollection::fromCreateAPIBody(body.slice(), {});
    auto result = testee.result();
    if (result.ok()) {
      result = testee->validateDatabaseConfiguration(config);
    }
    EXPECT_FALSE(result.ok())
        << "Created a satellite collection with a shardkey: " << key;
  }
  {
    // Shard by _key is allowed
    VPackBuilder body;
    {
      VPackObjectBuilder guard(&body);
      // has to be greater or equal to used writeConcern
      body.add("name", VPackValue("test"));
      body.add("replicationFactor", VPackValue("satellite"));
      body.add(VPackValue("shardKeys"));
      {
        VPackArrayBuilder arrayGuard{&body};
        body.add(VPackValue("_key"));
      }
    }
    auto testee = PlanCollection::fromCreateAPIBody(body.slice(), {});
    auto result = testee.result();
    if (result.ok()) {
      result = testee->validateDatabaseConfiguration(config);
    }
    EXPECT_TRUE(result.ok())
        << "Failed to created a satellite collection with default sharding "
        << result.errorMessage();
  }

  {
    // Shard by _key + something is not allowed
    VPackBuilder body;
    {
      VPackObjectBuilder guard(&body);
      // has to be greater or equal to used writeConcern
      body.add("name", VPackValue("test"));
      body.add("replicationFactor", VPackValue("satellite"));
      body.add(VPackValue("shardKeys"));
      {
        VPackArrayBuilder arrayGuard{&body};
        body.add(VPackValue("_key"));
        body.add(VPackValue("testKey"));
      }
    }
    auto testee = PlanCollection::fromCreateAPIBody(body.slice(), {});
    auto result = testee.result();
    if (result.ok()) {
      result = testee->validateDatabaseConfiguration(config);
    }
    EXPECT_FALSE(result.ok())
        << "Created a satellite collection with a shardkeys [_key, testKey]";
  }
}

TEST_F(PlanCollectionUserAPITest, test_internal_values_as_shardkeys) {
  // Sharding by internal keys, or prefix/postfix of them is not allowed
  for (auto const& key : {"_id", "_rev", ":_id", "_id:", ":_rev", "_rev:"}) {
    // Specific shardKey is disallowed
    auto body = createMinimumBodyWithOneValue("shardKeys",
                                              std::vector<std::string>{key});
    auto testee = PlanCollection::fromCreateAPIBody(body.slice(), {});
    EXPECT_FALSE(testee.ok()) << "Created a collection with shardkey: " << key;
  }
}

TEST_F(PlanCollectionUserAPITest, test_distributeShardsLike_default) {
  // We do not need any special configuration
  // default is good enough
  std::string defaultShardBy = "_graphs";
  PlanCollection::DatabaseConfiguration config{};
  config.defaultDistributeShardsLike = defaultShardBy;

  // Specific shardKey is disallowed
  auto body = createMinimumBodyWithOneValue("name", "test");
  auto testee = PlanCollection::fromCreateAPIBody(body.slice(), config);
  ASSERT_TRUE(testee.ok()) << "Failed on " << testee.errorMessage();
  EXPECT_EQ(testee->distributeShardsLike, defaultShardBy);
}

TEST_F(PlanCollectionUserAPITest, test_oneShard_forcesDistributeShardsLike) {
  // We do not need any special configuration
  // default is good enough
  std::string defaultShardBy = "_graphs";
  PlanCollection::DatabaseConfiguration config{};
  config.defaultDistributeShardsLike = defaultShardBy;
  config.isOneShardDB = true;

  // Specific shardKey is disallowed
  auto body = createMinimumBodyWithOneValue("distributeShardsLike", "test");
  auto testee = PlanCollection::fromCreateAPIBody(body.slice(), config);
  ASSERT_TRUE(testee.ok()) << "Failed on " << testee.errorMessage();
  EXPECT_EQ(testee->distributeShardsLike, "test");

  auto res = testee->validateDatabaseConfiguration(config);
  EXPECT_FALSE(res.ok()) << "Distribute shards like violates oneShard database";
}

TEST_F(PlanCollectionUserAPITest, test_oneShard_moreShards) {
  // Configure oneShardDB properly
  std::string defaultShardBy = "_graphs";
  PlanCollection::DatabaseConfiguration config{};
  config.defaultDistributeShardsLike = defaultShardBy;
  config.isOneShardDB = true;

  // Specific shardKey is disallowed
  auto body = createMinimumBodyWithOneValue("numberOfShards", 5);
  auto testee = PlanCollection::fromCreateAPIBody(body.slice(), config);
  // This could already fail, as soon as we have a context
  ASSERT_TRUE(testee.ok()) << "Failed on " << testee.errorMessage();
  EXPECT_EQ(testee->numberOfShards, 5);

  auto res = testee->validateDatabaseConfiguration(config);
  EXPECT_FALSE(res.ok()) << "Number of Shards violates oneShard database";
}

TEST_F(PlanCollectionUserAPITest, test_smartJoinAttribute_cannot_be_empty) {
  PlanCollection::DatabaseConfiguration config{};

  // Specific shardKey is disallowed
  auto body =
      createMinimumBodyWithOneValue(StaticStrings::SmartJoinAttribute, "");
  auto testee = PlanCollection::fromCreateAPIBody(body.slice(), config);
  // This could already fail, as soon as we have a context
  EXPECT_FALSE(testee.ok()) << "Let an empty smartJoinAttribute through";
}

// Tests for generic attributes without special needs
GenerateBoolAttributeTest(waitForSync);
GenerateBoolAttributeTest(doCompact);
GenerateBoolAttributeTest(isSystem);
GenerateBoolAttributeTest(isVolatile);
GenerateBoolAttributeTest(cacheEnabled);

GeneratePositiveIntegerAttributeTest(numberOfShards);
GeneratePositiveIntegerAttributeTest(replicationFactor);
GeneratePositiveIntegerAttributeTest(writeConcern);

GenerateStringAttributeTest(distributeShardsLike);
GenerateOptionalStringAttributeTest(smartJoinAttribute);
GenerateStringAttributeTest(globallyUniqueId);

// Covers a non-documented API
GenerateBoolAttributeTest(syncByRevision);
GenerateBoolAttributeTest(usesRevisionsAsDocumentIds);
GenerateBoolAttributeTest(isSmart);
GenerateBoolAttributeTest(isSmartChild);
GenerateBoolAttributeTest(isDisjoint);
GenerateStringAttributeTest(id);
GenerateStringAttributeTest(smartGraphAttribute);

GeneratePositiveIntegerAttributeTestInternal(minReplicationFactor,
                                             writeConcern);
namespace {
enum AllowedFlags : uint8_t {
  Allways = 0,
  Disallowed = 1 << 0,
  AsSystem = 1 << 1,
  WithExtension = 1 << 2
};

struct CollectionNameTestParam {
  std::string name;
  uint8_t allowedFlags;
  std::string disallowReason;
};
}  // namespace
class PlanCollectionNamesTest
    : public ::testing::TestWithParam<CollectionNameTestParam> {
 protected:
  [[nodiscard]] std::string getName() const {
    auto p = GetParam();
    return p.name;
  }

  [[nodiscard]] std::string getErrorReason() const {
    auto p = GetParam();
    return p.disallowReason + " on collection " + p.name;
  }

  [[nodiscard]] bool isDisAllowedInGeneral() const {
    auto p = GetParam();
    return p.allowedFlags & AllowedFlags::Disallowed;
  };

  [[nodiscard]] bool requiresSystem() const {
    auto p = GetParam();
    return p.allowedFlags & AllowedFlags::AsSystem;
  };

  [[nodiscard]] bool requiresExtendedNames() const {
    auto p = GetParam();
    return p.allowedFlags & AllowedFlags::WithExtension;
  };
};

INSTANTIATE_TEST_CASE_P(
    PlanCollectionNamesTest, PlanCollectionNamesTest,
    ::testing::Values(
        CollectionNameTestParam{"", AllowedFlags::Disallowed,
                                "name cannot be empty"},
        CollectionNameTestParam{"test", AllowedFlags::Allways, ""},
        CollectionNameTestParam{std::string(256, 'x'), AllowedFlags::Allways,
                                "maximum allowed length"},
        CollectionNameTestParam{std::string(257, 'x'), AllowedFlags::Disallowed,
                                "above maximum allowed length"},
        CollectionNameTestParam{"_test", AllowedFlags::AsSystem,
                                "_ at the beginning requires system"},

        CollectionNameTestParam{"Десятую", AllowedFlags::WithExtension,
                                "non-ascii characters"},
        CollectionNameTestParam{"💩🍺🌧t⛈c🌩_⚡🔥💥🌨",
                                AllowedFlags::WithExtension,
                                "non-ascii characters"},
        CollectionNameTestParam{
            "_💩🍺🌧t⛈c🌩_⚡🔥💥🌨",
            AllowedFlags::AsSystem | AllowedFlags::WithExtension,
            "non-ascii and system"}));

TEST_P(PlanCollectionNamesTest, test_allowed_without_flags) {
  VPackBuilder body;
  {
    VPackObjectBuilder bodyGuard{&body};
    body.add("name", VPackValue(getName()));
  }
  PlanCollection::DatabaseConfiguration config{};
  EXPECT_EQ(config.allowExtendedNames, false);

  auto testee = PlanCollection::fromCreateAPIBody(body.slice(), {});
  auto result = testee.result();
  if (result.ok()) {
    result = testee->validateDatabaseConfiguration(config);
  }
  bool isAllowed =
      !isDisAllowedInGeneral() && !requiresSystem() && !requiresExtendedNames();

  if (isAllowed) {
    ASSERT_TRUE(result.ok()) << result.errorMessage();
    EXPECT_EQ(testee->name, getName()) << "Parsing error in " << body.toJson();
  } else {
    EXPECT_FALSE(result.ok()) << getErrorReason();
  }
}

TEST_P(PlanCollectionNamesTest, test_allowed_with_isSystem_flag) {
  VPackBuilder body;
  {
    VPackObjectBuilder bodyGuard{&body};
    body.add("name", VPackValue(getName()));
    body.add("isSystem", VPackValue(true));
  }
  PlanCollection::DatabaseConfiguration config{};
  EXPECT_EQ(config.allowExtendedNames, false);

  auto testee = PlanCollection::fromCreateAPIBody(body.slice(), {});
  auto result = testee.result();
  if (result.ok()) {
    result = testee->validateDatabaseConfiguration(config);
  }
  bool isAllowed = !isDisAllowedInGeneral() && !requiresExtendedNames();

  if (isAllowed) {
    ASSERT_TRUE(result.ok()) << result.errorMessage();
    EXPECT_EQ(testee->name, getName()) << "Parsing error in " << body.toJson();
  } else {
    EXPECT_FALSE(result.ok()) << getErrorReason();
  }
}

TEST_P(PlanCollectionNamesTest, test_allowed_with_extendendNames_flag) {
  VPackBuilder body;
  {
    VPackObjectBuilder bodyGuard{&body};
    body.add("name", VPackValue(getName()));
  }
  PlanCollection::DatabaseConfiguration config{};
  config.allowExtendedNames = true;

  auto testee = PlanCollection::fromCreateAPIBody(body.slice(), {});
  auto result = testee.result();
  if (result.ok()) {
    result = testee->validateDatabaseConfiguration(config);
  }
  bool isAllowed = !isDisAllowedInGeneral() && !requiresSystem();

  if (isAllowed) {
    ASSERT_TRUE(result.ok()) << result.errorMessage();
    EXPECT_EQ(testee->name, getName()) << "Parsing error in " << body.toJson();
  } else {
    EXPECT_FALSE(result.ok()) << getErrorReason();
  }
}

TEST_P(PlanCollectionNamesTest,
       test_allowed_with_isSystem_andextendedNames_flag) {
  VPackBuilder body;
  {
    VPackObjectBuilder bodyGuard{&body};
    body.add("name", VPackValue(getName()));
    body.add("isSystem", VPackValue(true));
  }
  PlanCollection::DatabaseConfiguration config{};
  config.allowExtendedNames = true;

  auto testee = PlanCollection::fromCreateAPIBody(body.slice(), {});
  auto result = testee.result();
  if (result.ok()) {
    result = testee->validateDatabaseConfiguration(config);
  }
  bool isAllowed = !isDisAllowedInGeneral();

  if (isAllowed) {
    ASSERT_TRUE(result.ok()) << result.errorMessage();
    EXPECT_EQ(testee->name, getName()) << "Parsing error in " << body.toJson();
  } else {
    EXPECT_FALSE(result.ok()) << getErrorReason();
  }
}

class PlanCollectionReplicationFactorTest
    : public ::testing::TestWithParam<std::tuple<uint32_t, uint32_t>> {
 protected:
  [[nodiscard]] uint32_t writeConcern() const {
    auto p = GetParam();
    return std::get<0>(p);
  };

  [[nodiscard]] uint32_t replicationFactor() const {
    auto p = GetParam();
    return std::get<1>(p);
  };

  [[nodiscard]] VPackBuilder testBody() {
    VPackBuilder body;
    {
      VPackObjectBuilder guard(&body);
      body.add("name", VPackValue("test"));
      body.add("writeConcern", VPackValue(writeConcern()));
      body.add("replicationFactor", VPackValue(replicationFactor()));
    }
    return body;
  }
};

INSTANTIATE_TEST_CASE_P(
    PlanCollectionReplicationFactorTest, PlanCollectionReplicationFactorTest,
    ::testing::Combine(::testing::Values(1ul, 2ul, 5ul, 8ul, 16ul),
                       ::testing::Values(1ul, 3ul, 5ul, 9ul, 15ul)));

TEST_P(PlanCollectionReplicationFactorTest, test_noMaxReplicationFactor) {
  auto body = testBody();
  PlanCollection::DatabaseConfiguration config{};
  EXPECT_EQ(config.minReplicationFactor, 0);
  EXPECT_EQ(config.maxReplicationFactor, 0);
  EXPECT_EQ(config.enforceReplicationFactor, true);

  config.enforceReplicationFactor = true;

  auto testee = PlanCollection::fromCreateAPIBody(body.slice(), {});
  // Parsing should always be okay

  ASSERT_TRUE(testee.ok()) << testee.result().errorMessage();
  EXPECT_EQ(testee->writeConcern, writeConcern())
      << "Parsing error in " << body.toJson();
  EXPECT_EQ(testee->replicationFactor, replicationFactor())
      << "Parsing error in " << body.toJson();

  auto result = testee->validateDatabaseConfiguration(config);

  // We only check if writeConcern is okay there is no upper bound
  // on replicationFactor
  bool isAllowed = writeConcern() <= replicationFactor();
  if (isAllowed) {
    ASSERT_TRUE(result.ok()) << result.errorMessage();
    EXPECT_EQ(testee->writeConcern, writeConcern());
    EXPECT_EQ(testee->replicationFactor, replicationFactor());
  } else {
    EXPECT_FALSE(result.ok()) << result.errorMessage();
  }
}

TEST_P(PlanCollectionReplicationFactorTest, test_maxReplicationFactor) {
  auto body = testBody();
  PlanCollection::DatabaseConfiguration config{};
  EXPECT_EQ(config.minReplicationFactor, 0);
  EXPECT_EQ(config.maxReplicationFactor, 0);
  EXPECT_EQ(config.enforceReplicationFactor, true);

  config.enforceReplicationFactor = true;
  config.maxReplicationFactor = 5;

  auto testee = PlanCollection::fromCreateAPIBody(body.slice(), {});
  // Parsing should always be okay

  ASSERT_TRUE(testee.ok()) << testee.result().errorMessage();
  EXPECT_EQ(testee->writeConcern, writeConcern())
      << "Parsing error in " << body.toJson();
  EXPECT_EQ(testee->replicationFactor, replicationFactor())
      << "Parsing error in " << body.toJson();

  auto result = testee->validateDatabaseConfiguration(config);

  // We only check if writeConcern is okay there is no upper bound
  // on replicationFactor
  bool isAllowed = writeConcern() <= replicationFactor() &&
                   replicationFactor() <= config.maxReplicationFactor;
  if (isAllowed) {
    ASSERT_TRUE(result.ok()) << result.errorMessage();
    EXPECT_EQ(testee->writeConcern, writeConcern());
    EXPECT_EQ(testee->replicationFactor, replicationFactor());
  } else {
    EXPECT_FALSE(result.ok()) << result.errorMessage();
  }
}

TEST_P(PlanCollectionReplicationFactorTest, test_minReplicationFactor) {
  auto body = testBody();
  PlanCollection::DatabaseConfiguration config{};
  EXPECT_EQ(config.minReplicationFactor, 0);
  EXPECT_EQ(config.maxReplicationFactor, 0);
  EXPECT_EQ(config.enforceReplicationFactor, true);

  config.enforceReplicationFactor = true;
  config.minReplicationFactor = 5;

  auto testee = PlanCollection::fromCreateAPIBody(body.slice(), {});
  // Parsing should always be okay

  ASSERT_TRUE(testee.ok()) << testee.result().errorMessage();
  EXPECT_EQ(testee->writeConcern, writeConcern())
      << "Parsing error in " << body.toJson();
  EXPECT_EQ(testee->replicationFactor, replicationFactor())
      << "Parsing error in " << body.toJson();

  auto result = testee->validateDatabaseConfiguration(config);

  // We only check if writeConcern is okay there is no upper bound
  // on replicationFactor
  bool isAllowed = writeConcern() <= replicationFactor() &&
                   replicationFactor() >= config.minReplicationFactor;
  if (isAllowed) {
    ASSERT_TRUE(result.ok()) << result.errorMessage();
    EXPECT_EQ(testee->writeConcern, writeConcern());
    EXPECT_EQ(testee->replicationFactor, replicationFactor());
  } else {
    EXPECT_FALSE(result.ok()) << result.errorMessage();
  }
}

TEST_P(PlanCollectionReplicationFactorTest, test_nonoEnforce) {
  auto body = testBody();
  PlanCollection::DatabaseConfiguration config{};
  EXPECT_EQ(config.minReplicationFactor, 0);
  EXPECT_EQ(config.maxReplicationFactor, 0);
  EXPECT_EQ(config.enforceReplicationFactor, true);

  config.enforceReplicationFactor = false;
  config.minReplicationFactor = 2;
  config.maxReplicationFactor = 5;

  auto testee = PlanCollection::fromCreateAPIBody(body.slice(), {});
  // Parsing should always be okay

  ASSERT_TRUE(testee.ok()) << testee.result().errorMessage();
  EXPECT_EQ(testee->writeConcern, writeConcern())
      << "Parsing error in " << body.toJson();
  EXPECT_EQ(testee->replicationFactor, replicationFactor())
      << "Parsing error in " << body.toJson();

  auto result = testee->validateDatabaseConfiguration(config);

  // Without enforcing you can do what you want, including illegal combinations
  bool isAllowed = true;
  if (isAllowed) {
    ASSERT_TRUE(result.ok()) << result.errorMessage();
    EXPECT_EQ(testee->writeConcern, writeConcern());
    EXPECT_EQ(testee->replicationFactor, replicationFactor());
  } else {
    EXPECT_FALSE(result.ok()) << result.errorMessage();
  }
}

}  // namespace arangodb::tests
