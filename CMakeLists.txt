cmake_minimum_required(VERSION 3.10.0)
project(kroxy VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED true)

# Boost и liburing
find_package(Boost CONFIG REQUIRED)
find_package(PkgConfig REQUIRED)
find_package(OpenSSL REQUIRED)
pkg_check_modules(LIBURING REQUIRED liburing)
pkg_check_modules(JSONCPP REQUIRED jsoncpp)

include(FetchContent)
FetchContent_Declare(
        googletest
        URL https://github.com/google/googletest/releases/download/v1.17.0/googletest-1.17.0.tar.gz
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(kroxy src/main.cpp
        src/logger.cpp
        src/logger.hpp
        src/server.cpp
        src/server.hpp
        src/upstream.cpp
        src/upstream.hpp
        src/config.cpp
        src/httpsession.cpp
        src/streamsession.cpp)

target_include_directories(kroxy PRIVATE
    ${LIBURING_INCLUDE_DIRS}
    ${JSONCPP_INCLUDE_DIRS}   # заголовки json-cpp
)

target_compile_options(kroxy PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wsign-conversion
        -Wshadow
        -Wnon-virtual-dtor
        -Wold-style-cast
        -Wcast-align
        -Woverloaded-virtual
        -Wnull-dereference
        -Wdouble-promotion
        -Wformat=2
    >
)

target_link_libraries(kroxy PRIVATE
    Boost::boost
    OpenSSL::SSL
    OpenSSL::Crypto
    ${LIBURING_LIBRARIES}
    ${JSONCPP_LIBRARIES}
)

target_include_directories(kroxy PRIVATE ${JSONCPP_INCLUDE_DIRS})

# Testing
enable_testing()

add_executable(kroxy_test tests/test_config.cpp)

target_link_libraries(kroxy_test PRIVATE
        Boost::boost
        OpenSSL::SSL
        OpenSSL::Crypto
        ${LIBURING_LIBRARIES}
        ${JSONCPP_LIBRARIES}
        GTest::gtest_main
)
include(GoogleTest)
gtest_discover_tests(kroxy_test)