add_library(arango_tests_optimizer_rules OBJECT
  EnumeratePathsRuleTest.cpp)

target_link_libraries(arango_tests_optimizer_rules PUBLIC
  arango_lightweight
  arango_aql
  velocypack
  arango_crashhandler_light
  arango_futures
  gtest
  gmock
  immer
  rocksdb_interface
  boost_boost
  )

target_include_directories(arango_tests_optimizer_rules PUBLIC
        "${PROJECT_SOURCE_DIR}/arangod"
        "${PROJECT_SOURCE_DIR}/tests"
        "${PROJECT_SOURCE_DIR}/${ENTERPRISE_INCLUDE_DIR}")

add_executable(arangodbtests_optimizer_rules EXCLUDE_FROM_ALL)

target_link_libraries(arangodbtests_optimizer_rules PRIVATE
  velocypack_utils
  arango_tests_basics
  arango_tests_optimizer_rules)

