add_library(arango_tests_iresearch OBJECT
  ContainersTest.cpp
  ExpressionFilterTest.cpp
  GeoAnalyzerTest.cpp
  GeoFilterTest.cpp
  GeoDistanceFilterTest.cpp
  IResearchAnalyzerFeatureTest.cpp
  IResearchAqlAnalyzerTest.cpp
  IResearchComparerTest.cpp
  IResearchDocumentTest.cpp
  IResearchFeatureTest.cpp
  IResearchFilterArrayInTest.cpp
  IResearchFilterArrayIntervalTest.cpp
  IResearchFilterBooleanTest.cpp
  IResearchFilterCompareTest.cpp
  IResearchFilterFunctionTest.cpp
  IResearchFilterGeoTest.cpp
  IResearchFilterInTest.cpp
  IResearchFilterMinHashMatchTest.cpp
  IResearchFilterNestedTest.cpp
  IResearchIndexTest.cpp
  IResearchInvertedIndexConditionTest.cpp
  IResearchInvertedIndexIteratorTest.cpp
  IResearchInvertedIndexMetaTest.cpp
  IResearchLinkTest.cpp
  IResearchLinkCoordinatorTest.cpp
  IResearchLinkHelperTest.cpp
  IResearchLinkMetaTest.cpp
  IResearchOrderTest.cpp
  IResearchQueryAggregateTest.cpp
  IResearchQueryAndTest.cpp
  IResearchQueryBooleanTermTest.cpp
  IResearchQueryComplexBooleanTest.cpp
  IResearchQueryExistsTest.cpp
  IResearchQueryFilterTest.cpp
  IResearchQueryGeoIntersectsTest.cpp
  IResearchQueryGeoContainsTest.cpp
  IResearchQueryGeoDistanceTest.cpp
  IResearchQueryGeoInRangeTest.cpp
  IResearchQueryInTest.cpp
  IResearchQueryInRangeTest.cpp
  IResearchQueryJoinTest.cpp
  IResearchQueryLateMaterializationTest.cpp
  IResearchQueryMinMatchTest.cpp
  IResearchQueryNGramMatchTest.cpp
  IResearchQueryNoMaterializationTest.cpp
  IResearchQueryNullTermTest.cpp
  IResearchQueryNumericTermTest.cpp
  IResearchQueryOptimizationTest.cpp
  IResearchQueryOptionsTest.cpp
  IResearchQueryOrTest.cpp
  IResearchQueryPhraseTest.cpp
  IResearchQueryLevenshteinMatchTest.cpp
  IResearchQueryWildcardTest.cpp
  IResearchQueryScorerTest.cpp
  IResearchQuerySelectAllTest.cpp
  IResearchQueryStartsWithTest.cpp
  IResearchQueryStringTermTest.cpp
  IResearchQueryTokensTest.cpp
  IResearchQueryValueTest.cpp
  IResearchQueryTraversalTest.cpp
  IResearchPrimaryKeyReuseTest.cpp
  IResearchTestCommon.cpp
  IResearchViewTest.cpp
  IResearchViewCoordinatorTest.cpp
  IResearchViewDBServerTest.cpp
  IResearchViewMetaTest.cpp
  IResearchViewNodeTest.cpp
  IResearchViewSortTest.cpp
  IResearchViewSortedTest.cpp
  IResearchViewCountApproximateTest.cpp
  VelocyPackHelperTest.cpp)

if (USE_JEMALLOC)
  add_dependencies(arango_tests_iresearch jemalloc_build)
endif ()

target_link_libraries(arango_tests_iresearch
  arango_tests_basics
  arango
  arango_agency
  arango_cluster_engine
  arango_rocksdb
  arango_v8server
  arangoserver
  gtest
  boost_boost
  v8_interface)

add_executable(arangodbtests_iresearch
  EXCLUDE_FROM_ALL)

#required by fakeit dtor mock -- https://github.com/eranpeer/FakeIt/issues/84
if (CMAKE_CXX_COMPILER_ID STREQUAL GNU)
  target_compile_options(arangodbtests_iresearch PRIVATE -fno-devirtualize)
endif ()

target_link_libraries(arangodbtests_iresearch
  arango_tests_basics
  arango
  arango_agency
  arango_cluster_engine
  arango_rocksdb
  arango_v8server
  arangoserver
  boost_boost
  v8_interface
  arango_tests_iresearch)

add_test(NAME arangodbtests_iresearch
  COMMAND arangodbtests_iresearch)
