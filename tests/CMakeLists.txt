add_compile_warnings_flags()

# ----------------------------------------
# Fuerte tests
# ----------------------------------------

add_subdirectory(Fuerte)

# ----------------------------------------
# Error tests
# ----------------------------------------

add_subdirectory(Assertions)
add_subdirectory(CrashHandler)
add_subdirectory(Errors)

add_subdirectory(Zkd)
add_subdirectory(Graph)
add_subdirectory(Replication2)
add_subdirectory(Futures)

# ----------------------------------------
# Link directories
# ----------------------------------------

foreach (LINK_DIR ${V8_LINK_DIRECTORIES})
  link_directories("${LINK_DIR}")
endforeach()

if (WINDOWS)
  set(RCFILE Maintenance/json.rc)
endif ()

add_library(arango_tests_basics OBJECT
  ${CMAKE_CURRENT_SOURCE_DIR}/Mocks/IResearchLinkMock.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/Mocks/IResearchInvertedIndexMock.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/Mocks/LogLevels.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/Mocks/PreparedResponseConnectionPool.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/Mocks/StorageEngineMock.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/Mocks/Servers.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/Mocks/MockGraph.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/Mocks/MockGraphProvider.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/Basics/icu-helper.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/IResearch/AgencyMock.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/IResearch/common.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/IResearch/ExpressionContextMock.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/IResearch/RestHandlerMock.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/Aql/VelocyPackHelper.cpp
  ../cmake/activeCodePage.manifest
  main.cpp)

target_include_directories(arango_tests_basics
  PUBLIC
    ${PROJECT_SOURCE_DIR}/arangod/
    ${PROJECT_SOURCE_DIR}/lib/
    ${PROJECT_SOURCE_DIR}/${ENTERPRISE_INCLUDE_DIR}
    ${PROJECT_SOURCE_DIR}/tests
    ${PROJECT_SOURCE_DIR}/tests/Mocks/)

target_link_libraries(arango_tests_basics gtest velocypack fmt fuerte arango_iresearch)

add_subdirectory(IResearch)


