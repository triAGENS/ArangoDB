# -*- mode: CMAKE; -*-

# ------------------------------------------------------------------------------
# General
# ------------------------------------------------------------------------------

cmake_minimum_required(VERSION 3.21)
message(STATUS "CMake version: ${CMAKE_MAJOR_VERSION}.${CMAKE_MINOR_VERSION}.${CMAKE_PATCH_VERSION}")
set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)

if (NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release
      CACHE STRING
      "Choose the type of build, options are: None Debug Release RelWithDebInfo MinSizeRel."
      FORCE
  )
endif ()

if (NOT (CMAKE_BUILD_TYPE STREQUAL "Debug"
      OR CMAKE_BUILD_TYPE STREQUAL "Release"
      OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo"
      OR CMAKE_BUILD_TYPE STREQUAL "MinSizeRel"
      OR CMAKE_BUILD_TYPE STREQUAL "None"))

  message(FATAL_ERROR "expecting CMAKE_BUILD_TYPE: None Debug Release RelWithDebInfo MinSizeRel, got ${CMAKE_BUILD_TYPE}.")
endif ()

string(TOUPPER ${CMAKE_BUILD_TYPE} CMAKE_BUILD_TYPE_UPPER)

if (APPLE)
  if (NOT CMAKE_OSX_DEPLOYMENT_TARGET OR "${CMAKE_OSX_DEPLOYMENT_TARGET}" STREQUAL "")
    if (NOT DEFINED ENV{MACOSX_DEPLOYMENT_TARGET} OR "ENV{MACOSX_DEPLOYMENT_TARGET}" STREQUAL "")
      message(FATAL_ERROR "environment variable MACOSX_DEPLOYMENT_TARGET is missing")
    else ()
      set(CMAKE_OSX_DEPLOYMENT_TARGET "$ENV{MACOSX_DEPLOYMENT_TARGET}" CACHE STRING "deployment target for MacOSX; adjust to your system")
    endif ()
  else ()
    set($ENV{MACOSX_DEPLOYMENT_TARGET} ${CMAKE_OSX_DEPLOYMENT_TARGET})
  endif ()
endif ()

# where to find CMAKE modules
set(CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake ${CMAKE_MODULE_PATH})

option(SKIP_PACKAGING "" OFF)

# enable frontend build
option(USE_FRONTEND "build the browser-based frontend (requires yarn and nodejs)" ON)

# be verbose about flags used
option(VERBOSE "be verbose about flags used" OFF)

# treat warnings as errors on some platforms
option(USE_FAIL_ON_WARNINGS "treat warnings as errors" ON)

# use strictly required OpenSSL version (from ./VERSIONS file)
option(USE_STRICT_OPENSSL_VERSION "use strictly required OpenSSL version (from ./VERSIONS file)" OFF)

option(USE_MINIMAL_DEBUGINFO "use minimal debug symbols for builds containing debug information" OFF)

find_package(Git 1.5.3 REQUIRED)

# ------------------------------------------------------------------------------
# VERSION information
# ------------------------------------------------------------------------------

# stable release:   MAJOR.MINOR.PATCH
# hot fix:          MAJOR.MINOR.PATCH-FIXNUMBER
# unstable release: MAJOR.MINOR.PATCH-TYPE.NUMBER
# devel:            MAJOR.MINOR.0-devel
#
# These are mapped to the following variables:
#
# ARANGODB_VERSION_MAJOR = MAJOR
# ARANGODB_VERSION_MINOR = MINOR
# ARANGODB_VERSION_PATCH = PATCH
#
# for pre-releases, otherwise empty:
#
# ARANGODB_VERSION_PRELEASE_TYPE   = TYPE
# ARANGODB_VERSION_PRELEASE_NUMBER = NUMBER
#
set(ARANGODB_VERSION_MAJOR "3")
set(ARANGODB_VERSION_MINOR "12")

# when building the nightly ARANGODB_VERSION_PATCH will be set
if (NOT DEFINED ARANGODB_VERSION_PATCH)
  set(ARANGODB_VERSION_PATCH "0")
  set(ARANGODB_VERSION_RELEASE_TYPE "beta")
  set(ARANGODB_VERSION_RELEASE_NUMBER "1")
else()
  unset (ARANGODB_VERSION_RELEASE_TYPE) # do not remove space
  unset (ARANGODB_VERSION_RELEASE_NUMBER) # do not remove space
endif()

# unset TYPE and NUMBER in case they are empty
if (DEFINED ARANGODB_VERSION_RELEASE_TYPE)
  if (ARANGODB_VERSION_RELEASE_TYPE STREQUAL "")
    unset (ARANGODB_VERSION_RELEASE_TYPE) # do not remove space
    unset (ARANGODB_VERSION_RELEASE_NUMBER) # do not remove space
  endif()
else()
  unset (ARANGODB_VERSION_RELEASE_NUMBER) # do not remove space
endif()

if (DEFINED ARANGODB_VERSION_RELEASE_NUMBER)
  if (ARANGODB_VERSION_RELEASE_NUMBER STREQUAL "")
    unset (ARANGODB_VERSION_RELEASE_NUMBER) # do not remove space
  endif()
endif()

# semantic version
set(ARANGODB_PLAIN_VERSION "${ARANGODB_VERSION_MAJOR}.${ARANGODB_VERSION_MINOR}.${ARANGODB_VERSION_PATCH}")

if (DEFINED ARANGODB_VERSION_RELEASE_TYPE)
  if (DEFINED ARANGODB_VERSION_RELEASE_NUMBER)
    set(ARANGODB_VERSION "${ARANGODB_PLAIN_VERSION}-${ARANGODB_VERSION_RELEASE_TYPE}.${ARANGODB_VERSION_RELEASE_NUMBER}")
  else()
    set(ARANGODB_VERSION "${ARANGODB_PLAIN_VERSION}-${ARANGODB_VERSION_RELEASE_TYPE}")
  endif()
else()
  set(ARANGODB_VERSION "${ARANGODB_PLAIN_VERSION}")
endif()
set(ARANGODB_JS_VERSION "js")

message(STATUS "ARANGODB PLAIN VERSION: ${ARANGODB_PLAIN_VERSION}")
message(STATUS "ARANGODB VERSION: ${ARANGODB_VERSION}")
message(STATUS "ARANGODB JS VERSION: ${ARANGODB_JS_VERSION}")

################################################################################
# SNAP version
################################################################################

set(ARANGODB_SNAP_REVISION "1")

message(STATUS "SNAP REVISION: ${ARANGODB_SNAP_REVISION}")

# ------------------------------------------------------------------------------
#
# ------------------------------------------------------------------------------

project(arangodb3 LANGUAGES CXX C ASM VERSION ${ARANGODB_VERSION_MAJOR}.${ARANGODB_VERSION_MINOR})

