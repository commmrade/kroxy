cmake_minimum_required(VERSION 3.10.0)
project(kroxy VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED true)

# Boost и liburing
find_package(Boost CONFIG REQUIRED)
find_package(PkgConfig REQUIRED)
find_package(OpenSSL REQUIRED)
pkg_check_modules(LIBURING REQUIRED liburing)

# json-cpp
find_package(jsoncpp REQUIRED)

add_executable(kroxy main.cpp
        httpsession.hpp
        streamsession.hpp
        session.hpp
        stream.hpp
        utils.hpp)

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
    jsoncpp_lib
)
