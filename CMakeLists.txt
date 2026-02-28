cmake_minimum_required(VERSION 3.22...3.31)
# 3.22 = good C++23 support + many FetchContent improvements
# Upper bound prevents very surprising behavior from future versions

project(kroxy
        VERSION 0.1.0
        DESCRIPTION "High-performance proxy server using io_uring"
        LANGUAGES CXX C
)

# ── Global settings ──────────────────────────────────────────────────────────────

set(CMAKE_CXX_STANDARD          23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS        OFF)   # no -std=gnu++23

# Strongly recommended in 2024+
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Prefer new behavior (important for FetchContent + dependencies)
set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)   # let *_ROOT variables override find_package

# ── Options ──────────────────────────────────────────────────────────────────────

option(KROXY_BUILD_TESTS "Build unit tests" ${PROJECT_IS_TOP_LEVEL})
option(KROXY_SANITIZERS   "Enable address + undefined behavior sanitizers (debug only)" OFF)

# ── Dependencies ─────────────────────────────────────────────────────────────────

find_package(Boost CONFIG)   # most projects only need headers
find_package(OpenSSL REQUIRED)
find_package(PkgConfig REQUIRED)
find_package(spdlog REQUIRED)

pkg_check_modules(LIBURING REQUIRED IMPORTED_TARGET liburing>=2.5)
pkg_check_modules(JSONCPP  REQUIRED IMPORTED_TARGET jsoncpp>=1.9)

# ── Third-party content ──────────────────────────────────────────────────────────

include(FetchContent)

FetchContent_Declare(
        googletest
        URL     https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
)

# Don't install googletest artifacts
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(googletest)

FetchContent_Declare(
        argparse
        GIT_REPOSITORY https://github.com/p-ranav/argparse.git
        GIT_TAG        v3.2
        # GIT_SHALLOW    TRUE
)

set(ARGPARSE_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(argparse)

# ── Main target ──────────────────────────────────────────────────────────────────

add_executable(kroxy)

target_sources(kroxy
        PRIVATE
        src/main.cpp
        src/logger.cpp
        src/server.cpp
        src/upstream.cpp
        src/config.cpp
        src/httpsession.cpp
        src/streamsession.cpp
        src/worker.cpp

        PUBLIC
        src/logger.hpp
        src/server.hpp
        src/upstream.hpp
        src/worker.hpp
)

target_include_directories(kroxy
        PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(kroxy
        PRIVATE
        Boost::headers
        OpenSSL::SSL
        OpenSSL::Crypto
        PkgConfig::LIBURING
        PkgConfig::JSONCPP
        argparse::argparse
        spdlog::spdlog
)

# ── Compiler warnings ────────────────────────────────────────────────────────────

target_compile_options(kroxy PRIVATE
        # Very good warning set for modern C++
        -Wall -Wextra -Wpedantic
        -Wconversion -Wsign-conversion
        -Wshadow -Wnon-virtual-dtor
        -Wold-style-cast -Wcast-align
        -Woverloaded-virtual -Wnull-dereference
        -Wdouble-promotion -Wformat=2
        -Wmisleading-indentation -Wduplicated-cond -Wduplicated-branches
        -Wlogical-op -Wuseless-cast

        # Optional – very strict (good for CI, can be noisy locally)
        # -Werror
)

# ── Sanitizers (debug only) ──────────────────────────────────────────────────────

if(KROXY_SANITIZERS AND CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_options(kroxy PRIVATE -fsanitize=address,undefined)
    target_link_options   (kroxy PRIVATE -fsanitize=address,undefined)
endif()

# ── Testing ──────────────────────────────────────────────────────────────────────

if(KROXY_BUILD_TESTS)
    enable_testing()

    add_executable(kroxy_test
            tests/test_config.cpp

            src/logger.cpp
            src/server.cpp
            src/upstream.cpp
            src/config.cpp
            src/httpsession.cpp
            src/streamsession.cpp
            src/worker.cpp
            # add more test files here as they appear
    )

    target_link_libraries(kroxy_test
            PRIVATE
            GTest::gtest_main
            Boost::headers
            OpenSSL::SSL
            OpenSSL::Crypto
            PkgConfig::LIBURING
            PkgConfig::JSONCPP
            argparse::argparse
            spdlog::spdlog

#            $<TARGET_OBJECTS:kroxy>
            # If tests need parts of the real implementation:
            # $<TARGET_OBJECTS:kroxy>   # ← object library trick (advanced)
    )

    target_include_directories(kroxy_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

    # Reuse same strict warnings
    target_compile_options(kroxy_test PRIVATE
            $<$<COMPILE_LANGUAGE:CXX>:
            -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
            >
    )

    include(GoogleTest)
    gtest_discover_tests(kroxy_test)
endif()

# ── Install (mainly for packaging / deployment) ─────────────────────────────────

include(GNUInstallDirs)

install(TARGETS kroxy
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(FILES conf/kroxy.conf.json DESTINATION ${CMAKE_INSTALL_DATADIR}/kroxy/conf)