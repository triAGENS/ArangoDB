# -*- mode: CMAKE; -*-

################################################################################
## global definitions
################################################################################

add_compile_warnings_flags()

if (USE_GOOGLE_TESTS)
  add_definitions("-DTEST_VIRTUAL=virtual")
else()
  add_definitions("-DTEST_VIRTUAL=")
endif()

# used by 3rdParty/date, but must be set here because we include some headers
# of the date library from here
add_definitions("-DHAS_REMOTE_API=0")

# pass informational string with compiler optimization flags in as a define
# the value of the define is picked up by some code that prints out build info.
add_definitions("-DARCHITECTURE_OPTIMIZATIONS=\"${ARCHITECTURE_OPTIMIZATIONS}\"")

# add necessary defines for ICU. this will suffix all included ICU functions
# and the ICU namespace with _64_64
add_definitions("-DU_HAVE_LIB_SUFFIX=1")
add_definitions("-DU_LIB_SUFFIX_C_NAME=_64")
# also make sure that the correct ICU directory appears early in the list of include files
include_directories(BEFORE SYSTEM ${ICU64_INCLUDE_DIR})

################################################################################
## LIB_ARANGO
################################################################################

add_library(arango_static_strings STATIC
  Basics/StaticStrings.cpp
)

set(FROZEN_INCLUDE_DIR "${PROJECT_SOURCE_DIR}/3rdParty/iresearch/external/frozen/include")
set(FUNCTION2_INCLUDE_DIR "${PROJECT_SOURCE_DIR}/3rdParty/iresearch/external/function2")

add_library(arango_lightweight STATIC
        ApplicationFeatures/ApplicationFeature.cpp
        ApplicationFeatures/ApplicationServer.cpp
        ApplicationFeatures/LazyApplicationFeatureReference.h
        ApplicationFeatures/ShellColorsFeature.cpp
        Basics/ArangoGlobalContext.cpp
        Basics/CleanupFunctions.cpp
        Basics/ErrorCode.cpp
        Basics/Exceptions.cpp Basics/Exceptions.tpp Basics/Exceptions.h
        Basics/FileResult.cpp
        Basics/FileUtils.cpp
        Basics/Guarded.h
        Basics/Identifier.cpp
        Basics/InputProcessors.cpp
        Basics/MaintainerMode.h
        Basics/NumberOfCores.cpp
        Basics/PageSize.cpp
        Basics/PhysicalMemory.cpp
        Basics/ReadWriteLock.cpp
        Basics/RebootId.cpp Basics/RebootId.h
        Basics/Result.cpp
        Basics/ResultError.cpp Basics/ResultError.tpp Basics/ResultError.h
        Basics/StringBuffer.cpp
        Basics/StringHeap.cpp
        Basics/StringUtils.cpp
        Basics/Thread.cpp
        Basics/UnshackledMutex.cpp
        Basics/application-exit.cpp
        Basics/conversions.cpp
        Basics/debugging.cpp
        Basics/error.cpp
        Basics/files.cpp
        Basics/fpconv.cpp
        Basics/levenshtein.cpp
        Basics/memory.cpp
        Basics/process-utils.cpp
        Basics/signals.cpp
        Basics/SourceLocation.cpp
        Basics/system-functions.cpp
        Basics/terminal-utils-posix.cpp
        Basics/threads-posix.cpp
        Basics/tri-strings.cpp
        Containers/ImmerMemoryPolicy.h
        Logger/Appenders.cpp
        Logger/Escaper.cpp
        Logger/LogAppender.cpp
        Logger/LogAppenderFile.cpp
        Logger/LogAppenderStdStream.cpp
        Logger/LogAppenderStream.cpp
        Logger/LogAppenderSyslog.cpp
        Logger/LogContext.cpp
        Logger/LogLevel.cpp
        Logger/LogThread.cpp
        Logger/LogTopic.cpp
        Logger/Logger.cpp
        Logger/LoggerFeature.cpp
        Logger/LoggerStream.cpp
        Logger/LogTimeFormat.cpp
        ProgramOptions/IniFileParser.cpp
        ProgramOptions/Option.cpp
        ProgramOptions/Parameters.cpp
        ProgramOptions/ProgramOptions.cpp
        ProgramOptions/Section.cpp
        ProgramOptions/Translator.cpp
        ProgramOptions/UnitsHelper.cpp
        Random/RandomGenerator.cpp
)