# Enable CMake's Testing facilities.
#
# This enables you to use the add_test cmake command to add a test which is
# automatically run by executing ctest
enable_testing()

set(BUILD_SHARED_LIBS OFF)

# required for clang completion in editors - must be set after creating project
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if (USE_FAIL_ON_WARNINGS
    AND CMAKE_GENERATOR MATCHES "^Visual Studio"
    AND CMAKE_GENERATOR_TOOLSET STREQUAL "ClangCL")
  message(WARNING [[
    clang-cl is used with USE_FAIL_ON_WARNINGS (i.e. /WX).
    The build will probably fail, because system header includes aren't passed
    as system header includes to clang-cl through the VS toolchain.
  ]])
endif()

# Static executables:
option(STATIC_EXECUTABLES "produce static executables" OFF)
if (STATIC_EXECUTABLES)
  set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static")
  set(CMAKE_FIND_LIBRARY_SUFFIXES ".a")
endif()

# enable V8/JavaScript in executables. 
# note that V8 will always be built, regardless of the value of this option.
# this is required because the V8 build step also builds ICU, which is needed
# in all programs.
# if set to OFF, V8 will not be linked to arangod and the client-tools, 
# and V8 integration code for arangod will not be compiled. 
# additionally, arangosh will not be built.
# setting the option to OFF will further disable all JavaScript support in
# arangod, rendering the web UI, JavaScript transactions, AQL user-defined
# functions (UDFs), Foxx and some other smaller features unusable.
# setting this option to OFF also renders many tests unusable as many tests
# depend on JavaScript.
# setting the option to OFF is highly experimental option and not well tested.
# use at your own risk!
option(USE_V8 "build with V8/JavaScript support" ON)
if (USE_V8)
  add_definitions("-DUSE_V8=1")
endif ()

# enable dtrace
option(USE_DTRACE "enable dtrace probes" OFF)
if (USE_DTRACE)
  add_definitions("-DUSE_DTRACE=1")
endif ()

# enable code coverage
option(USE_COVERAGE "enable gcov code coverage" OFF)
if (USE_COVERAGE)
  add_compile_options(-fno-stack-protector -fprofile-arcs -ftest-coverage)
  add_link_options(--coverage)
  add_definitions("-DUSE_COVERAGE=1")
endif ()

# enable Enterprise Edition features
set(ENTERPRISE_INCLUDE_DIR "enterprise")
option(USE_ENTERPRISE "enable enterprise build" OFF)

# we want the following definitions to be in effect for both rocksdb and arangodb
add_definitions("-DNROCKSDB_THREAD_STATUS")
add_definitions("-DROCKSDB_SUPPORT_THREAD_LOCAL")

# for the packages
set(ARANGODB_PACKAGE_VENDOR  "ArangoDB GmbH")
set(ARANGODB_PACKAGE_CONTACT "info@arangodb.com")
set(ARANGODB_DISPLAY_NAME    "ArangoDB")
set(ARANGODB_URL_INFO_ABOUT  "https://www.arangodb.com")
set(ARANGODB_HELP_LINK       "https://docs.arangodb.com/${ARANGODB_VERSION_MAJOR}.${ARANGODB_VERSION_MINOR}/")
set(ARANGODB_CONTACT         "hackers@arangodb.com")
set(ARANGODB_FRIENDLY_STRING "ArangoDB - the native multi-model NoSQL database")


# binaries
set(BIN_ARANGOBENCH   arangobench)
set(BIN_ARANGOBACKUP  arangobackup)
set(BIN_ARANGOD       arangod)
set(BIN_ARANGODUMP    arangodump)
set(BIN_ARANGOEXPORT  arangoexport)
set(BIN_ARANGOIMPORT  arangoimport)
set(BIN_ARANGORESTORE arangorestore)
set(BIN_ARANGOSH      arangosh)
set(BIN_ARANGOVPACK   arangovpack)

# test binaries
set(BIN_ARANGODB_TESTS arangodbtests)
set(CLEAN_AUTOGENERATED_FILES)
set(PACKAGES_LIST)
set(COPY_PACKAGES_LIST)
set(CLEAN_PACKAGES_LIST)
set(INSTALL_CONFIGFILES_LIST)

# ------------------------------------------------------------------------------
# update files containing VERSION information
# ------------------------------------------------------------------------------

configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/lib/Basics/build.h.in"
  "${CMAKE_CURRENT_BINARY_DIR}/lib/Basics/build.h"
  NEWLINE_STYLE UNIX
)

option(ARANGODB_BUILD_DATE "Specific build date set from the outside (leave empty to auto-generate)" "")
if (ARANGODB_BUILD_DATE STREQUAL "")
  if (NOT EXISTS "${CMAKE_CURRENT_BINARY_DIR}/lib/Basics/build-date.h")
    # auto-generate build date
    string(TIMESTAMP ARANGODB_BUILD_DATE "%Y-%m-%d %H:%M:%S")
    set(GENERATE_BUILD_DATE ON)
  else ()
    # build-date.h file already exists. whatever is in there will be kept
    set(GENERATE_BUILD_DATE OFF)
  endif ()
else ()
  # forcefully recreate build-date.h file from provided date
  set(GENERATE_BUILD_DATE ON)
endif ()

if (NOT DEFINED GENERATE_BUILD_DATE OR GENERATE_BUILD_DATE)
  set(GENERATE_BUILD_DATE ON CACHE INTERNAL "whether we should generate the build date")
  configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/lib/Basics/build-date.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/lib/Basics/build-date.h"
    NEWLINE_STYLE UNIX
    )
else ()
  set(GENERATE_BUILD_DATE OFF CACHE INTERNAL "whether we should generate the build date")
endif ()

configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/lib/Basics/VERSION.in"
  "${CMAKE_CURRENT_SOURCE_DIR}/ARANGO-VERSION"
  NEWLINE_STYLE UNIX
)

################################################################################
## Find the git revision
################################################################################

