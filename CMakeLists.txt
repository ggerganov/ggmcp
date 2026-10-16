cmake_minimum_required(VERSION 3.16)

project(ggmcp VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(GGMCP_VERSION "${PROJECT_VERSION}")

add_subdirectory(vendor)

add_library(ggmcp_lib STATIC
    src/json/json.cpp
    src/json/json_nlohmann.cpp
    src/http/http.cpp
    src/backends.cpp
    src/http/httplib_server.cpp
    src/mcp/tools.cpp
    src/mcp/session.cpp
    src/mcp/file_tools.cpp
    src/mcp/bash_tool.cpp
    src/mcp/auth.cpp
    src/mcp/config.cpp
    src/mcp/protocol.cpp
    src/mcp/http_transport.cpp
    src/mcp/stdio_transport.cpp
)

target_include_directories(ggmcp_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_definitions(ggmcp_lib PUBLIC GGMCP_VERSION="${GGMCP_VERSION}")
target_link_libraries(ggmcp_lib PUBLIC ggmcp_vendor_nlohmann_json ggmcp_vendor_httplib)

if (MSVC)
    target_compile_options(ggmcp_lib PRIVATE /W4)
else()
    target_compile_options(ggmcp_lib PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(ggmcp src/main.cpp)
set_target_properties(ggmcp PROPERTIES OUTPUT_NAME ggmcp)
target_compile_definitions(ggmcp PRIVATE GGMCP_VERSION="${GGMCP_VERSION}")
target_link_libraries(ggmcp PRIVATE ggmcp_lib)

enable_testing()
add_subdirectory(tests)
