cmake_minimum_required(VERSION 3.10.0)
project(kroxy VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED true)

# Boost и liburing
find_package(Boost CONFIG REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBURING REQUIRED liburing)

# json-cpp
find_package(jsoncpp REQUIRED)

add_executable(kroxy main.cpp)

target_include_directories(kroxy PRIVATE
    ${LIBURING_INCLUDE_DIRS}
    ${JSONCPP_INCLUDE_DIRS}   # заголовки json-cpp
)

target_link_libraries(kroxy PRIVATE
    Boost::boost
    ${LIBURING_LIBRARIES}
    jsoncpp_lib
)
