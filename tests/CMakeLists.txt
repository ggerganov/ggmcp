add_executable(ggmcp-e2e
    e2e/http_client.cpp
    e2e/harness.cpp
    e2e/main.cpp
)
target_include_directories(ggmcp-e2e PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/e2e)
target_link_libraries(ggmcp-e2e PRIVATE ggmcp_lib)
add_dependencies(ggmcp-e2e ggmcp)

add_test(NAME e2e COMMAND ggmcp-e2e $<TARGET_FILE:ggmcp>)
set_tests_properties(e2e PROPERTIES TIMEOUT 300)
