include_directories(.)


# ----------------------------------------
# Fuerte tests
# ----------------------------------------

set(FUERTE_TESTS_SOURCES
  Fuerte/test_vst.cpp
  Fuerte/test_connection_basic.cpp
  Fuerte/test_connection_concurrent.cpp
  Fuerte/test_connection_failures.cpp
  Fuerte/test_connection_timeouts.cpp
#    test_10000_writes.cpp
)

add_executable(fuertetest
  ${FUERTE_TESTS_SOURCES}
  Fuerte/main.cpp
)

target_link_libraries(fuertetest
    fuerte
    gtest
    boost_boost
    arango
    arango_v8server
    ${MSVC_LIBS}
    ${OPENSSL_LIBRARIES}
    ${CMAKE_THREAD_LIBS_INIT}
)

target_include_directories(fuertetest SYSTEM PRIVATE
  ${V8_INTERNAL_INCLUDE_DIR}
  ${INCLUDE_DIRECTORIES}
  ${CMAKE_SOURCE_DIR}/3rdParty/fakeit-gtest
  ${CMAKE_SOURCE_DIR}/3rdParty/rocksdb/${ARANGO_ROCKSDB_VERSION}/third-party/gtest-1.8.1/fused-src
  ${CMAKE_SOURCE_DIR}/3rdParty/fuerte/src
)


# ----------------------------------------
# Link directories
# ----------------------------------------

foreach (LINK_DIR ${V8_LINK_DIRECTORIES})
  link_directories("${LINK_DIR}")
endforeach()

################################################################################
## IResearch
################################################################################
if (WINDOWS)
  set(RCFILE Maintenance/json.rc)
endif ()

