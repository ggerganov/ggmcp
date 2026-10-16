cmake_minimum_required(VERSION 3.16)

project(ggmcp VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "build type" FORCE)
endif()

option(GGMCP_BUILD_TESTS "Build the end-to-end suite" ON)

add_library(ggmcp_vendor INTERFACE)
target_include_directories(ggmcp_vendor INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/vendor)

find_package(Threads REQUIRED)

add_library(ggmcp_core STATIC
    src/auth.cpp
    src/config.cpp
    src/http.cpp
    src/paths.cpp
    src/protocol.cpp
    src/session.cpp
    src/shell.cpp
    src/stdio.cpp
    src/tools.cpp)
target_include_directories(ggmcp_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(ggmcp_core PUBLIC ggmcp_vendor PRIVATE Threads::Threads)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU|AppleClang")
    target_compile_options(ggmcp_core PRIVATE -Wall -Wextra)
endif()

add_executable(ggmcp src/main.cpp)
set_target_properties(ggmcp PROPERTIES OUTPUT_NAME ggmcp)
target_link_libraries(ggmcp PRIVATE ggmcp_core)

if(GGMCP_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
