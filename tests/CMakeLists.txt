find_package(Catch2 3 REQUIRED)

add_executable(
    bg_tests
    evaluation/evaluation_context_test.cpp
    evaluation/future_scheduling_test.cpp
    evaluation/input_activity_test.cpp
    evaluation/invalidation_behavior_test.cpp
    evaluation/runtime_lifecycle_test.cpp
    evaluation/runtime_scheduling_test.cpp
    evaluation/runtime_storage_test.cpp
    evaluation/runtime_validity_test.cpp
    evaluation/runtime_views_test.cpp
    evaluation/simulation_evaluation_engine_test.cpp
    graph/compiled_graph_test.cpp
    node/aggregate_node_test.cpp
    node/input_test.cpp
    node/node_template_test.cpp
    node/output_test.cpp
    testing/test_graph_test.cpp
    type/type_system_test.cpp
    utils/polymorphic_value_test.cpp
    utils/reflection_test.cpp
    utils/symbol_test.cpp
    utils/version_test.cpp
    value/erased_scalar_value_test.cpp
    value/global_state_test.cpp
    value/opaque_scalar_value_test.cpp
    value/scalar_list_value_test.cpp
    value/scalar_storage_test.cpp
    value/scalar_value_test.cpp
    value/scalar_view_test.cpp
    value/time_series_view_test.cpp
    wiring/graph_template_test.cpp
    wiring/graph_wiring_test.cpp
)
target_link_libraries(bg_tests PRIVATE bg::bg Catch2::Catch2WithMain)

include(Catch)
catch_discover_tests(bg_tests)