set(ARANGODB_TESTS_SOURCES
  Agency/ActiveFailoverTest.cpp
  Agency/AddFollowerTest.cpp
  Agency/CleanOutServerTest.cpp
  Agency/CleanUpLostCollectionTest.cpp
  Agency/FailedFollowerTest.cpp
  Agency/FailedLeaderTest.cpp
  Agency/FailedServerTest.cpp
  Agency/MoveShardTest.cpp
  Agency/NodeTest.cpp
  Agency/RemoveFollowerTest.cpp
  Agency/StoreTest.cpp
  Agency/SupervisionTest.cpp
  Aql/BlockCollector.cpp
  Aql/AqlCallListTest.cpp
  Aql/AqlExecutorTestCase.cpp
  Aql/AqlHelper.cpp
  Aql/AqlItemBlockInputRangeTest.cpp
  Aql/AqlItemBlockTest.cpp
  Aql/AqlItemMatrixTest.cpp
  Aql/AqlItemRowPrinter.cpp
  Aql/AqlItemRowTest.cpp
  Aql/AqlShadowRowTest.cpp
  Aql/AttributeNamePathTest.cpp
  Aql/CalculationExecutorTest.cpp
  Aql/CountCollectExecutorTest.cpp
  Aql/DateFunctionsTest.cpp
  Aql/InRangeFunctionTest.cpp
  Aql/JaccardFunctionTest.cpp
  Aql/LevenshteinMatchFunctionTest.cpp
  Aql/DependencyProxyMock.cpp
  Aql/DistinctCollectExecutorTest.cpp
  Aql/EngineInfoContainerCoordinatorTest.cpp
  Aql/EnumerateCollectionExecutorTest.cpp
  Aql/EnumerateListExecutorTest.cpp
  Aql/ExecutionBlockImplTest.cpp
  Aql/ExecutionBlockImplTestInstances.cpp
  Aql/ExecutionNodeTest.cpp
  Aql/ExecutorTestHelper.cpp
  Aql/FilterExecutorTest.cpp
  Aql/GatherExecutorCommonTest.cpp
  Aql/HashedCollectExecutorTest.cpp
  Aql/IdExecutorTest.cpp
  Aql/IndexNodeTest.cpp
  Aql/InputRangeTest.cpp
  Aql/InsertExecutorTest.cpp
  Aql/KShortestPathsExecutorTest.cpp
  Aql/LimitExecutorTest.cpp
  Aql/MockTypedNode.cpp
  Aql/NgramMatchFunctionTest.cpp
  Aql/NgramSimilarityFunctionTest.cpp
  Aql/NgramPosSimilarityFunctionTest.cpp
  Aql/NoResultsExecutorTest.cpp
  Aql/ProjectionsTest.cpp
  Aql/QueryHelper.cpp
  Aql/ReadAllExecutionBlockTest.cpp
  Aql/RegisterPlanTest.cpp
  Aql/RemoteExecutorTest.cpp
  Aql/RemoveExecutorTest.cpp
  Aql/ReplaceExecutorTest.cpp
  Aql/ReturnExecutorTest.cpp
  Aql/RowFetcherHelper.cpp
  Aql/ScatterExecutorTest.cpp
  Aql/ShortestPathExecutorTest.cpp
  Aql/SkipResultTest.cpp
  Aql/SortedCollectExecutorTest.cpp
  Aql/SortExecutorTest.cpp
  Aql/SortLimit-test.cpp
  Aql/SpliceSubqueryOptimizerRuleTest.cpp
  Aql/SplicedSubqueryIntegrationTest.cpp
  Aql/SubqueryEndExecutorTest.cpp
  Aql/SubqueryStartExecutorTest.cpp
  Aql/TestEmptyExecutorHelper.cpp
  Aql/TestLambdaExecutor.cpp
  Aql/TraversalExecutorTest.cpp
  Aql/UpdateExecutorTest.cpp
  Aql/UpsertExecutorTest.cpp
  Aql/VelocyPackHelper.cpp
  Aql/WaitingExecutionBlockMock.cpp
  AsyncAgencyComm/AsyncAgencyCommTest.cpp
  Auth/UserManagerTest.cpp
  Basics/conversions-test.cpp
  Basics/csv-test.cpp
  Basics/datetime.cpp
  Basics/files-test.cpp
  Basics/fpconv-test.cpp
  Basics/hashes-test.cpp
  Basics/icu-helper.cpp
  Basics/json-test.cpp
  Basics/overload-test.cpp
  Basics/priorityqueue-test.cpp
  Basics/string-buffer-test.cpp
  Basics/string-test.cpp
  Basics/string-utf8-normalize-test.cpp
  Basics/string-utf8-test.cpp
  Basics/vector-test.cpp
  Basics/ApplicationServerTest.cpp
  Basics/AttributeNameParserTest.cpp
  Basics/CompileTimeStrlenTest.cpp
  Basics/EndpointTest.cpp
  Basics/FixedSizeAllocatorTest.cpp
  Basics/InifileParserTest.cpp
  Basics/LoggerTest.cpp
  Basics/RandomTest.cpp
  Basics/ReadWriteLockTest.cpp
  Basics/SpinLockTest.cpp
  Basics/StringBufferTest.cpp
  Basics/StringUtilsTest.cpp
  Basics/VelocyPackHelper-test.cpp
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
  Cache/TransactionalStore.cpp
  Cache/TransactionManager.cpp
  Cache/TransactionsWithBackingStore.cpp
  Cluster/AgencyPathsTest.cpp
  Cluster/ClusterHelpersTest.cpp
  Cluster/ClusterRepairsTest.cpp
  Cluster/RebootTrackerTest.cpp
  Cluster/QueryAnalyzerRevisions-test.cpp
  Containers/EnumerateTest.cpp
  Containers/HashSetTest.cpp
  Containers/MerkleTreeTest.cpp
  Futures/Future-test.cpp
  Futures/Promise-test.cpp
  Futures/Try-test.cpp
  Geo/GeoConstructorTest.cpp
  Geo/GeoFunctionsTest.cpp
  Geo/GeoJsonTest.cpp
  Geo/NearUtilsTest.cpp
  Geo/ShapeContainerTest.cpp
  Graph/ClusterTraverserCacheTest.cpp
  Graph/ConstantWeightShortestPathFinder.cpp
  Graph/GraphTestTools.cpp
  Graph/KShortestPathsFinder.cpp
  HotBackup/HotBackupCoordinatorTest.cpp
  IResearch/common.cpp
  IResearch/AgencyMock.cpp
  IResearch/Containers-test.cpp
  IResearch/ExpressionContextMock.cpp
  IResearch/ExpressionFilter-test.cpp
  IResearch/GeoAnalyzer-test.cpp
  IResearch/GeoFilter-test.cpp
  IResearch/GeoDistanceFilter-test.cpp
  IResearch/IResearchAnalyzerFeature-test.cpp
  IResearch/IResearchComparer-test.cpp
  IResearch/IResearchDocument-test.cpp
  IResearch/IResearchFeature-test.cpp
  IResearch/IResearchFilterArrayIn-test.cpp
  IResearch/IResearchFilterArrayInterval-test.cpp
  IResearch/IResearchFilterBoolean-test.cpp
  IResearch/IResearchFilterCompare-test.cpp
  IResearch/IResearchFilterFunction-test.cpp
  IResearch/IResearchFilterIn-test.cpp
  IResearch/IResearchIndex-test.cpp
  IResearch/IResearchLink-test.cpp
  IResearch/IResearchLinkCoordinator-test.cpp
  IResearch/IResearchLinkHelper-test.cpp
  IResearch/IResearchLinkMeta-test.cpp
  IResearch/IResearchOrder-test.cpp
  IResearch/IResearchQueryAggregate-test.cpp
  IResearch/IResearchQueryAnd-test.cpp
  IResearch/IResearchQueryBooleanTerm-test.cpp
  IResearch/IResearchQueryComplexBoolean-test.cpp
  IResearch/IResearchQueryExists-test.cpp
  IResearch/IResearchQueryFilter-test.cpp
  IResearch/IResearchQueryIn-test.cpp
  IResearch/IResearchQueryInRange-test.cpp
  IResearch/IResearchQueryJoin-test.cpp
  IResearch/IResearchQueryLateMaterialization-test.cpp
  IResearch/IResearchQueryMinMatch-test.cpp
  IResearch/IResearchQueryNGramMatch-test.cpp
  IResearch/IResearchQueryNoMaterialization-test.cpp
  IResearch/IResearchQueryNullTerm-test.cpp
  IResearch/IResearchQueryNumericTerm-test.cpp
  IResearch/IResearchQueryOptimization-test.cpp
  IResearch/IResearchQueryOptions-test.cpp
  IResearch/IResearchQueryOr-test.cpp
  IResearch/IResearchQueryPhrase-test.cpp
  IResearch/IResearchQueryLevenshteinMatch-test.cpp
  IResearch/IResearchQueryWildcard-test.cpp
  IResearch/IResearchQueryScorer-test.cpp
  IResearch/IResearchQuerySelectAll-test.cpp
  IResearch/IResearchQueryStartsWith-test.cpp
  IResearch/IResearchQueryStringTerm-test.cpp
  IResearch/IResearchQueryTokens-test.cpp
  IResearch/IResearchQueryValue-test.cpp
  IResearch/IResearchQueryTraversal-test.cpp
  IResearch/IResearchPrimaryKeyReuse-test.cpp
  IResearch/IResearchView-test.cpp
  IResearch/IResearchViewCoordinator-test.cpp
  IResearch/IResearchViewDBServer-test.cpp
  IResearch/IResearchViewMeta-test.cpp
  IResearch/IResearchViewNode-test.cpp
  IResearch/IResearchViewSort-test.cpp
  IResearch/IResearchViewSorted-test.cpp
  IResearch/RestHandlerMock.cpp
  IResearch/VelocyPackHelper-test.cpp
  Maintenance/MaintenanceFeatureTest.cpp
  Maintenance/MaintenanceRestHandlerTest.cpp
  Maintenance/MaintenanceTest.cpp
  ${RCFILE}
  Metrics/MetricsTest.cpp
  Metrics/MetricsFeatureTest.cpp
  Mocks/IResearchLinkMock.cpp
  Mocks/LogLevels.cpp
  Mocks/StorageEngineMock.cpp
  Mocks/Servers.cpp
  Network/ConnectionPoolTest.cpp
  Network/MethodsTest.cpp
  Network/UtilsTest.cpp
  Pregel/typedbuffer.cpp
  Replication/ReplicationClientsProgressTrackerTest.cpp
  RestHandler/RestAnalyzerHandler-test.cpp
  RestHandler/RestDocumentHandler-test.cpp
  RestHandler/RestUsersHandler-test.cpp
  RestHandler/RestViewHandler-test.cpp
  RestServer/FlushFeature-test.cpp
  RocksDBEngine/Endian.cpp
  RocksDBEngine/KeyTest.cpp
  RocksDBEngine/HotBackupTest.cpp
  RocksDBEngine/IndexEstimatorTest.cpp
  RocksDBEngine/ShaEventListener.cpp
  RocksDBEngine/TransactionManagerTest.cpp
  Sharding/ShardDistributionReporterTest.cpp
  StorageEngine/PhysicalCollectionTest.cpp
  Transaction/Context-test.cpp
  Transaction/Manager-test.cpp
  Transaction/RestTransactionHandler-test.cpp
  Utils/CollectionNameResolver-test.cpp
  V8Server/v8-analyzers-test.cpp
  V8Server/v8-users-test.cpp
  V8Server/v8-views-test.cpp
  VocBase/vocbase-test.cpp
  VocBase/LogicalDataSource-test.cpp
  VocBase/LogicalView-test.cpp
  VocBase/VersionTest.cpp
  VPackDeserializer/BasicTests.cpp)

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