target_include_directories(arango_lightweight PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# some auto-generated headers live here, like "Basics/build.h"
target_include_directories(arango_lightweight PUBLIC ${CMAKE_CURRENT_BINARY_DIR})
target_include_directories(arango_lightweight PUBLIC
        ${FROZEN_INCLUDE_DIR}
        ${FUNCTION2_INCLUDE_DIR}
        )
# Rebuild generated error files so they are up-to-date when all
# dependent stuff is built
add_dependencies(arango_lightweight errorfiles)
add_dependencies(arango_lightweight exitcodefiles)
add_dependencies(arango_lightweight iculegacy_build)

target_link_libraries(arango_lightweight
  PUBLIC velocypack
         arango_inspection
         absl::flat_hash_set
         absl::synchronization
         arango_async
  PRIVATE date_interface
          fmt
          arango_assertions
          arango_static_strings
          arango_futures
          boost_boost
          boost_system)

# Enterprise
if (USE_ENTERPRISE)
  target_compile_definitions(arango_lightweight PUBLIC "-DUSE_ENTERPRISE=1")
  target_include_directories(arango_lightweight PUBLIC "${PROJECT_SOURCE_DIR}/${ENTERPRISE_INCLUDE_DIR}")
endif()

add_library(arango STATIC
  ${PROJECT_SOURCE_DIR}/3rdParty/date/src/tz.cpp
  ApplicationFeatures/ApplicationFeaturePhase.cpp
  ApplicationFeatures/BumpFileDescriptorsFeature.cpp
  ApplicationFeatures/CommunicationFeaturePhase.cpp
  ApplicationFeatures/ConfigFeature.cpp
  ApplicationFeatures/FileSystemFeature.cpp
  ApplicationFeatures/GreetingsFeature.cpp
  ApplicationFeatures/LanguageFeature.cpp
  ApplicationFeatures/OptionsCheckFeature.cpp
  ApplicationFeatures/ShutdownFeature.cpp
  ApplicationFeatures/TempFeature.cpp
  ApplicationFeatures/VersionFeature.cpp
  Assertions/AssertionConditionalLogger.cpp
  Assertions/AssertionLogger.cpp
  Basics/AttributeNameParser.cpp
  Basics/CpuUsageSnapshot.cpp
  Basics/DebugRaceController.cpp
  Basics/EncodingUtils.cpp
  Basics/FeatureFlags.cpp
  Basics/FileDescriptors.cpp
  Basics/FunctionUtils.cpp
  Basics/GlobalResourceMonitor.cpp
  Basics/GlobalSerialization.cpp
  Basics/HybridLogicalClock.cpp
  Basics/IndexIter.h
  Basics/Nonce.cpp
  Basics/NumberUtils.cpp
  Basics/ReadWriteSpinLock.cpp
  Basics/ResourceUsage.cpp
  Basics/RocksDBLogger.cpp
  Basics/RocksDBUtils.cpp
  Basics/StringBufferAdvanced.cpp
  Basics/Utf8Helper.cpp
  Basics/VelocyPackHelper.cpp
  Basics/csv.cpp
  Basics/datetime.cpp
  Basics/fasthash.cpp
  Basics/files-advanced.cpp
  Basics/hashes.cpp
  Basics/socket-utils.cpp
  Basics/terminal-utils.cpp
  Basics/tri-zip.cpp
  Containers/MerkleTree.cpp
  Containers/MerkleTreeHelpers.cpp
  Endpoint/Endpoint.cpp
  Endpoint/EndpointIp.cpp
  Endpoint/EndpointIpV4.cpp
  Endpoint/EndpointIpV6.cpp
  Endpoint/EndpointList.cpp
  Endpoint/EndpointSrv.cpp
  Endpoint/EndpointUnixDomain.cpp
  Maskings/AttributeMasking.cpp
  Maskings/Collection.cpp
  Maskings/MaskingFunction.cpp
  Maskings/Maskings.cpp
  Maskings/Path.cpp
  Maskings/RandomMask.cpp
  Maskings/RandomStringMask.cpp
  Random/RandomFeature.cpp
  Random/UniformCharacter.cpp
  Rest/CommonDefines.cpp
  Rest/GeneralRequest.cpp
  Rest/GeneralResponse.cpp
  Rest/HttpRequest.cpp
  Rest/HttpResponse.cpp
  Rest/PathMatch.h
  Rest/Version.cpp
  SimpleHttpClient/ClientConnection.cpp
  SimpleHttpClient/ConnectionCache.cpp
  SimpleHttpClient/GeneralClientConnection.cpp
  SimpleHttpClient/HttpResponseChecker.cpp
  SimpleHttpClient/SimpleHttpClient.cpp
  SimpleHttpClient/SimpleHttpResult.cpp
  SimpleHttpClient/SslClientConnection.cpp
  Ssl/SslFeature.cpp
  Ssl/SslInterface.cpp
  Ssl/ssl-helper.cpp
  Utilities/LinenoiseShell.cpp
  Utilities/LineEditor.cpp
  Utilities/NameValidator.cpp
  Utilities/ScriptLoader.cpp
  Utilities/ShellBase.cpp
  Zip/ioapi.cpp
  Zip/unzip.cpp
  Zip/zip.cpp
)

target_link_libraries(arango
  PUBLIC s2
         arango_build_id
         boost_system
         boost_boost
         arango_futures
         arango_static_strings
         arango_crashhandler
         velocypack
         fuerte
         linenoise-ng
         date_interface
         ${SYSTEM_LIBRARIES}
         ${ICU64_LIBS}
         absl::crc32c
         absl::flat_hash_set
         absl::flat_hash_map
         absl::node_hash_set
         absl::node_hash_map
         absl::synchronization
         absl::strings
         arango_lightweight)

if (USE_V8)
  target_link_libraries(arango PUBLIC ${V8_LIBS})
endif()

target_link_libraries(arango PUBLIC fmt::fmt)

# this is "."
target_include_directories(arango PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# some auto-generated headers live here, like "Basics/build.h"
target_include_directories(arango PUBLIC ${CMAKE_CURRENT_BINARY_DIR})
target_include_directories(arango PUBLIC ${BUILD_DIR})

target_include_directories(arango SYSTEM PUBLIC ${OPENSSL_INCLUDE_DIR})
target_include_directories(arango SYSTEM PUBLIC ${Lz4_INCLUDE_DIR})

target_link_libraries(arango PUBLIC rocksdb_interface)
target_link_libraries(arango PUBLIC zlib_interface)
if (USE_V8)
  target_link_libraries(arango PUBLIC v8_interface)
endif()

add_dependencies(arango v8_build)
add_dependencies(arango iculegacy_build)

add_dependencies(arango snappy)
target_link_libraries(arango PUBLIC snappy)

add_dependencies(arango lz4_static)
target_link_libraries(arango PUBLIC lz4_static)

if (USE_LIBUNWIND)
  target_link_libraries(arango PUBLIC ${LIBUNWIND_LIB})
  target_include_directories(arango PRIVATE "${LIBUNWIND_HOME}/include")
  add_dependencies(arango libunwind_build)
endif ()

# Rebuild generated error files so they are up-to-date when all
# dependent stuff is built
add_dependencies(arango errorfiles)
add_dependencies(arango exitcodefiles)

# Enterprise
if (USE_ENTERPRISE)
  target_compile_definitions(arango PUBLIC "-DUSE_ENTERPRISE=1")
  target_include_directories(arango PUBLIC "${PROJECT_SOURCE_DIR}/${ENTERPRISE_INCLUDE_DIR}")
endif()

add_subdirectory(Actor)
add_subdirectory(Async)
add_subdirectory(Futures)
add_subdirectory(Geo)
if (USE_V8)
  add_subdirectory(V8)
endif()
add_subdirectory(VelocypackUtils)
add_subdirectory(CrashHandler)
add_subdirectory(Assertions)
add_subdirectory(Inspection)
add_subdirectory(BuildId)