set(ARANGODB_TESTS_SOURCES
  Agency/ActiveFailoverTest.cpp
  Agency/AddFollowerTest.cpp
  Agency/CleanOutServerTest.cpp
  Agency/CleanUpLostCollectionTest.cpp
  Agency/FailedFollowerTest.cpp
  Agency/FailedLeaderTest.cpp
  Agency/FailedServerTest.cpp
  Agency/MoveShardTest.cpp
  Agency/NodeLoadTest.cpp
  Agency/NodeTest.cpp
  Agency/RemoveFollowerTest.cpp
  Agency/StoreTest.cpp
  Agency/StoreTestAPI.cpp
  Agency/SupervisionTest.cpp
  Agency/TransactionBuilderTests.cpp
  Aql/AqlCallListTest.cpp
  Aql/AqlExecutorTestCase.cpp
  Aql/AqlHelper.cpp
  Aql/AqlItemBlockInputRangeTest.cpp
  Aql/AqlItemBlockTest.cpp
  Aql/AqlItemRowPrinter.cpp
  Aql/AqlItemRowTest.cpp
  Aql/AqlShadowRowTest.cpp
  Aql/AqlValueMemoryLayoutTest.cpp
  Aql/AstNodeTest.cpp
  Aql/AstResourcesTest.cpp
  Aql/AttributeNamePathTest.cpp
  Aql/BitFunctionsTest.cpp
  Aql/BlockCollector.cpp
  Aql/CalculationExecutorTest.cpp
  Aql/CountCollectExecutorTest.cpp
  Aql/DateFunctionsTest.cpp
  Aql/FixedOutputExecutionBlockMock.cpp
  Aql/InRangeFunctionTest.cpp
  Aql/MinHashFunctionsTest.cpp
  Aql/JaccardFunctionTest.cpp
  Aql/LevenshteinMatchFunctionTest.cpp
  Aql/DependencyProxyMock.cpp
  Aql/DistinctCollectExecutorTest.cpp
  Aql/EngineInfoContainerCoordinatorTest.cpp
  Aql/EnumerateCollectionExecutorTest.cpp
  Aql/EnumerateListExecutorTest.cpp
  Aql/ExecutionBlockImplTest.cpp
  Aql/ExecutionNodeTest.cpp
  Aql/ExecutorTestHelper.cpp
  Aql/FilterExecutorTest.cpp
  Aql/GatherExecutorCommonTest.cpp
  Aql/HashedCollectExecutorTest.cpp
  Aql/IdExecutorTest.cpp
  Aql/IndexNodeTest.cpp
  Aql/InputRangeTest.cpp
  Aql/InsertExecutorTest.cpp
  Aql/EnumeratePathsExecutorTest.cpp
  Aql/EnumeratePathsNodeTest.cpp
  Aql/LimitExecutorTest.cpp
  Aql/MockTypedNode.cpp
  Aql/NgramMatchFunctionTest.cpp
  Aql/NgramSimilarityFunctionTest.cpp
  Aql/NgramPosSimilarityFunctionTest.cpp
  Aql/NodeWalkerTest.cpp
  Aql/NoResultsExecutorTest.cpp
  Aql/ProjectionsTest.cpp
  Aql/QueryCursorTest.cpp
  Aql/QueryHelper.cpp
  Aql/QueryLimitsTest.cpp
  Aql/RegisterPlanTest.cpp
  Aql/RemoteExecutorTest.cpp
  Aql/RemoveExecutorTest.cpp
  Aql/ReplaceExecutorTest.cpp
  Aql/ReturnExecutorTest.cpp
  Aql/RowFetcherHelper.cpp
  Aql/ScatterExecutorTest.cpp
  Aql/ShortestPathExecutorTest.cpp
  Aql/ShortestPathNodeTest.cpp
  Aql/SkipResultTest.cpp
  Aql/SortedCollectExecutorTest.cpp
  Aql/SortExecutorTest.cpp
  Aql/SortLimitTest.cpp
  Aql/SpliceSubqueryOptimizerRuleTest.cpp
  Aql/SplicedSubqueryIntegrationTest.cpp
  Aql/SubqueryEndExecutorTest.cpp
  Aql/SubqueryStartExecutorTest.cpp
  Aql/TestEmptyExecutorHelper.cpp
  Aql/TestLambdaExecutor.cpp
  Aql/TraversalNodeTest.cpp
  Aql/UpdateExecutorTest.cpp
  Aql/UpsertExecutorTest.cpp
  Aql/WaitingExecutionBlockMock.cpp
  Aql/WindowExecutorTest.cpp
  Aql/DecaysFunctionTest.cpp
  Aql/DistanceFunctionTest.cpp
  AsyncAgencyComm/AsyncAgencyCommTest.cpp
  Auth/UserManagerTest.cpp
  Auth/UserManagerClusterTest.cpp
  Cache/BucketState.cpp
  Cache/CachedValue.cpp
  Cache/FrequencyBuffer.cpp
  Cache/Manager.cpp
  Cache/Metadata.cpp
  Cache/MockScheduler.cpp
  Cache/PlainBucket.cpp
  Cache/PlainCache.cpp
  Cache/Rebalancer.cpp
  Cache/Table.cpp
  Cache/TransactionalBucket.cpp
  Cache/TransactionalCache.cpp
  Cache/TransactionalCacheVPackKeyHasher.cpp
  Cache/TransactionalStore.cpp
  Cache/TransactionManager.cpp
  Cache/TransactionsWithBackingStore.cpp
  Cluster/AgencyPathsTest.cpp
  Cluster/ClusterHelpersTest.cpp
  Cluster/ClusterInfoTest.cpp
  Cluster/RebootTrackerTest.cpp
  Cluster/QueryAnalyzerRevisionsTest.cpp
  Containers/EnumerateTest.cpp
  Containers/HashSetTest.cpp
  Containers/MerkleTreeTest.cpp
  Containers/SmallVectorTest.cpp
  Errors/ErrorTTest.cpp
  Geo/GeoConstructorTest.cpp
  Geo/GeoFunctionsTest.cpp
  Geo/GeoJsonTest.cpp
  Geo/NearUtilsTest.cpp
  Geo/ShapeContainerTest.cpp
  HotBackup/HotBackupCoordinatorTest.cpp
  Logger/EscaperTest.cpp
  Logger/LogContextTest.cpp
  Maintenance/MaintenanceFeatureTest.cpp
  Maintenance/MaintenanceRestHandlerTest.cpp
  Maintenance/MaintenanceTest.cpp
  ${RCFILE}
  Metrics/MetricsTest.cpp
  Metrics/MetricsFeatureTest.cpp
  Metrics/MetricsServerTest.cpp
  Network/ConnectionPoolTest.cpp
  Network/MethodsTest.cpp
  Network/UtilsTest.cpp
  ProgramOptions/InifileParserTest.cpp
  ProgramOptions/ParametersTest.cpp
  Replication/ReplicationClientsProgressTrackerTest.cpp
  Rest/HttpRequestTest.cpp
  Rest/PathMatchTest.cpp
  RestHandler/RestAnalyzerHandlerTest.cpp
  RestHandler/RestDocumentHandlerTest.cpp
  RestHandler/RestUsersHandlerTest.cpp
  RestHandler/RestViewHandlerTest.cpp
  RestHandler/VstResponseTest.cpp
  RestServer/FlushFeatureTest.cpp
  RestServer/LanguageFeatureTest.cpp
  Restore/CollectionRestoreOrder.cpp
  RocksDBEngine/CachedCollectionNameTest.cpp
  RocksDBEngine/ChecksumCalculatorTest.cpp
  RocksDBEngine/ChecksumHelperTest.cpp
  RocksDBEngine/EndianTest.cpp
  RocksDBEngine/KeyTest.cpp
  RocksDBEngine/HotBackupTest.cpp
  RocksDBEngine/EncryptionProviderTest.cpp
  RocksDBEngine/IndexEstimatorTest.cpp
  RocksDBEngine/TransactionManagerTest.cpp
  Sharding/ShardDistributionReporterTest.cpp
  SimpleHttpClient/HttpResponseCheckerTest.cpp
  SimpleHttpClient/ConnectionCacheTest.cpp
  StorageEngine/PhysicalCollectionTest.cpp
  Transaction/ContextTest.cpp
  Transaction/CountCacheTest.cpp
  Transaction/ManagerTest.cpp
  Transaction/RestTransactionHandlerTest.cpp
  Utils/CollectionNameResolverTest.cpp
  Utils/NameValidatorTest.cpp
  Utils/QuickGen.cpp
  V8Server/V8AnalyzersTest.cpp
  V8Server/V8UsersTest.cpp
  V8Server/V8ViewsTest.cpp
  VocBase/ComputedValuesTest.cpp
  VocBase/KeyGeneratorTest.cpp
  VocBase/LogicalDataSourceTest.cpp
  VocBase/LogicalViewTest.cpp
  VocBase/VersionTest.cpp
  VocBase/VocbaseTest.cpp
  VPackDeserializer/BasicTests.cpp
  Cluster/ShardAutoRebalancerTest.cpp)

