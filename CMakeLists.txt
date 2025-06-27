cmake_minimum_required(VERSION 3.16)
project(cast_and_serve LANGUAGES CXX)

# Require C++14
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Install system dependencies (if not in CI)
if(UNIX AND NOT DEFINED ENV{CI})
  message(STATUS "Installing system packages via apt: libavahi-client-dev libcurl4-openssl-dev libboost-system-dev libboost-thread-dev pkg-config")
  execute_process(COMMAND sudo apt-get update)
  execute_process(COMMAND sudo apt-get install -y libavahi-client-dev libcurl4-openssl-dev libboost-system-dev libboost-thread-dev pkg-config)
endif()

# Find Avahi client only
enable_language(C)
find_package(PkgConfig REQUIRED)
pkg_check_modules(AVAHI_CLIENT REQUIRED avahi-client)

# Find Boost.System & Boost.Thread
find_package(Boost 1.66 REQUIRED COMPONENTS system thread)

# Find libcurl
find_package(CURL REQUIRED)

# Find OpenSSL for TLS WebSocket
find_package(OpenSSL REQUIRED)

# Find Protobuf for Cast V2 protocol
find_package(Protobuf REQUIRED)

# Fetch header-only dependencies
include(FetchContent)

# WebSocket++ (header-only)
FetchContent_Declare(
  websocketpp
  GIT_REPOSITORY https://github.com/zaphoyd/websocketpp.git
  GIT_TAG        0.8.2
)
FetchContent_MakeAvailable(websocketpp)

# JsonCpp (disable tests to avoid extra executables)
set(JSONCPP_WITH_TESTS OFF CACHE BOOL "Disable JsonCpp tests")
set(JSONCPP_WITH_POST_BUILD_UNITTEST OFF CACHE BOOL "Disable JsonCpp post build tests")
set(JSONCPP_WITH_PKGCONFIG_SUPPORT OFF CACHE BOOL "Disable JsonCpp pkgconfig")
set(JSONCPP_WITH_CMAKE_PACKAGE OFF CACHE BOOL "Disable JsonCpp CMake package config")
FetchContent_Declare(
  jsoncpp
  GIT_REPOSITORY https://github.com/open-source-parsers/jsoncpp.git
  GIT_TAG        1.9.5
)
FetchContent_MakeAvailable(jsoncpp)

# Generate protobuf files
set(PROTO_FILES ${CMAKE_CURRENT_SOURCE_DIR}/cast_channel.proto)
protobuf_generate_cpp(PROTO_SRCS PROTO_HDRS ${PROTO_FILES})

# Declare the executable with your C++ source
add_executable(cast_and_serve cast_and_serve.cpp ${PROTO_SRCS})

# Include directories for headers
target_include_directories(cast_and_serve PRIVATE
  ${AVAHI_CLIENT_INCLUDE_DIRS}
  ${Boost_INCLUDE_DIRS}
  ${CURL_INCLUDE_DIRS}
  ${Protobuf_INCLUDE_DIRS}
  ${CMAKE_CURRENT_BINARY_DIR}
  ${websocketpp_SOURCE_DIR}
  ${jsoncpp_SOURCE_DIR}/include
)

# Compiler flags from avahi-client
target_compile_options(cast_and_serve PRIVATE ${AVAHI_CLIENT_CFLAGS_OTHER})

# Link libraries (jsoncpp_lib is provided by FetchContent)
target_link_libraries(cast_and_serve PRIVATE
  ${AVAHI_CLIENT_LIBRARIES}
  ${Boost_LIBRARIES}
  ${CURL_LIBRARIES}
  OpenSSL::SSL
  OpenSSL::Crypto
  protobuf::libprotobuf
  jsoncpp_lib
  pthread
)

# Linker flags from avahi-client
target_link_options(cast_and_serve PRIVATE ${AVAHI_CLIENT_LDFLAGS_OTHER})