add_executable(
  arangodbtests
  ${ARANGODB_TESTS_SOURCES}
  main.cpp
)

#required by fakeit dtor mock -- https://github.com/eranpeer/FakeIt/issues/84
if(CMAKE_CXX_COMPILER_ID STREQUAL GNU)
    target_compile_options(arangodbtests PRIVATE -fno-devirtualize)
endif()

target_link_libraries(arangodbtests
  arango_agency
  arango_cluster_engine
  arango_rocksdb
  arango_v8server
  arangoserver
  fuerte
  rocksdb
  snappy
  gtest
  boost_boost
)

if (USE_ENTERPRISE)
  target_link_libraries(arangodbtests arango_rclone)
endif()

target_include_directories(arangodbtests PRIVATE
  ${INCLUDE_DIRECTORIES}
)

if(MSVC AND FAIL_ON_WARNINGS)
  target_compile_options(arangodbtests PRIVATE /WX /D_WINSOCK_DEPRECATED_NO_WARNINGS)
endif()

if (DARWIN AND FAIL_ON_WARNINGS)
  # Missing Braces is broken in older CLangs - disable them here.
  target_compile_options(arangodbtests PRIVATE -Werror -Wno-error=deprecated-declarations -Wno-missing-braces)
endif()

# add these includes as system includes because otherwise
# the compiler will emit warnings for fakeit.hpp
target_include_directories(arangodbtests SYSTEM PRIVATE
  ${V8_INCLUDE_DIR}
  ${V8_INTERNAL_INCLUDE_DIR}
  ${CMAKE_SOURCE_DIR}/3rdParty/fakeit-gtest
  ${CMAKE_SOURCE_DIR}/3rdParty/rocksdb/${ARANGO_ROCKSDB_VERSION}/third-party/gtest-1.8.1/fused-src
)

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
  add_custom_command(
    TARGET arangodbtests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E echo "copying library resource:" "${ELEMENT}" " -> " "$<TARGET_FILE_DIR:arangodbtests>"
    COMMAND cp -lf ${ELEMENT} $<TARGET_FILE_DIR:arangodbtests> || ${CMAKE_COMMAND} -E copy ${ELEMENT} $<TARGET_FILE_DIR:arangodbtests>
  )
endforeach()
