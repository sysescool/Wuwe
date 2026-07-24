include("${CMAKE_SOURCE_DIR}/cmake/wuwe-runtime.cmake")

add_executable(memory_tests
    memory_tests.cpp
)

target_link_libraries(memory_tests PRIVATE wuwe)
wuwe_copy_runtime_dlls(memory_tests)

add_test(NAME memory_tests COMMAND memory_tests)

add_executable(knowledge_tests
    knowledge_tests.cpp
)

target_link_libraries(knowledge_tests PRIVATE wuwe)
wuwe_copy_runtime_dlls(knowledge_tests)

add_test(NAME knowledge_tests COMMAND knowledge_tests)
set_tests_properties(knowledge_tests PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)

add_executable(agent_runtime_tests
    agent_runtime_tests.cpp
)

target_link_libraries(agent_runtime_tests PRIVATE wuwe)
wuwe_copy_runtime_dlls(agent_runtime_tests)

add_test(NAME agent_runtime_tests COMMAND agent_runtime_tests)

add_executable(llm_provider_tests
    llm_provider_tests.cpp
)

target_link_libraries(llm_provider_tests PRIVATE wuwe)
wuwe_copy_runtime_dlls(llm_provider_tests)

add_test(NAME llm_provider_tests COMMAND llm_provider_tests)

add_executable(net_tests
    net_tests.cpp
)

target_link_libraries(net_tests PRIVATE wuwe)
if(WUWE_BUILT_WITH_HTTPLIB_HTTPS)
    if(NOT TARGET OpenSSL::SSL)
        find_package(OpenSSL REQUIRED)
    endif()
    target_compile_definitions(net_tests PRIVATE
        CPPHTTPLIB_OPENSSL_SUPPORT=1
        WUWE_NET_TESTS_HAS_OPENSSL=1
        WUWE_NET_TESTS_HAS_HTTPLIB_HTTPS=1
    )
    target_link_libraries(net_tests PRIVATE OpenSSL::SSL OpenSSL::Crypto)
endif()
wuwe_copy_runtime_dlls(net_tests)

add_test(NAME net_tests COMMAND net_tests)

add_executable(planning_tests
    planning_tests.cpp
)

target_link_libraries(planning_tests PRIVATE wuwe)
wuwe_copy_runtime_dlls(planning_tests)

add_test(NAME planning_tests COMMAND planning_tests)

add_executable(reflection_tests
    reflection_tests.cpp
)

target_link_libraries(reflection_tests PRIVATE wuwe)
wuwe_copy_runtime_dlls(reflection_tests)

add_test(NAME reflection_tests COMMAND reflection_tests)

add_executable(guardrails_tests
    guardrails_tests.cpp
)

target_link_libraries(guardrails_tests PRIVATE wuwe)
wuwe_copy_runtime_dlls(guardrails_tests)

add_test(NAME guardrails_tests COMMAND guardrails_tests)

add_executable(evaluation_tests
    evaluation_tests.cpp
)

target_link_libraries(evaluation_tests PRIVATE wuwe)
wuwe_copy_runtime_dlls(evaluation_tests)

add_test(NAME evaluation_tests COMMAND evaluation_tests)

add_executable(learning_tests
    learning_tests.cpp
)

target_link_libraries(learning_tests PRIVATE wuwe)
wuwe_copy_runtime_dlls(learning_tests)

add_test(NAME learning_tests COMMAND learning_tests)

add_executable(exploration_tests
    exploration_tests.cpp
)

target_link_libraries(exploration_tests PRIVATE wuwe)
wuwe_copy_runtime_dlls(exploration_tests)

add_test(NAME exploration_tests COMMAND exploration_tests)

add_executable(routing_tests
    routing_tests.cpp
)

target_link_libraries(routing_tests PRIVATE wuwe)
wuwe_copy_runtime_dlls(routing_tests)

add_test(NAME routing_tests COMMAND routing_tests)

add_executable(orchestration_tests
    orchestration_tests.cpp
)

target_link_libraries(orchestration_tests PRIVATE wuwe)
wuwe_copy_runtime_dlls(orchestration_tests)

add_test(NAME orchestration_tests COMMAND orchestration_tests)

add_executable(best_of_n_tests
    best_of_n_tests.cpp
)

target_link_libraries(best_of_n_tests PRIVATE wuwe)
wuwe_copy_runtime_dlls(best_of_n_tests)

add_test(NAME best_of_n_tests COMMAND best_of_n_tests)

add_executable(multi_agent_tests
    multi_agent_tests.cpp
)

target_link_libraries(multi_agent_tests PRIVATE wuwe)
wuwe_copy_runtime_dlls(multi_agent_tests)

add_test(NAME multi_agent_tests COMMAND multi_agent_tests)

add_executable(a2a_tests
    a2a_tests.cpp
)

target_link_libraries(a2a_tests PRIVATE wuwe)
wuwe_copy_runtime_dlls(a2a_tests)

add_test(NAME a2a_tests COMMAND a2a_tests)

add_executable(header_independence_tests
    header_independence/main.cpp
    header_independence/evaluation_header.cpp
    header_independence/multi_agent_header.cpp
    header_independence/a2a_header.cpp
)

target_link_libraries(header_independence_tests PRIVATE wuwe)
wuwe_copy_runtime_dlls(header_independence_tests)

add_test(NAME header_independence_tests COMMAND header_independence_tests)

add_executable(reasoning_tests
    reasoning_tests.cpp
)

target_link_libraries(reasoning_tests PRIVATE wuwe)
wuwe_copy_runtime_dlls(reasoning_tests)

add_test(NAME reasoning_tests COMMAND reasoning_tests)

add_executable(execution_tests
    execution_tests.cpp
)

target_link_libraries(execution_tests PRIVATE wuwe)
if(WIN32)
    target_link_libraries(execution_tests PRIVATE Advapi32 Shell32 Userenv Ws2_32)
endif()
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    target_compile_definitions(execution_tests PRIVATE
        WUWE_EXECUTION_TEST_PYTHON="${Python3_EXECUTABLE}"
    )
endif()
wuwe_copy_runtime_dlls(execution_tests)

add_test(NAME execution_tests COMMAND execution_tests)

add_executable(mcp_tests
    mcp_tests.cpp
)

target_link_libraries(mcp_tests PRIVATE wuwe)
wuwe_copy_runtime_dlls(mcp_tests)
add_dependencies(mcp_tests mcp_stdio_example)
target_compile_definitions(mcp_tests PRIVATE
    WUWE_MCP_STDIO_EXAMPLE_PATH="$<TARGET_FILE:mcp_stdio_example>"
)

add_test(NAME mcp_tests COMMAND mcp_tests)

set_tests_properties(
    knowledge_tests
    planning_tests
    orchestration_tests
    best_of_n_tests
    multi_agent_tests
    a2a_tests
    reasoning_tests
    mcp_tests
    PROPERTIES LABELS "concurrency"
)
