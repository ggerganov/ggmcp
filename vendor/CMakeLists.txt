# Vendored third-party backends.
#
# nlohmann/json  - https://github.com/nlohmann/json           v3.12.0 (MIT)
# cpp-httplib    - https://github.com/yhirose/cpp-httplib     v0.29.0 (MIT)
#
# Both are header only and are wrapped in INTERFACE targets so that the rest of
# the build never sees a vendor specific include flag.

add_library(ggmcp_vendor_nlohmann_json INTERFACE)
target_include_directories(ggmcp_vendor_nlohmann_json SYSTEM INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/nlohmann_json/include)

add_library(ggmcp_vendor_httplib INTERFACE)
target_include_directories(ggmcp_vendor_httplib SYSTEM INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/cpp-httplib/include)

find_package(Threads REQUIRED)
target_link_libraries(ggmcp_vendor_httplib INTERFACE Threads::Threads)
