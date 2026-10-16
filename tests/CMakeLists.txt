add_executable(ggmcp-e2e
    e2e/cases_browser.cpp
    e2e/cases_cli.cpp
    e2e/cases_http.cpp
    e2e/cases_protocol.cpp
    e2e/cases_tools.cpp
    e2e/harness.cpp
    e2e/main.cpp
    e2e/net.cpp
    e2e/proc.cpp)

target_include_directories(ggmcp-e2e PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/e2e)
target_compile_definitions(ggmcp-e2e PRIVATE GGMCP_BINARY="$<TARGET_FILE:ggmcp>")
target_link_libraries(ggmcp-e2e PRIVATE ggmcp_core)
add_dependencies(ggmcp-e2e ggmcp)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU|AppleClang")
    target_compile_options(ggmcp-e2e PRIVATE -Wall -Wextra)
endif()

# One instance per case, so a failure names the case that failed.
set(GGMCP_E2E_CASES
    a_failing_tool_is_not_a_protocol_error
    a_hung_up_client_does_not_kill_the_server
    a_refused_origin_carries_nothing
    a_restart_over_a_used_socket_works
    a_signal_stops_the_process
    a_token_in_the_query_string_is_refused
    always_on_tools
    an_outside_root_is_taken_as_given
    an_unusable_root_is_fatal
    an_unusable_token_source_is_fatal
    any_origin_is_allowed_by_default
    bash_tool
    bearer_token
    before_the_handshake
    confinement
    cors_preflight
    custom_endpoint
    failing_writes_leave_nothing_behind
    file_tools
    flag_validation
    health
    help_and_version
    index_page
    initialize
    initialize_on_a_live_session
    keep_alive_and_chunked_bodies
    malformed_bodies
    methods
    no_cors
    notifications_answer_202
    one_instance_per_port
    policy_stateless
    policy_strict
    protected_resource_metadata
    protocol_version_header
    server_info
    session_teardown
    start_up_banner
    stdio_ignores_the_session_policy
    stdio_quiet
    stdio_transport
    strict_origin
    the_default_token_source_is_the_environment
    three_error_shapes
    token_file_rotation
    tool_alias
    tools_list
    tools_list_and_call_agree
    tools_off
    transport_status_codes
)

foreach(case_ IN LISTS GGMCP_E2E_CASES)
    add_test(NAME e2e_${case_} COMMAND ggmcp-e2e ${case_})
    set_tests_properties(e2e_${case_} PROPERTIES TIMEOUT 180)
endforeach()

add_test(NAME e2e_all COMMAND ggmcp-e2e)
set_tests_properties(e2e_all PROPERTIES TIMEOUT 900)