if (LINUX)
  # add "-fno-var-tracking" to the compiler flags
  # this speeds up the compilation with optimizations and newer g++ versions
  # and prevents the "variable tracking size limit exceeded warnings" from
  # occurring
  if (CMAKE_COMPILER_IS_GNUCC)
    # also turn off super-expensive global common subexpression elimination for tests
    set_source_files_properties(${ARANGODB_TESTS_SOURCES} PROPERTIES COMPILE_FLAGS "-fno-var-tracking -fno-gcse")
  else ()
    set_source_files_properties(${ARANGODB_TESTS_SOURCES} PROPERTIES COMPILE_FLAGS -fno-var-tracking)
  endif ()
endif ()

add_executable(arangodbtests
  ${ARANGODB_TESTS_SOURCES})

add_subdirectory(Basics)
add_subdirectory(Inspection)

# This is the default from the Jenkins setup
add_test(NAME arangodbtests
  COMMAND arangodbtests --gtest_output=xml --gtest_filter=-*_LongRunning)

# Run all tests
add_test(NAME arangodbtests-full
  COMMAND arangodbtests --gtest_output=xml)

# An attempt at partitioning tests cases
# for CircleCI
add_test(NAME arangodbtests-iresearch
  COMMAND arangodbtests --gtest_output=xml --gtest_filter=IResearch*)

add_test(NAME arangodbtests-non-iresearch
  COMMAND arangodbtests --gtest_output=xml --gtest_filter=-*_LongRunning:IResearch*)


#required by fakeit dtor mock -- https://github.com/eranpeer/FakeIt/issues/84
if(CMAKE_CXX_COMPILER_ID STREQUAL GNU)
  target_compile_options(arangodbtests PRIVATE -fno-devirtualize)
endif()

target_link_libraries(arangodbtests
  arango
  arango_tests_basics
  arango_tests_replication2
  arango_tests_graph
  arango_tests_iresearch
  arango_tests_futures
  arango_tests_zkd
  arango_tests_pregel
  arango_tests_pregel_graphstore
  arango_tests_actor
  arango_tests_inspection
  arango_inspection_test_helper
  arango_agency
  arango_cluster_engine
  arango_rocksdb
  arango_v8server
  arangoserver
  arango_restore
  fuerte
  rocksdb
  snappy
  gtest
  boost_boost
  fmt
  velocypack_utils
)

if (USE_ENTERPRISE)
  target_link_libraries(arangodbtests arango_rclone)
endif ()

target_include_directories(arangodbtests PRIVATE
  ${PROJECT_SOURCE_DIR}/arangod
  ${PROJECT_SOURCE_DIR}/${ENTERPRISE_INCLUDE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/Mocks/
)

# add these includes as system includes because otherwise
# the compiler will emit warnings for fakeit.hpp
target_include_directories(arangodbtests SYSTEM PRIVATE
  ${V8_INTERNAL_INCLUDE_DIR}
  ${CMAKE_SOURCE_DIR}/3rdParty/fakeit-gtest
)

target_link_libraries(arangodbtests v8_interface)

if (USE_JEMALLOC)
  add_dependencies(arangodbtests jemalloc_build)
endif ()

find_package(OpenSSL REQUIRED)
list(APPEND IRESEARCH_LIB_RESOURCES
  "${LIB_EAY_DEBUG_DLL}"
  "${SSL_EAY_DEBUG_DLL}"
  "${LIB_EAY_RELEASE_DLL}"
  "${SSL_EAY_RELEASE_DLL}"
)

foreach(ELEMENT ${IRESEARCH_LIB_RESOURCES})
  if (ELEMENT)
    add_custom_command(
      TARGET arangodbtests POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E echo "copying library resource:" "${ELEMENT}" " -> " "$<TARGET_FILE_DIR:arangodbtests>"
      COMMAND cp -lf ${ELEMENT} $<TARGET_FILE_DIR:arangodbtests> || ${CMAKE_COMMAND} -E copy ${ELEMENT} $<TARGET_FILE_DIR:arangodbtests>
    )
  endif()
endforeach()

add_subdirectory(Pregel)
add_subdirectory(sepp)