function(determine_repository_version source_dir build_repository have_build_repository)
  # Get commit hash 
  execute_process(
    WORKING_DIRECTORY ${source_dir}
    COMMAND ${GIT_EXE} rev-parse --short HEAD
    OUTPUT_VARIABLE COMMIT_RAW
  )
  if (NOT COMMIT_RAW)
    message(FATAL_ERROR "Can't extract current commit with the command: 'git rev-parse --short HEAD'")
  endif()

  string(STRIP ${COMMIT_RAW} COMMIT_SHORT)

  if (NOT DEFINED BUILD_REPO_INFO OR BUILD_REPO_INFO STREQUAL "default")
    execute_process(
      WORKING_DIRECTORY ${source_dir}
      COMMAND ${GIT_EXE} branch --show-current
      OUTPUT_VARIABLE BRANCH_NAME_RAW)
    if (NOT BRANCH_NAME_RAW)
      # For example, in docker we do 'checkout'. Hence, it is impossible to detect branch
      set(${build_repository} "${COMMIT_SHORT}" PARENT_SCOPE)
      set(${have_build_repository} "1" PARENT_SCOPE)
    else()
      string(STRIP ${BRANCH_NAME_RAW} BRANCH_NAME)
      set(${build_repository} "refs/${BRANCH_NAME} ${COMMIT_SHORT}" PARENT_SCOPE)
      set(${have_build_repository} "1" PARENT_SCOPE)
    endif()
  elseif(BUILD_REPO_INFO STREQUAL "release")
    if ("${ARANGODB_VERSION_RELEASE_NUMBER}" STREQUAL "" AND ARANGODB_VERSION_RELEASE_TYPE MATCHES "^[1-9][0-9]*$")
      string(REPLACE "-" "." RELEASE_TAG ${ARANGODB_VERSION})
    else()
      set(RELEASE_TAG ${ARANGODB_VERSION})
    endif()
    set(RELEASE_TAG "v${RELEASE_TAG}")
    execute_process(
      WORKING_DIRECTORY ${source_dir}
      COMMAND ${GIT_EXE} describe --all --tags --match ${RELEASE_TAG}
      OUTPUT_VARIABLE TAG_RAW)
    if (NOT TAG_RAW)
      message(FATAL_ERROR "Can't extract tag using the command: 'git describe --all --tags --match v${ARANGODB_PLAIN_VERSION}")
    else()
      string(STRIP ${TAG_RAW} TAG)
      set(${build_repository} "refs/${TAG} ${COMMIT_SHORT}" PARENT_SCOPE)
      set(${have_build_repository} "1" PARENT_SCOPE)
    endif()
  elseif(BUILD_REPO_INFO STREQUAL "nightly")
    set(${build_repository} "refs/head/${ARANGODB_VERSION_MAJOR}.${ARANGODB_VERSION_MINOR} ${COMMIT_SHORT}" PARENT_SCOPE)
    set(${have_build_repository} "1" PARENT_SCOPE)
  else ()
    set(${build_repository} "GIT FAILED TO RETRIEVE THE VERSION - UNSUPPORTED BUILD MODE" PARENT_SCOPE)
    set(${have_build_repository} "1" PARENT_SCOPE)
  endif()
endfunction()

find_program (GIT_EXE git)
if (DEFINED GIT_EXE AND IS_DIRECTORY "${CMAKE_SOURCE_DIR}/.git")
  determine_repository_version(${CMAKE_SOURCE_DIR} ARANGODB_BUILD_REPOSITORY HAVE_ARANGODB_BUILD_REPOSITORY)
else()
  set(ARANGODB_BUILD_REPOSITORY "")
  set(HAVE_ARANGODB_BUILD_REPOSITORY "0")
endif()

if (DEFINED GIT_EXE AND USE_ENTERPRISE AND IS_DIRECTORY "${CMAKE_SOURCE_DIR}/enterprise/.git")
  determine_repository_version(${CMAKE_SOURCE_DIR}/enterprise ENTERPRISE_BUILD_REPOSITORY HAVE_ENTERPRISE_BUILD_REPOSITORY)
else ()
  set(ENTERPRISE_BUILD_REPOSITORY "")
  set(HAVE_ENTERPRISE_BUILD_REPOSITORY "0")
endif()

if (DEFINED OSKAR_BUILD_REPOSITITORY)
  set(HAVE_OSKAR_BUILD_REPOSITORY "1")
else ()
  set(OSKAR_BUILD_REPOSITORY "")
  set(HAVE_OSKAR_BUILD_REPOSITORY "0")
endif ()

configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/lib/Basics/build-repository.h.in"
  "${CMAKE_CURRENT_BINARY_DIR}/lib/Basics/build-repository.h"
  NEWLINE_STYLE UNIX
)

if (VERBOSE)
  message(STATUS "ARANGODB_BUILD_REPOSITORY=\"${ARANGODB_BUILD_REPOSITORY}\"")
  message(STATUS "ENTERPRISE_BUILD_REPOSITORY=\"${ENTERPRISE_BUILD_REPOSITORY}\"")
endif ()

################################################################################
## OPERATION SYSTEM
################################################################################

option(HOMEBREW
  "whether to install for homebrew"
  Off
)

if (UNIX AND NOT APPLE)
  if(CMAKE_SYSTEM_NAME MATCHES ".*Linux")
    set(LINUX TRUE)
  elseif (CMAKE_SYSTEM_NAME MATCHES "kFreeBSD.*")
    set(FREEBSD TRUE)
  elseif (CMAKE_SYSTEM_NAME MATCHES "kNetBSD.*|NetBSD.*")
    set(NETBSD TRUE)
  elseif (CMAKE_SYSTEM_NAME MATCHES "kOpenBSD.*|OpenBSD.*")
    set(OPENBSD TRUE)
  elseif (CMAKE_SYSTEM_NAME MATCHES ".*GNU.*")
    set(GNU TRUE)
  elseif (CMAKE_SYSTEM_NAME MATCHES ".*BSDI.*")
    set(BSDI TRUE)
  elseif (CMAKE_SYSTEM_NAME MATCHES "DragonFly.*|FreeBSD")
    set(FREEBSD TRUE)
  elseif (CMAKE_SYSTEM_NAME MATCHES "SYSV5.*")
    set(SYSV5 TRUE)
  endif ()
elseif (APPLE)
  if (CMAKE_SYSTEM_NAME MATCHES ".*Darwin.*")
    set(DARWIN TRUE)
  elseif (CMAKE_SYSTEM_NAME MATCHES ".*MacOS.*")
    set(DARWIN TRUE)
  endif ()
  find_program(BREW brew)
endif ()

# ------------------------------------------------------------------------------
# user options
# ------------------------------------------------------------------------------

if (APPLE)
  SET(USE_JEMALLOC OFF)
else ()
  option(
    USE_JEMALLOC
    "use jemalloc memory allocator"
    ON
  )
endif ()

if (LINUX)
  option(
    USE_LIBUNWIND
    "use libunwind for stack traces"
    ON
  )
else ()
  SET(USE_LIBUNWIND OFF)
endif ()

if (LINUX)
   option(USE_BUILD_ID_READER "Add code to read arangod's build-id from within arangod" ON)
else ()
   set(USE_BUILD_ID_READER OFF)
endif ()

if (USE_BUILD_ID_READER)
  add_definitions(-DUSE_BUILD_ID_READER=true)
else ()
  add_definitions(-DUSE_BUILD_ID_READER=false)
endif ()

# Guess whether we're using mold
execute_process(
  COMMAND ${CMAKE_CXX_COMPILER} ${CMAKE_EXE_LINKER_FLAGS} -Wl,--version
  OUTPUT_VARIABLE LINKER_VERSION_OUT)
if (LINKER_VERSION_OUT MATCHES ^mold)
  SET(LINKER_IS_MOLD TRUE)
else ()
  SET(LINKER_IS_MOLD FALSE)
