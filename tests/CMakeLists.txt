add_compile_warnings_flags()

if (USE_GOOGLE_TESTS)
  add_definitions("-DTEST_VIRTUAL=virtual")
else()
  add_definitions("-DTEST_VIRTUAL=")
endif()

# add necessary defines for ICU. this will suffix all included ICU functions
# and the ICU namespace with _64_64
add_definitions("-DU_HAVE_LIB_SUFFIX=1")
add_definitions("-DU_LIB_SUFFIX_C_NAME=_64")
# also make sure that the correct ICU directory appears early in the list of include files
include_directories(BEFORE SYSTEM ${ICU64_INCLUDE_DIR})

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
add_subdirectory(BuildId)

# ----------------------------------------
# Link directories
# ----------------------------------------

if (USE_V8)
  foreach (LINK_DIR ${V8_LINK_DIRECTORIES})
    link_directories("${LINK_DIR}")
  endforeach()
endif ()

add_library(arango_tests_basics OBJECT
  ${CMAKE_CURRENT_SOURCE_DIR}/Mocks/ClusterInfoMock.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/Mocks/FakeScheduler.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/Mocks/FakeScheduler.h
  ${CMAKE_CURRENT_SOURCE_DIR}/Mocks/IResearchLinkMock.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/Mocks/IResearchInvertedIndexMock.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/Mocks/LogLevels.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/Mocks/PreparedResponseConnectionPool.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/Mocks/PhysicalCollectionMock.cpp
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
  ${ARANGODB_IRESEARCH_TESTS_SOURCES}
  ${RCFILE}
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
  Aql/AqlHelper.cpp
  Aql/AqlItemBlockInputRangeTest.cpp
  Aql/AqlItemBlockTest.cpp
  Aql/AqlItemRowPrinter.cpp
  Aql/AqlItemRowTest.cpp
  Aql/AqlShadowRowTest.cpp
  Aql/AqlValueCompare.cpp
  Aql/AqlValueMemoryLayoutTest.cpp
  Aql/AstNodeTest.cpp
  Aql/AstResourcesTest.cpp
  Aql/AttributeNamePathTest.cpp
  Aql/BlockCollector.cpp
  Aql/DependencyProxyMock.cpp
  Aql/EngineInfoContainerCoordinatorTest.cpp
  Aql/ExecutionBlockImplTest.cpp
  Aql/ExecutionNode/EnumeratePathsNodeTest.cpp
  Aql/ExecutionNode/ExecutionNodeTest.cpp
  Aql/ExecutionNode/IndexNodeTest.cpp
  Aql/ExecutionNode/MockTypedNode.cpp
  Aql/ExecutionNode/ShortestPathNodeTest.cpp
  Aql/ExecutionNode/TraversalNodeTest.cpp
  Aql/Executor/AqlExecutorTestCase.cpp
  Aql/Executor/AsyncExecutorTest.cpp
  Aql/Executor/CalculationExecutorTest.cpp
  Aql/Executor/CountCollectExecutorTest.cpp
  Aql/Executor/DistinctCollectExecutorTest.cpp
  Aql/Executor/EnumerateCollectionExecutorTest.cpp
  Aql/Executor/EnumerateListExecutorTest.cpp
  Aql/Executor/EnumeratePathsExecutorTest.cpp
  Aql/Executor/ExecutorTestHelper.cpp
  Aql/Executor/FilterExecutorTest.cpp
  Aql/Executor/GatherExecutorCommonTest.cpp
  Aql/Executor/HashedCollectExecutorTest.cpp
  Aql/Executor/IdExecutorTest.cpp
  Aql/Executor/InsertExecutorTest.cpp
  Aql/Executor/LimitExecutorTest.cpp
  Aql/Executor/NoResultsExecutorTest.cpp
  Aql/Executor/RemoteExecutorTest.cpp
  Aql/Executor/RemoveExecutorTest.cpp
  Aql/Executor/ReplaceExecutorTest.cpp
  Aql/Executor/ReturnExecutorTest.cpp
  Aql/Executor/ScatterExecutorTest.cpp
  Aql/Executor/ShortestPathExecutorTest.cpp
  Aql/Executor/SortExecutorTest.cpp
  Aql/Executor/SortedCollectExecutorTest.cpp
  Aql/Executor/SubqueryEndExecutorTest.cpp
  Aql/Executor/SubqueryStartExecutorTest.cpp
  Aql/Executor/TestEmptyExecutorHelper.cpp
  Aql/Executor/TestLambdaExecutor.cpp
  Aql/Executor/UpdateExecutorTest.cpp
  Aql/Executor/UpsertExecutorTest.cpp
  Aql/Executor/WindowExecutorTest.cpp
  Aql/FixedOutputExecutionBlockMock.cpp
  Aql/Function/BitFunctionsTest.cpp
  Aql/Function/DateFunctionsTest.cpp
  Aql/Function/DecaysFunctionTest.cpp
  Aql/Function/DistanceFunctionTest.cpp
  Aql/Function/InRangeFunctionTest.cpp
  Aql/Function/JaccardFunctionTest.cpp
  Aql/Function/LevenshteinMatchFunctionTest.cpp
  Aql/Function/MinHashFunctionsTest.cpp
  Aql/Function/NgramMatchFunctionTest.cpp
  Aql/Function/NgramPosSimilarityFunctionTest.cpp
  Aql/Function/NgramSimilarityFunctionTest.cpp
  Aql/InputRangeTest.cpp
  Aql/JoinStrategy/GenericAndTwoNonUniqueJoinTest.cpp
  Aql/JoinStrategy/TwoIndicesUniqueJoinTest.cpp
  Aql/NodeWalkerTest.cpp
  Aql/ParserTest.cpp
  Aql/ProjectionsTest.cpp
  Aql/QueryCursorTest.cpp
  Aql/QueryHelper.cpp
  Aql/QueryLimitsTest.cpp
  Aql/RegisterPlanTest.cpp
  Aql/RowFetcherHelper.cpp
  Aql/ShadowRowForwardingTest.cpp
  Aql/SkipResultTest.cpp
  Aql/SortLimitTest.cpp
  Aql/SpliceSubqueryOptimizerRuleTest.cpp
  Aql/SplicedSubqueryIntegrationTest.cpp
  Aql/WaitingExecutionBlockMock.cpp
  AsyncAgencyComm/AsyncAgencyCommTest.cpp
  Auth/UserManagerClusterTest.cpp
  Auth/UserManagerTest.cpp
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
  Cache/TransactionManager.cpp
  Cache/TransactionalBucket.cpp
  Cache/TransactionalCache.cpp
  Cache/TransactionalCacheVPackKeyHasher.cpp
  Cache/TransactionalStore.cpp
  Cache/TransactionsWithBackingStore.cpp
  Cluster/AgencyPathsTest.cpp
  Cluster/ClusterHelpersTest.cpp
  Cluster/ClusterInfoTest.cpp
  Cluster/EvenDistributionTest.cpp
  Cluster/PlanCollectionEntryTest.cpp
  Cluster/PlanCollectionToAgencyWriterTest.cpp
  Cluster/QueryAnalyzerRevisionsTest.cpp
  Cluster/RebootTrackerTest.cpp
  Cluster/ShardAutoRebalancerTest.cpp
  Cluster/ShardIDTest.cpp
  Cluster/ServerStateTest.cpp
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
  Logger/EscaperTest.cpp
  Logger/LogContextTest.cpp
  Maintenance/MaintenanceFeatureTest.cpp
  Maintenance/MaintenanceRestHandlerTest.cpp
  Maintenance/MaintenanceTest.cpp
  Metrics/MetricsFeatureTest.cpp
  Metrics/MetricsServerTest.cpp
  Metrics/MetricsTest.cpp
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
  RestServer/FlushFeatureTest.cpp
  RestServer/LanguageFeatureTest.cpp
  Restore/CollectionRestoreOrder.cpp
  RocksDBEngine/CachedCollectionNameTest.cpp
  RocksDBEngine/ChecksumCalculatorTest.cpp
  RocksDBEngine/ChecksumHelperTest.cpp
  RocksDBEngine/EncryptionProviderTest.cpp
  RocksDBEngine/EndianTest.cpp
  RocksDBEngine/IndexEstimatorTest.cpp
  RocksDBEngine/KeyTest.cpp
  RocksDBEngine/MemoryUsageTest.cpp
  RocksDBEngine/TransactionManagerTest.cpp
  Scheduler/EventCountTest.cpp
  Sharding/ShardDistributionReporterTest.cpp
  SimpleHttpClient/ConnectionCacheTest.cpp
  SimpleHttpClient/HttpResponseCheckerTest.cpp
  StorageEngine/PhysicalCollectionTest.cpp
  Transaction/ContextTest.cpp
  Transaction/CountCacheTest.cpp
  Transaction/ManagerTest.cpp
  Transaction/RestTransactionHandlerTest.cpp
  Utils/CollectionNameResolverTest.cpp
  Utils/NameValidatorTest.cpp
  Utils/QuickGen.cpp
  VocBase/ComputedValuesTest.cpp
  VocBase/KeyGeneratorTest.cpp
  VocBase/LogicalDataSourceTest.cpp
  VocBase/LogicalViewTest.cpp
  VocBase/VersionTest.cpp
  VocBase/VocbaseTest.cpp
)

