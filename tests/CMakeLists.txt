find_package(Catch2 3 REQUIRED)

add_executable(
    bg_tests
    aggregate_node_test.cpp
    compiled_graph_test.cpp
    global_state_test.cpp
    graph_wiring_test.cpp
    graph_template_test.cpp
    input_test.cpp
    node_template_test.cpp
    output_test.cpp
    polymorphic_value_test.cpp
    reflection_test.cpp
    runtime_graph_test.cpp
    scalar_storage_test.cpp
    scalar_value_test.cpp
    scalar_view_test.cpp
    simulation_evaluation_engine_test.cpp
    symbol_test.cpp
    test_graph_test.cpp
    time_series_view_test.cpp
    type_system_test.cpp
    version_test.cpp
)
target_link_libraries(bg_tests PRIVATE bg::bg Catch2::Catch2WithMain)

include(Catch)
catch_discover_tests(bg_tests)