endif ()

# mold doesn't support the linker scripts currently used for the build id reader, and will result in the error
#   mold: fatal: /home/tobias/Documents/ArangoDB/arangodb/arangodb/lib/BuildId/BuildId.ld:1: build_id_start = ADDR(.note.gnu.build-id);
#                                                                                    ^ unknown linker script token
# . Let's warn about this early, and how to work around it:
if (LINKER_IS_MOLD AND USE_BUILD_ID_READER)
  message(SEND_ERROR "It looks like you're using mold as a linker. That doesn't work together with USE_BUILD_ID_READER. Either disable USE_BUILD_ID_READER, or use another linker.")
endif ()

################################################################################
## EXTERNAL PROGRAMS
################################################################################

set(MAKE make)

find_package(PythonInterp 3 EXACT REQUIRED)
get_filename_component(PYTHON_EXECUTABLE "${PYTHON_EXECUTABLE}" REALPATH)

set($ENV{PYTHON_EXECUTABLE} ${PYTHON_EXECUTABLE})
execute_process(
    COMMAND ${PYTHON_EXECUTABLE} -c "from shutil import which"
    RESULT_VARIABLE EXIT_CODE
    OUTPUT_QUIET
    )
if (NOT "${EXIT_CODE}" EQUAL "0")
  message(FATAL_ERROR "python shutil.which package is required! ")
endif()

# FIXME the build containers seem to have a
# /usr/bin/ch(mod|own) to prevent the search
# to find those files the NO_DEFAULT_PATH
# argument is passed
find_program (MAKE make gmake)

find_program(
  CHMOD_EXECUTABLE chmod
  PATHS "/bin/" "/usr/bin/"
  NO_DEFAULT_PATH
)
message(STATUS "chmod found in ${CHMOD_EXECUTABLE}")
find_program(
  CHOWN_EXECUTABLE chown
  PATHS "/bin" "/usr/bin"
  NO_DEFAULT_PATH
)
message(STATUS "chown found in ${CHOWN_EXECUTABLE}")

################################################################################
## ARCHITECTURE
################################################################################

math(EXPR BITS "8*${CMAKE_SIZEOF_VOID_P}")

################################################################################
## COMPILER FEATURES
################################################################################

if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  set(CMAKE_COMPILER_IS_CLANG 1)
elseif (CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
  set(CMAKE_COMPILER_IS_CLANG 1)
endif ()

set(BASE_FLAGS     ""                                  CACHE STRING "base flags")
set(BASE_C_FLAGS   "${CMAKE_C_FLAGS}   $ENV{CFLAGS}"   CACHE STRING "base C flags")
set(BASE_CXX_FLAGS "${CMAKE_CXX_FLAGS} $ENV{CXXFLAGS}" CACHE STRING "base C++ flags")
set(BASE_LD_FLAGS                     "$ENV{LDFLAGS}"  CACHE STRING "base linker flags")
set(BASE_LIBS                         "$ENV{LIBS}"     CACHE STRING "base libraries")

include(CheckCompilerVersion)

CheckCompilerVersion(
  11.2 # GCC
  16.0 # Clang
)

if (CMAKE_COMPILER_IS_CLANG) 
  if (APPLE)
    if (NOT DEFINED ENV{MACOSX_DEPLOYMENT_TARGET} OR ENV{MACOSX_DEPLOYMENT_TARGET} STREQUAL "")
      set(BASE_CXX_FLAGS "${BASE_CXX_FLAGS} -stdlib=libc++")
    else ()
      set(BASE_C_FLAGS "${BASE_C_FLAGS} -mmacosx-version-min=$ENV{MACOSX_DEPLOYMENT_TARGET}")
      set(BASE_CXX_FLAGS "${BASE_CXX_FLAGS} -stdlib=libc++ -mmacosx-version-min=$ENV{MACOSX_DEPLOYMENT_TARGET}")
      set(BASE_LD_FLAGS "${BASE_LD_FLAGS} -mmacosx-version-min=$ENV{MACOSX_DEPLOYMENT_TARGET}")
    endif ()
    add_definitions("-Wno-deprecated-declarations")
  else ()
    if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS "10.0")
      message(WARNING "ArangoDB requires clang 10.0 or newer, building with older compiler versions is unsupported")
    endif()
    list(APPEND BASE_LIBS atomic)
  endif ()
endif ()

# need c++20
# XXX this should really be set on a per target level using cmake compile_features capabilities
set(CMAKE_CXX_STANDARD 20)
# turn off compiler language extensions (e.g. don't use -std=gnu++20, but
# -std=c++20).
set(CMAKE_CXX_EXTENSIONS OFF)

# need threads
find_package(Threads REQUIRED)

set(CMAKE_EXE_LINKER_FLAGS
  "${CMAKE_EXE_LINKER_FLAGS} ${BASE_LD_FLAGS}"
)

################################################################################
## TARGET ARCHITECTURE
################################################################################

include(OptimizeForArchitecture)
set(BASE_FLAGS "${BASE_FLAGS} ${ARCHITECTURE_OPTIMIZATIONS}")

################################################################################
## BACKTRACE
################################################################################

# iresearch uses backtrace, so we need to find and link libexecinfo
# for the case that we are on libmusl and not on glibc
find_package(Backtrace)

if (Backtrace_LIBRARY)
  set(BT_LIBS ${Backtrace_LIBRARY} CACHE PATH "Debug Helper libraries")
else()
  set(BT_LIBS "" CACHE PATH "Debug Helper libraries")
endif()

################################################################################
## ASSEMBLER OPTIMIZATIONS
################################################################################

# Allow to prohibit assembler optimization code explicitly
if (ARCH_AMD64)
  SET(ASM_OPTIMIZATIONS_DEFAULT ON)
else (ARCH_AMD64)
  SET(ASM_OPTIMIZATIONS_DEFAULT OFF)
endif (ARCH_AMD64)

option(ASM_OPTIMIZATIONS "whether hand-optimized assembler code should be used"
  ${ASM_OPTIMIZATIONS_DEFAULT})

if (ASM_OPTIMIZATIONS)
  add_definitions("-DASM_OPTIMIZATIONS=1")
else (ASM_OPTIMIZATIONS)
  add_definitions("-DASM_OPTIMIZATIONS=0")
endif (ASM_OPTIMIZATIONS)

################################################################################
## MAINTAINER MODE
################################################################################

option(USE_MAINTAINER_MODE
  "whether we want to have assertions and other development features"
  OFF
)

