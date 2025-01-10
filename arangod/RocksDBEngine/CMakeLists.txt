add_library(arango_rocksdb STATIC
  RocksDBBackgroundThread.cpp
  ReplicatedRocksDBTransactionCollection.cpp
  RocksDBBuilderIndex.cpp
  RocksDBChecksumEnv.cpp
  RocksDBCollection.cpp
  RocksDBColumnFamilyManager.cpp
  RocksDBCommon.cpp
  RocksDBComparator.cpp
  RocksDBCuckooIndexEstimator.cpp
  RocksDBDumpContext.cpp
  RocksDBDumpManager.cpp
  RocksDBEdgeIndex.cpp
  RocksDBEngine.cpp
  RocksDBFormat.cpp
  RocksDBFulltextIndex.cpp
  RocksDBGeoIndex.cpp
  RocksDBIncrementalSync.cpp
  RocksDBIndex.cpp
  RocksDBIndexCacheRefillFeature.cpp
  RocksDBIndexCacheRefillThread.cpp
  RocksDBIndexFactory.cpp
  RocksDBIndexingDisabler.cpp
  RocksDBIterators.cpp
  RocksDBKey.cpp
  RocksDBKeyBounds.cpp
  RocksDBLogValue.cpp
  RocksDBMetaCollection.cpp
  RocksDBMetadata.cpp
  RocksDBMethods.cpp
  RocksDBMethodsMemoryTracker.cpp
  RocksDBMultiDimIndex.cpp
  RocksDBOptimizerRules.cpp
  RocksDBOptionFeature.cpp
  RocksDBOptionsProvider.cpp
  RocksDBPrimaryIndex.cpp
  RocksDBRateLimiterThread.cpp
  RocksDBRecoveryManager.cpp
  RocksDBReplicationCommon.cpp
  RocksDBReplicationContext.cpp
  RocksDBReplicationContextGuard.cpp
  RocksDBReplicationIterator.cpp
  RocksDBReplicationManager.cpp
  RocksDBReplicationTailing.cpp
  RocksDBRestCollectionHandler.cpp
  RocksDBRestHandlers.cpp
  RocksDBRestReplicationHandler.cpp
  RocksDBRestWalHandler.cpp
  RocksDBSavePoint.cpp
  RocksDBSettingsManager.cpp
  RocksDBSortedRowsStorageContext.cpp
  RocksDBSyncThread.cpp
  RocksDBTempStorage.cpp
  RocksDBTransactionCollection.cpp
  RocksDBTransactionMethods.cpp
  RocksDBTransactionState.cpp
  RocksDBTtlIndex.cpp
  RocksDBTypes.cpp
  RocksDBUpgrade.cpp
  RocksDBVPackIndex.cpp
  RocksDBValue.cpp
  RocksDBVectorIndex.cpp
  RocksDBWalAccess.cpp
  ReplicatedRocksDBTransactionState.cpp
  SimpleRocksDBTransactionState.cpp
  SortedRowsStorageBackendRocksDB.cpp)

if (USE_V8) 
  target_sources(arango_rocksdb PRIVATE
    RocksDBV8Functions.cpp)
endif()

target_link_libraries(arango_rocksdb
  arango_cache
  arango_common_rest_handler
  arango_geo
  arango_health
  arango_indexes
  arango_restart_action
  arango_storage_engine_common
  arango::validation
  arango_zkd
  boost_boost
  rocksdb
  rocksdb_interface
  snappy
  faiss)

set(SNAPPY_INCLUDE_DIR "${SNAPPY_SOURCE_DIR};${SNAPPY_BUILD_DIR}" CACHE PATH "relation to snappy")
target_include_directories(arango_rocksdb PUBLIC ${SNAPPY_INCLUDE_DIR})

if (USE_ENTERPRISE)
  # this is required for hotbackup. Views need to be flushed.
  target_include_directories(arango_rocksdb PUBLIC
    ${IRESEARCH_INCLUDE})
  target_link_libraries(arango_rocksdb
    arango_rclone 
  )
endif()

if ((CMAKE_COMPILER_IS_GNUCC OR CMAKE_COMPILER_IS_CLANG) AND ARCH_AMD64)
  target_compile_options(arango_rocksdb PUBLIC "-maes")
endif ()

## these defines are copied from rocksdb's CMakeLists.txt
##  because we are currently using some of their internal headers
##  in our build
if(CMAKE_SYSTEM_NAME MATCHES "Cygwin")
  target_compile_options(arango_rocksdb PRIVATE -fno-builtin-memcmp -DCYGWIN)
elseif(CMAKE_SYSTEM_NAME MATCHES "Darwin")
  target_compile_options(arango_rocksdb PRIVATE -DOS_MACOSX)
elseif(CMAKE_SYSTEM_NAME MATCHES "Linux")
  target_compile_options(arango_rocksdb PRIVATE -DOS_LINUX)
elseif(CMAKE_SYSTEM_NAME MATCHES "SunOS")
  target_compile_options(arango_rocksdb PRIVATE -DOS_SOLARIS)
elseif(CMAKE_SYSTEM_NAME MATCHES "FreeBSD")
  target_compile_options(arango_rocksdb PRIVATE -DOS_FREEBSD)
elseif(CMAKE_SYSTEM_NAME MATCHES "NetBSD")
  target_compile_options(arango_rocksdb PRIVATE -DOS_NETBSD)
elseif(CMAKE_SYSTEM_NAME MATCHES "OpenBSD")
  target_compile_options(arango_rocksdb PRIVATE -DOS_OPENBSD)
elseif(CMAKE_SYSTEM_NAME MATCHES "DragonFly")
  target_compile_options(arango_rocksdb PRIVATE -DOS_DRAGONFLYBSD)
elseif(CMAKE_SYSTEM_NAME MATCHES "Android")
  target_compile_options(arango_rocksdb PRIVATE -DOS_ANDROID)
elseif(CMAKE_SYSTEM_NAME MATCHES "Windows")
  target_compile_options(arango_rocksdb PRIVATE -DWIN32 -DOS_WIN -D_MBCS -DWIN64 -DNOMINMAX)
  if(MINGW)
    target_compile_options(arango_rocksdb PRIVATE -D_WIN32_WINNT=_WIN32_WINNT_VISTA)
  endif()
endif()

target_include_directories(arango_rocksdb PRIVATE
  "${PROJECT_SOURCE_DIR}/arangod"
  "${PROJECT_SOURCE_DIR}/${ENTERPRISE_INCLUDE_DIR}")

add_subdirectory(Listeners)
add_subdirectory(Methods)
