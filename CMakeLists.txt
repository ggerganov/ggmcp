cmake_minimum_required(VERSION 3.16)
project(ggmcp VERSION 0.1.0 LANGUAGES CXX
        DESCRIPTION "A tiny Model Context Protocol server in dependency-free C++17")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

option(GGMCP_BUILD_TESTS "Build the ggmcp test binary" ON)

find_package(Threads REQUIRED)

add_library(ggmcp_core STATIC
  src/json.cpp
  src/log.cpp
  src/protocol.cpp
  src/http_server.cpp
  src/mcp_http.cpp
  src/tools.cpp
  src/filesystem_tools.cpp
)
target_include_directories(ggmcp_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(ggmcp_core PUBLIC Threads::Threads)
target_compile_options(ggmcp_core PRIVATE -Wall -Wextra -Wpedantic -Wshadow)

add_executable(ggmcp src/main.cpp)
target_link_libraries(ggmcp PRIVATE ggmcp_core)
target_compile_options(ggmcp PRIVATE -Wall -Wextra -Wpedantic -Wshadow)

if(GGMCP_BUILD_TESTS)
  enable_testing()

  add_executable(ggmcp_tests tests/test_main.cpp)
  target_link_libraries(ggmcp_tests PRIVATE ggmcp_core)
  target_compile_options(ggmcp_tests PRIVATE -Wall -Wextra)
  add_test(NAME unit COMMAND ggmcp_tests)

  # End-to-end: spawns the real server and drives the MCP handshake over HTTP.
  find_program(PYTHON3 NAMES python3)
  if(PYTHON3)
    add_test(NAME http_roundtrip
             COMMAND ${PYTHON3} ${CMAKE_CURRENT_SOURCE_DIR}/tests/http_roundtrip.py
                     $<TARGET_FILE:ggmcp>)
    set_property(TEST http_roundtrip PROPERTY TIMEOUT 60)
  else()
    message(STATUS "python3 not found: skipping the http_roundtrip integration test")
  endif()
endif()

install(TARGETS ggmcp RUNTIME DESTINATION bin)