if (USE_MAINTAINER_MODE)
  add_definitions("-DIRESEARCH_DEBUG")

  add_definitions("-DARANGODB_ENABLE_MAINTAINER_MODE=1")
  if (CMAKE_COMPILER_IS_GNUCC AND NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_definitions("-D_FORTIFY_SOURCE=2")
  endif()

  find_package(FLEX)
  find_package(BISON)
  # these are required for generateREADME.sh
  find_program(FGREP_EXECUTABLE fgrep)
  find_program(SED_EXECUTABLE sed)
  find_program(AWK_EXECUTABLE awk)
endif ()

option(USE_GOOGLE_TESTS "Compile C++ unit tests" ON)
if (USE_GOOGLE_TESTS)
  add_definitions("-DARANGODB_USE_GOOGLE_TESTS=1")
endif()

include(debugInformation)
find_program(READELF_EXECUTABLE readelf)
detect_binary_id_type(CMAKE_DEBUG_FILENAMES_SHA_SUM)

################################################################################
## FAILURE TESTS
################################################################################

option(USE_FAILURE_TESTS
  "whether we want to have failure tests compiled in"
  OFF
)

if (USE_FAILURE_TESTS)
  add_definitions("-DARANGODB_ENABLE_FAILURE_TESTS=1")
endif ()

################################################################################
## INTERPROCEDURAL OPTIMIZATION (LINK TIME OPTIMIZATION)
################################################################################

set(USE_IPO AUTO CACHE STRING "Use interprocedural optimization: ON, OFF or AUTO")
set_property(CACHE USE_IPO PROPERTY STRINGS AUTO ON OFF)

set(IPO_ENABLED False)

# Determine value if IPO_ENABLED from USE_IPO and CMAKE_BUILD_TYPE
if (USE_IPO STREQUAL "AUTO")
  # When USE_IPO=AUTO, enable IPO for optimized / release builds.
  # But to work around a g++ segfault triggered by using both -flto and
  # -fno-devirtualize-functions, we disable IPO when using google tests, because
  # this will set no-devirtualize. See
  # https://gcc.gnu.org/bugzilla/show_bug.cgi?id=91387 and
  # https://gcc.gnu.org/bugzilla/show_bug.cgi?id=91375.
  # So this check may be removed later as soon as we use fixed gcc versions.
  # - Tobias, 2019-08-08
  if (NOT USE_GOOGLE_TESTS AND
    (CMAKE_BUILD_TYPE STREQUAL "Release"
    OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo"
    OR CMAKE_BUILD_TYPE STREQUAL "MinSizeRel"))
    set(IPO_ENABLED True)
  else()
    set(IPO_ENABLED False)
  endif ()
elseif (USE_IPO)
  set(IPO_ENABLED True)
else()
  set(IPO_ENABLED False)
endif()

message(STATUS "IPO_ENABLED: ${IPO_ENABLED}")
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ${IPO_ENABLED})

if (IPO_ENABLED)
  add_definitions("-DARANGODB_USE_IPO=1")
endif()

################################################################################
## LIBRARY RESOLV
################################################################################

set(SYS_LIBS ${SYS_LIBS} resolv)

if (NOT DARWIN)
  set(SYS_LIBS ${SYS_LIBS} rt)
endif ()

# ------------------------------------------------------------------------------
# IMPLICIT INCLUDES AND LIBRARY DIRECTORIES
# ------------------------------------------------------------------------------

function(CREATE_FLAGS OUTPUT GLUE)
  set(_TMP_RESULT "")

  foreach(arg ${ARGN})
    set(_TMP_RESULT "${_TMP_RESULT} ${GLUE}${arg}")
  endforeach()

  set(${OUTPUT} "${_TMP_RESULT}" PARENT_SCOPE)
endfunction()

# ------------------------------------------------------------------------------
# JEMALLOC
# ------------------------------------------------------------------------------

option(USE_JEMALLOC_PROF "use jemalloc profiler" ON)
option(USE_JEMALLOC_CHECKS "use jemalloc extended safety checks" ON)

if (USE_JEMALLOC)
  add_definitions("-DARANGODB_HAVE_JEMALLOC=1")
else ()
  # Must not compile in profiling stuff if we are not using JEMALLOC
  set(USE_JEMALLOC_PROF OFF)
endif ()

if (USE_JEMALLOC_PROF)
  add_definitions("-DUSE_MEMORY_PROFILE=1")
endif ()

# ------------------------------------------------------------------------------
# LIBUNWIND
# ------------------------------------------------------------------------------

if (USE_LIBUNWIND)
  add_definitions("-DARANGODB_HAVE_LIBUNWIND=1")
endif ()

# ------------------------------------------------------------------------------
# NDEBUG
# ------------------------------------------------------------------------------

add_definitions(-DNDEBUG)

################################################################################
## FLAGS
################################################################################

include(CheckCCompilerFlag)
include(CheckCXXCompilerFlag)

# Tests whether an argument can be passed to the C compiler without diagnostics,
# and appends it to the first variable if possible.
# If called with multiple arguments, they are tested and possibly appended
# independently.
function(add_c_flags_if_supported var)
  foreach(flag ${ARGN})
    set(flag_var_name "C_COMPILER_SUPPORTS_${flag}")
    # Note that this compiles a test file, also using CMAKE_C_FLAGS, which
    # may thus affect the result.
    # Also note that check_c_compiler_flag *internally caches* the result for
    # per *var_name*, forcing us to use a unique variable for each call with the
    # same option.
    # An alternative (albeit more costly) would be to unset the variable each time.
    check_c_compiler_flag(${flag} "${flag_var_name}")
    set(is_supported ${${flag_var_name}})
    if(is_supported)
      set(${var} "${${var}} ${flag}")
    endif()
  endforeach()
  set(${var} "${${var}}" PARENT_SCOPE)
endfunction()

# Tests whether an argument can be passed to the C++ compiler without diagnostics,
# and appends it to the first variable if possible.
# If called with multiple arguments, they are tested and possibly appended
# independently.
function(add_cxx_flags_if_supported var)
  foreach(flag ${ARGN})
    set(flag_var_name "CXX_COMPILER_SUPPORTS_${flag}")
    # Note that this compiles a test file, also using CMAKE_CXX_FLAGS, which
    # may thus affect the result.
    # Also note that check_cxx_compiler_flag *internally caches* the result for
    # per *var_name*, forcing us to use a unique variable for each call with the
    # same option.
    # An alternative (albeit more costly) would be to unset the variable each time.
    check_cxx_compiler_flag(${flag} "${flag_var_name}")
    set(is_supported ${${flag_var_name}})
    if(is_supported)
      set(${var} "${${var}} ${flag}")
    endif()
  endforeach()
  set(${var} "${${var}}" PARENT_SCOPE)
endfunction()

function(add_compile_warnings_flags)
  if (USE_FAIL_ON_WARNINGS)
    add_compile_options(-Werror -Wno-error=deprecated-declarations)
    if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
      # This option has too many false positives
      add_compile_options(-Wno-error=maybe-uninitialized)
      # This prevents useful partial initializations
      add_compile_options(-Wno-missing-field-initializers)
    endif()
  endif ()
endfunction()

if (VERBOSE)
  message(STATUS)
endif ()


