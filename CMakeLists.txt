cmake_minimum_required(VERSION 3.24)

project(
    NovaCache
    VERSION 0.1.0
    DESCRIPTION "A distributed in-memory data store"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(NOVACACHE_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(NOVACACHE_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)
option(NOVACACHE_ENABLE_TSAN "Enable ThreadSanitizer" OFF)
option(NOVACACHE_WARNINGS_AS_ERRORS "Treat project warnings as errors" OFF)

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")
include(CompilerWarnings)
include(Sanitizers)

function(novacache_configure_target target)
    target_compile_features(${target} PUBLIC cxx_std_20)
    novacache_set_project_warnings(${target})
    novacache_enable_sanitizers(${target})
endfunction()

add_library(
    novacache_core
    src/commands/registry.cpp
    src/net/reactor.cpp
    src/net/socket.cpp
    src/protocol/encoder.cpp
    src/protocol/parser.cpp
    src/server/server.cpp
    src/server/stats.cpp
    src/store/store.cpp
    src/util/thread_pool.cpp
    src/version.cpp
)
add_library(NovaCache::core ALIAS novacache_core)

target_include_directories(
    novacache_core
    PUBLIC
        "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
        "$<INSTALL_INTERFACE:include>"
)
target_compile_definitions(
    novacache_core
    PRIVATE NOVACACHE_VERSION_STRING="${PROJECT_VERSION}"
)
novacache_configure_target(novacache_core)

foreach(application IN ITEMS server cli benchmark)
    set(target "novacache-${application}")
    add_executable(${target} "apps/${target}.cpp")
    target_link_libraries(${target} PRIVATE NovaCache::core)
    novacache_configure_target(${target})
endforeach()

include(CTest)
if(BUILD_TESTING)
    find_package(Threads REQUIRED)
    include(FetchContent)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.16.0
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(googletest)

    add_executable(
        novacache-tests
        tests/concurrency/store_concurrency_test.cpp
        tests/integration/server_test.cpp
        tests/unit/expiry_test.cpp
        tests/unit/protocol_test.cpp
        tests/unit/store_test.cpp
        tests/unit/version_test.cpp
    )
    target_link_libraries(
        novacache-tests
        PRIVATE
            NovaCache::core
            GTest::gtest_main
            Threads::Threads
    )
    novacache_configure_target(novacache-tests)

    include(GoogleTest)
    gtest_discover_tests(novacache-tests)
endif()