if (USE_V8)
  set(ARANGODB_TESTS_SOURCES ${ARANGODB_TESTS_SOURCES}
    Scheduler/ThreadPoolTest.cpp
    V8Server/V8AnalyzersTest.cpp
    V8Server/V8UsersTest.cpp
    V8Server/V8ViewsTest.cpp)
endif()

if (USE_PERF_TESTS)
  set(ARANGODB_TESTS_SOURCES ${ARANGODB_TESTS_SOURCES}
    Scheduler/ThreadPoolPerfTest.cpp)
endif()
    
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

add_executable(arangodbtests_scheduler
   Scheduler/EventCountTest.cpp
   Scheduler/ThreadPoolTest.cpp
   Scheduler/ThreadPoolPerfTest.cpp)
target_link_libraries(arangodbtests_scheduler
   arangoserver
   arango_tests_basics
   gtest_main)

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
  arango_tests_async_registry_server
  arango_tests_basics
  arango_tests_replication2
  arango_tests_replication2_pure
  arango_tests_graph
  arango_tests_futures
  arango_tests_zkd
  arango_tests_actor
  arango_tests_inspection
  arango_inspection_test_helper
  arango_agency
  arango_cluster_engine
  arango_rocksdb
  arangoserver
  arango_restore
  clienttools_utils
  fuerte
  rocksdb
  snappy
  gtest
  boost_boost
  fmt
  velocypack_utils
)

if (USE_V8)
  target_link_libraries(arangodbtests arango_v8server)
endif ()

if (USE_ENTERPRISE)
  target_link_libraries(arangodbtests arango_rclone)
endif ()

add_dependencies(arangodbtests iculegacy_build)

if (USE_JEMALLOC)
  target_link_libraries(arangodbtests ${JEMALLOC_LIB})
  if (USE_JEMALLOC_PROF AND USE_LIBUNWIND)
    target_link_libraries(arangodbtests ${LIBUNWIND_LIB})
  endif ()
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
  ${CMAKE_SOURCE_DIR}/3rdParty/fakeit-gtest
)

if (USE_V8)
  target_include_directories(arangodbtests SYSTEM PRIVATE
    ${V8_INTERNAL_INCLUDE_DIR})
endif ()

if (USE_V8)
  target_link_libraries(arangodbtests v8_interface)
endif ()

if (NOT USE_PRECOMPILED_V8)
  add_dependencies(arangodbtests v8_build)
endif ()

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

add_subdirectory(Actor)
add_subdirectory(Async)
add_subdirectory(AsyncRegistryServer)
add_subdirectory(sepp)
add_subdirectory(VocBase/Properties)