# compiler options
set(EXTRA_C_FLAGS "")
set(EXTRA_CXX_FLAGS "")

if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  message(STATUS "Compiler type GNU: ${CMAKE_CXX_COMPILER}")
  set(BASE_FLAGS "-Wall -Wextra -Wno-unused-parameter -Wno-deprecated-declarations ${BASE_FLAGS}")
  set(EXTRA_CXX_FLAGS "-Wsuggest-override -Wnon-virtual-dtor")
  if (CMAKE_CXX_COMPILER_VERSION VERSION_EQUAL "11.1.0")
    set(EXTRA_CXX_FLAGS "${EXTRA_CXX_FLAGS} -Wno-error=nonnull")
  endif()
  if (CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL "13.0.0")
    # the following warning types need to be suppressed with newer
    # versions of g++, as they produce a lot of false positives.
    # if the no-error directives are removed, a lot 3rdParty libraries
    # such as date, fmt, and immer cannot be compiled anymore.
    # https://gcc.gnu.org/bugzilla/buglist.cgi?bug_status=__open__&content=stringop-overread&no_redirect=1&product=gcc
    set(EXTRA_CXX_FLAGS "${EXTRA_CXX_FLAGS} -Wno-error=dangling-reference")
    set(EXTRA_CXX_FLAGS "${EXTRA_CXX_FLAGS} -Wno-error=array-bounds")
    set(EXTRA_CXX_FLAGS "${EXTRA_CXX_FLAGS} -Wno-error=overloaded-virtual")
    set(EXTRA_CXX_FLAGS "${EXTRA_CXX_FLAGS} -Wno-error=stringop-overflow")
    set(EXTRA_CXX_FLAGS "${EXTRA_CXX_FLAGS} -Wno-error=stringop-overread")
  endif()
elseif (CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  message(STATUS "Compiler type CLANG: ${CMAKE_CXX_COMPILER}")
  set(BASE_FLAGS "-Wall -Wextra -Wno-unused-parameter -Wno-deprecated-declarations ${BASE_FLAGS}")
  set(EXTRA_CXX_FLAGS "-Wnon-virtual-dtor")
  
  if (NOT DARWIN AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL "11.0")
    # clang 11 and higher supports -Wsuggest-override. older versions don't
    set(EXTRA_CXX_FLAGS "-Wsuggest-override ${EXTRA_CXX_FLAGS}")
  endif ()

  if (DARWIN AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL "12.0" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS "12.1")
    # With the introduction of apple clang version 12.0.0 (clang-1200.0.32.2) the warning "range-loop-analysis" is now
    # enabled by default or changed analysis behaviour. The compiler now complains about "wrong" usage of
    # VPackObjectIterator and VPackArrayIterator - which is not the case.
    #
    # See example here: https://github.com/arangodb/arangodb/pull/12730
    #
    # Since we're stopping compilation on Apple in any warning case, we need to temporary disable that warning and
    # check upcoming versions if the compiler behaviour will be fixed.
    set(EXTRA_C_FLAGS "-Wno-range-loop-analysis")
    set(EXTRA_CXX_FLAGS "${EXTRA_CXX_FLAGS} -Wno-range-loop-analysis")
  endif ()
else ()
  # unknown compiler
  message(STATUS "Compiler type UNKNOWN: ${CMAKE_CXX_COMPILER}")
  set(BASE_FLAGS "-Wall ${BASE_FLAGS}")
endif ()

# flags for builds without debug symbols
set(NODEBUGINFO_FLAGS "-g0")

# flags for builds with debug symbols
if (USE_MINIMAL_DEBUGINFO)
  # minimal debug symbols
  set(DEBUGINFO_FLAGS "-g1 -gno-column-info -gz")
else ()
  # full debug symbols
  set(DEBUGINFO_FLAGS "-g -gz")
endif ()

# c
# note: when building one of the build types, CMake will automatically combine
# the base flags from CMAKE_C_FLAGS with build type-specific flags in 
# CMAKE_C_FLAGS_${CMAKE_BUILD_TYPE}.
# there is no need to repeat the base flags in the build-type specific flags!
set(CMAKE_C_FLAGS                  ""                                                CACHE INTERNAL "default C compiler flags")
set(CMAKE_C_FLAGS_DEBUG            "${DEBUGINFO_FLAGS} -O0 -D_DEBUG=1"               CACHE INTERNAL "C debug flags")
set(CMAKE_C_FLAGS_MINSIZEREL       "${NODEBUGINFO_FLAGS} -Os"                        CACHE INTERNAL "C minimal size flags")
set(CMAKE_C_FLAGS_RELEASE          "${NODEBUGINFO_FLAGS} -O3 -fomit-frame-pointer"   CACHE INTERNAL "C release flags")
set(CMAKE_C_FLAGS_RELWITHDEBINFO   "${DEBUGINFO_FLAGS} -O3 -fno-omit-frame-pointer"  CACHE INTERNAL "C release with debug info flags")

# cxx
# note: when building one of the build types, CMake will automatically combine
# the base flags from CMAKE_CXX_FLAGS with build type-specific flags in 
# CMAKE_CXX_FLAGS_${CMAKE_BUILD_TYPE}.
# there is no need to repeat the base flags in the build-type specific flags!
set(CMAKE_CXX_FLAGS                ""                                                CACHE INTERNAL "default C++ compiler flags")
set(CMAKE_CXX_FLAGS_DEBUG          "${DEBUGINFO_FLAGS} -O0 -D_DEBUG=1"               CACHE INTERNAL "C++ debug flags")
set(CMAKE_CXX_FLAGS_MINSIZEREL     "${NODEBUGINFO_FLAGS} -Os"                        CACHE INTERNAL "C++ minimal size flags")
set(CMAKE_CXX_FLAGS_RELEASE        "${NODEBUGINFO_FLAGS} -O3 -fomit-frame-pointer"   CACHE INTERNAL "C++ release flags")
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "${DEBUGINFO_FLAGS} -O3 -fno-omit-frame-pointer"  CACHE INTERNAL "C++ release with debug info flags")

if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    # On Darwin -fstandalone-debug is enabled by default, so we don't need
    # to add it for AppleClang.
    set(CMAKE_CXX_FLAGS_DEBUG             "${CMAKE_CXX_FLAGS_DEBUG} -fstandalone-debug")
    set(CMAKE_CXX_FLAGS_RELWITHDEBINFO    "${CMAKE_CXX_FLAGS_RELWITHDEBINFO} -fstandalone-debug")
endif ()

# put together the final flags
set(CMAKE_C_FLAGS    "${BASE_FLAGS} ${BASE_C_FLAGS} ${CMAKE_C_FLAGS} ${EXTRA_C_FLAGS}")
set(CMAKE_CXX_FLAGS  "${BASE_FLAGS} ${BASE_CXX_FLAGS} ${CMAKE_CXX_FLAGS} ${EXTRA_CXX_FLAGS}")

