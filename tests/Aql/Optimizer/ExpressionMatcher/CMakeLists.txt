add_library(arango_tests_ast_expression_matcher OBJECT
  ExpressionMatcherTest.cpp)

target_link_libraries(arango_tests_ast_expression_matcher PUBLIC
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

target_include_directories(arango_tests_ast_expression_matcher PUBLIC
        "${PROJECT_SOURCE_DIR}/arangod"
        "${PROJECT_SOURCE_DIR}/tests"
        "${PROJECT_SOURCE_DIR}/${ENTERPRISE_INCLUDE_DIR}")

add_executable(arangodbtests_ast_expression_matcher EXCLUDE_FROM_ALL)

target_link_libraries(arangodbtests_ast_expression_matcher PRIVATE
  velocypack_utils
  arango_tests_basics
  arango_tests_ast_expression_matcher)