if (VERBOSE)
  message(STATUS "Info BASE_FLAGS:     ${BASE_FLAGS}")
  message(STATUS "Info BASE_C_FLAGS:   ${BASE_C_FLAGS}")
  message(STATUS "Info BASE_CXX_FLAGS: ${BASE_CXX_FLAGS}")
  message(STATUS "Info BASE_LD_FLAGS:  ${BASE_LD_FLAGS}")
  message(STATUS "Info BASE_LIBS:      ${BASE_LIBS}")
  message(STATUS)

  message(STATUS "Info EXTRA_C_FLAGS:   ${EXTRA_C_FLAGS}")
  message(STATUS "Info EXTRA_CXX_FLAGS: ${EXTRA_CXX_FLAGS}")
  message(STATUS)

  if (NOT CMAKE_BUILD_TYPE STREQUAL "None")
    message(STATUS "Info CMAKE_C_FLAGS:   ${CMAKE_C_FLAGS}")
    message(STATUS "Info CMAKE_CXX_FLAGS: ${CMAKE_CXX_FLAGS}")
  endif ()
  message(STATUS "Info CMAKE_C_FLAGS_${CMAKE_BUILD_TYPE_UPPER}:   ${CMAKE_C_FLAGS_${CMAKE_BUILD_TYPE_UPPER}}")
  message(STATUS "Info CMAKE_CXX_FLAGS_${CMAKE_BUILD_TYPE_UPPER}: ${CMAKE_CXX_FLAGS_${CMAKE_BUILD_TYPE_UPPER}}")

  message(STATUS "Info CMAKE_EXE_LINKER_FLAGS: ${CMAKE_EXE_LINKER_FLAGS}")
  message(STATUS)
endif ()

if(ARANGODB_DEBUG_CMAKE)
  get_directory_property( DirDefs DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} COMPILE_DEFINITIONS )
  foreach( d ${DirDefs} )
    message( STATUS "Found Define: " ${d} )
  endforeach()
  message( STATUS "DirDefs: ${DirDefs}" )
endif()

################################################################################
## OpenSSL
################################################################################

if (NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/VERSIONS")
  message(FATAL_ERROR "expecting ${CMAKE_CURRENT_LIST_DIR}/VERSIONS")
else ()
  file(READ "${CMAKE_CURRENT_LIST_DIR}/VERSIONS" ARANGODB_VERSIONS_CONTENT)
  if (LINUX)
    set (TARGET_OS "LINUX")
  elseif (DARWIN)
    set (TARGET_OS "MACOS")
  endif ()

  if (USE_STRICT_OPENSSL_VERSION)
    set (OPENSSL_VERSION_PATTERN ".*OPENSSL_${TARGET_OS}[ ]+\"([^\"]*).*")
  else ()
    set (OPENSSL_VERSION_PATTERN ".*OPENSSL_${TARGET_OS}[ ]+\"([0-9]\.[0-9]\.[^0-9\"]*).*")
  endif ()
  string(REGEX MATCH
            "${OPENSSL_VERSION_PATTERN}"
            ARANGODB_REQUIRED_OPENSSL_VERSION
            "${ARANGODB_VERSIONS_CONTENT}")
  if ("${CMAKE_MATCH_1}" STREQUAL "")
    message(FATAL_ERROR "expecting OPENSSL_${TARGET_OS} in ${CMAKE_CURRENT_LIST_DIR}/VERSIONS")
  else ()
    set (ARANGODB_REQUIRED_OPENSSL_VERSION "${CMAKE_MATCH_1}")
    if (USE_STRICT_OPENSSL_VERSION)
      set (MSG_ARANGODB_REQUIRED_OPENSSL_VERSION "${ARANGODB_REQUIRED_OPENSSL_VERSION}")
    else ()
      set (MSG_ARANGODB_REQUIRED_OPENSSL_VERSION "${ARANGODB_REQUIRED_OPENSSL_VERSION}*")
    endif ()
    message ("Required OpenSSL version: ${MSG_ARANGODB_REQUIRED_OPENSSL_VERSION}")
  endif ()
endif ()


if (NOT DEFINED OPENSSL_ROOT_DIR OR "${OPENSSL_ROOT_DIR}" STREQUAL "")
  if (DEFINED ENV{OPENSSL_ROOT_DIR} AND NOT "$ENV{OPENSSL_ROOT_DIR}" STREQUAL "")
    set (OPENSSL_ROOT_DIR "$ENV{OPENSSL_ROOT_DIR}")
  endif ()
else ()
  set (ENV{OPENSSL_ROOT_DIR} "${OPENSSL_ROOT_DIR}")
endif ()

unset (OPENSSL_FOUND CACHE)
unset (OPENSSL_INCLUDE_DIR CACHE)
unset (OPENSSL_CRYPTO_LIBRARY CACHE)
unset (OPENSSL_SSL_LIBRARY CACHE)
unset (OPENSSL_LIBRARIES CACHE)
unset (OPENSSL_VERSION CACHE)

if (DEFINED OPENSSL_ROOT_DIR AND NOT "${OPENSSL_ROOT_DIR}" STREQUAL "")
  message ("Use OPENSSL_ROOT_DIR: ${OPENSSL_ROOT_DIR}")
endif ()

find_package(OpenSSL REQUIRED)

if (OPENSSL_FOUND AND USE_STRICT_OPENSSL_VERSION)
  if (NOT "${OPENSSL_VERSION}" MATCHES "${ARANGODB_REQUIRED_OPENSSL_VERSION}")
    message (FATAL_ERROR "Wrong OpenSSL version was found: ${OPENSSL_VERSION}! Required version: ${MSG_ARANGODB_REQUIRED_OPENSSL_VERSION}!")
  endif ()
endif ()

message(${OPENSSL_INCLUDE_DIR})

add_definitions(-DARANGODB_OPENSSL_VERSION=\"${OPENSSL_VERSION}\")
add_definitions(-DARANGODB_OPENSSL_VERSION_MAJOR=${OPENSSL_VERSION_MAJOR})
add_definitions(-DARANGODB_OPENSSL_VERSION_MINOR=${OPENSSL_VERSION_MINOR})

if (OPENSSL_VERSION)
  string(REPLACE "." ";" OPENSSL_VERSION_LIST ${OPENSSL_VERSION})
  list(GET OPENSSL_VERSION_LIST 0 OPENSSL_VERSION_MAJOR)
  list(GET OPENSSL_VERSION_LIST 1 OPENSSL_VERSION_MINOR)

  if ("${OPENSSL_VERSION_MAJOR}" GREATER 0 AND "${OPENSSL_VERSION_MINOR}" GREATER 0)
    option(USE_OPENSSL_NO_SSL2
      "do not use OPENSSL_NO_SSL2"
      ON
    )
  else ()
    option(USE_OPENSSL_NO_SSL2
      "do not use OPENSSL_NO_SSL2"
      OFF
    )
  endif ()
endif ()

if (USE_OPENSSL_NO_SSL2)
  add_definitions(-DOPENSSL_NO_SSL2)
endif ()

################################################################################
## 3RD PARTY
################################################################################

add_definitions(-DBOOST_UUID_RANDOM_PROVIDER_FORCE_POSIX=1)  # fix random provider
add_definitions(-DBOOST_ALL_NO_LIB=1)  # disable boost autolink on windows
add_subdirectory(3rdParty EXCLUDE_FROM_ALL)

add_definitions("-DARANGODB_BOOST_VERSION=\"${BOOST_VERSION}\"")

# ------------------------------------------------------------------------------
# RocksDB
# ------------------------------------------------------------------------------

add_dependencies(rocksdb snappy)

if (USE_JEMALLOC)
  add_dependencies(rocksdb jemalloc_build)
  link_directories("${JEMALLOC_HOME}/lib")
endif ()

################################################################################
## VELOCYPACK
################################################################################

add_definitions("-DVELOCYPACK_XXHASH=1")

set(V8_LINK_DIRECTORIES "${LINK_DIRECTORIES}" CACHE INTERNAL "" FORCE)

################################################################################
## V8
################################################################################

add_definitions("-DARANGODB_V8_VERSION=\"${V8_VERSION}\"")

foreach (LINK_DIR ${V8_LINK_DIRECTORIES})
  link_directories("${LINK_DIR}")
endforeach()

################################################################################
## ZLIB
################################################################################

add_definitions("-DARANGODB_ZLIB_VERSION=\"${ZLIB_VERSION}\"")
link_directories("${PROJECT_BINARY_DIR}/bin")

################################################################################
## PATHS, installation, packages, frontend
################################################################################

add_subdirectory(Documentation)
include(ArangoDBInstall)
if(USE_FRONTEND)
  include(frontend/aardvark)
endif()
if (NOT(SKIP_PACKAGING))
  include(packages/packages)
endif()

################################################################################
## ERRORS FILES
################################################################################

# If "make clean" removes these files, afterwards neither "make" nor "cmake" work any more.
set_directory_properties(PROPERTIES CLEAN_NO_CUSTOM "On")

set(ERROR_FILES
  lib/Basics/voc-errors.h
  lib/Basics/error-registry.h
  js/common/bootstrap/errors.js
)

set(ERROR_FILES_GEN)
set(ERRORS_DAT lib/Basics/errors.dat)

foreach (m IN LISTS ERROR_FILES)
  get_filename_component(GEN_BASENAME "${m}" NAME)
  if (GEN_BASENAME STREQUAL "errors.js")
    # generated errors.js is copied into the source dir
    set(TARGET_FILENAME ${CMAKE_SOURCE_DIR}/${m})
  else ()
    # generated C++ header files are copied into the build dir
    set(TARGET_FILENAME ${CMAKE_BINARY_DIR}/${m})
  endif ()
  add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/${m}
    COMMAND ${PYTHON_EXECUTABLE} ${CMAKE_SOURCE_DIR}/utils/generateErrorfile.py ${CMAKE_SOURCE_DIR}/${ERRORS_DAT} ${CMAKE_BINARY_DIR}/${GEN_BASENAME}.tmp
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_BINARY_DIR}/${GEN_BASENAME}.tmp ${TARGET_FILENAME}
    COMMAND ${CMAKE_COMMAND} -E remove ${CMAKE_BINARY_DIR}/${GEN_BASENAME}.tmp
    DEPENDS ${CMAKE_SOURCE_DIR}/${ERRORS_DAT} ${CMAKE_SOURCE_DIR}/utils/generateErrorfile.py
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT  "Building errors file ${m}"
    VERBATIM
  )

  list(APPEND ERROR_FILES_GEN ${CMAKE_BINARY_DIR}/${m})
endforeach ()

add_custom_target(errorfiles ALL DEPENDS ${ERROR_FILES_GEN})
  
set(EXIT_CODE_FILES
  lib/Basics/exitcodes.h
  js/common/bootstrap/exitcodes.js
  Installation/Windows/Plugins/exitcodes.nsh
)

set(EXIT_CODE_FILES_GEN)
set(EXIT_CODES_DAT lib/Basics/exitcodes.dat)

foreach (m IN LISTS EXIT_CODE_FILES)
  get_filename_component(GEN_BASENAME "${m}" NAME)
  add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/${m}
    COMMAND ${PYTHON_EXECUTABLE} ${CMAKE_SOURCE_DIR}/utils/generateExitCodesFiles.py ${CMAKE_SOURCE_DIR}/${EXIT_CODES_DAT} ${CMAKE_BINARY_DIR}/${GEN_BASENAME}.tmp
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_BINARY_DIR}/${GEN_BASENAME}.tmp ${CMAKE_BINARY_DIR}/${m}
    COMMAND ${CMAKE_COMMAND} -E remove ${CMAKE_BINARY_DIR}/${GEN_BASENAME}.tmp
    DEPENDS ${CMAKE_SOURCE_DIR}/${EXIT_CODES_DAT} ${CMAKE_SOURCE_DIR}/utils/generateExitCodesFiles.py
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT  "Building exitcode file ${m}"
    VERBATIM
  )

  list(APPEND EXIT_CODE_FILES_GEN ${CMAKE_BINARY_DIR}/${m})
endforeach ()

add_custom_target(exitcodefiles ALL DEPENDS ${EXIT_CODE_FILES_GEN})

################################################################################
## SUB-PROJECTS
################################################################################

list(INSERT SYSTEM_LIBRARIES 0
  ${BT_LIBS}
  ${ZLIB_LIBS}
  ${ICU_LIBS}
  ${OPENSSL_LIBRARIES}
  ${BASE_LIBS}
  ${SYS_LIBS}
  ${CMAKE_DL_LIBS}
  ${CMAKE_THREAD_LIBS_INIT}
)

add_subdirectory(lib)
add_subdirectory(client-tools)
add_subdirectory(arangod)


if (USE_GOOGLE_TESTS)
  add_subdirectory(tests)
endif ()

add_custom_target(packages
  DEPENDS ${PACKAGES_LIST}
)

add_custom_target(copy_packages
  DEPENDS ${COPY_PACKAGES_LIST}
)

add_custom_target(clean_packages
  DEPENDS ${CLEAN_PACKAGES_LIST}
)

add_custom_target(clean_autogenerated_files
  DEPENDS ${CLEAN_AUTOGENERATED_FILES}
)

message(STATUS "building for git revision: ${ARANGODB_BUILD_REPOSITORY}")

if (USE_ENTERPRISE)
  add_definitions("-DUSE_ENTERPRISE=1")
  add_subdirectory(enterprise)
endif ()

add_custom_target(arangodb
   DEPENDS arangod client-tools)

add_subdirectory(utils/gdb-pretty-printers/immer/test)
