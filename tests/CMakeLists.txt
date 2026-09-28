find_package(Catch2 3 REQUIRED)

add_executable(
    bg_tests
    evaluation/context/evaluation_context_test.cpp
    evaluation/context/evaluation_scratch_test.cpp
    evaluation/context/context_time_series_test.cpp
    evaluation/engine/simulation_evaluation_engine_test.cpp
    evaluation/lifecycle/runtime_lifecycle_test.cpp
    evaluation/map_runtime_node_test.cpp
    evaluation/map_test.cpp
    evaluation/mesh_test.cpp
    evaluation/switch_test.cpp
    evaluation/nested_graph_test.cpp
    evaluation/nested_graph_owner_test.cpp
    evaluation/scheduling/timed_scheduling_test.cpp
    evaluation/scheduling/runtime_scheduling_test.cpp
    evaluation/storage/runtime_storage_test.cpp
    evaluation/storage/runtime_views_test.cpp
    evaluation/storage/node_test.cpp
    evaluation/time_series/time_series_list_test.cpp
    evaluation/time_series/time_series_list_getitem_test.cpp
    evaluation/time_series/time_series_bundle_test.cpp
    evaluation/time_series/constant_test.cpp
    evaluation/time_series/time_series_dict_test.cpp
    evaluation/time_series/time_series_dict_getitem_test.cpp
    evaluation/time_series/time_series_reference_test.cpp
    evaluation/time_series/time_series_set_test.cpp
    evaluation/time_series/time_series_value_test.cpp
    evaluation/validity/input_activity_test.cpp
    evaluation/validity/invalidation_behavior_test.cpp
    evaluation/validity/runtime_validity_test.cpp
    graph/compiled_graph_test.cpp
    node/aggregate_node_test.cpp
    node/input_test.cpp
    node/node_template_test.cpp
    node/runtime_signature_node_test.cpp
    node/output_test.cpp
    node/scalar/dict_test.cpp
    node/scalar/list_test.cpp
    node/scalar/set_test.cpp
    node/scalar_state_test.cpp
    testing/test_graph_test.cpp
    type/type_system_test.cpp
    type/type_variable_test.cpp
    type/time_series_dict_type_test.cpp
    type/time_series_bundle_type_test.cpp
    type/time_series_list_type_test.cpp
    type/time_series_set_type_test.cpp
    type/time_series_reference_type_test.cpp
    type/operator_specificity_test.cpp
    utils/adaptive_set_test.cpp
    utils/allocator_test.cpp
    utils/contiguous_bunch_test.cpp
    utils/hash_dictionary_test.cpp
    utils/hash_set_test.cpp
    utils/index_test.cpp
    utils/linear_hash_table_test.cpp
    utils/occupancy_index_test.cpp
    utils/reflection_test.cpp
    utils/runtime_bunch_test.cpp
    utils/schema_reflection_test.cpp
    utils/scope_exit_test.cpp
    utils/sparse_bunch_test.cpp
    utils/symbol_test.cpp
    utils/tagged_ptr_test.cpp
    utils/version_test.cpp
    value/global_state_test.cpp
    value/scalar/builtins_test.cpp
    value/scalar/capability_test.cpp
    value/scalar/dict/mutable_test.cpp
    value/scalar/dict/storage_test.cpp
    value/scalar/dict/value_test.cpp
    value/scalar/dispatch_test.cpp
    value/scalar/erased_value_test.cpp
    value/scalar/list/mutable_test.cpp
    value/scalar/list/storage_test.cpp
    value/scalar/list/value_test.cpp
    value/scalar/opaque_test.cpp
    value/scalar/set/mutable_test.cpp
    value/scalar/set/storage_test.cpp
    value/scalar/set/value_test.cpp
    value/scalar/storage_test.cpp
    value/scalar/struct_test.cpp
    value/scalar/test_scalar_provider.cpp
    value/scalar/value_test.cpp
    value/scalar/visitor_test.cpp
    value/scalar/view_test.cpp
    value/time_series_view_test.cpp
    value/time_series/dict/storage_test.cpp
    value/time_series/list/storage_test.cpp
    value/time_series/reference/storage_test.cpp
    value/time_series/set/storage_test.cpp
    wiring/graph_template_test.cpp
    wiring/context_time_series_test.cpp
    wiring/graph_argument_test.cpp
    wiring/graph_wiring_test.cpp
    wiring/graph_wiring_structure_test.cpp
    wiring/map_call_test.cpp
    wiring/map_test.cpp
    wiring/mesh_test.cpp
    wiring/switch_test.cpp
    wiring/operator_test.cpp
    wiring/requirement_test.cpp
    wiring/resolver_test.cpp
    wiring/time_series_bundle_wiring_test.cpp
    wiring/variadic_collection_wiring_test.cpp
    wiring/variadic_wiring_test.cpp
    wiring/time_series_dict_wiring_test.cpp
    wiring/time_series_list_wiring_test.cpp
    wiring/time_series_reference_wiring_test.cpp
    wiring/type_mapping_test.cpp
)
target_link_libraries(bg_tests PRIVATE bg::bg Catch2::Catch2WithMain)
target_compile_options(bg_tests PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/bigobj>)
target_compile_definitions(bg_tests PRIVATE $<$<CXX_COMPILER_ID:MSVC>:CATCH_CONFIG_NO_CPP17_STRING_VIEW>)

if(WIN32)
    add_custom_command(
        TARGET bg_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_RUNTIME_DLLS:bg_tests>
                $<TARGET_FILE_DIR:bg_tests>
        COMMAND_EXPAND_LISTS
    )
endif()

include(Catch)

# Keep Catch2 discovery files in the build tree because XDG_RUNTIME_DIR may be unavailable to sandboxed builds.
set(catch2_environment "${CMAKE_CURRENT_BINARY_DIR}/catch2_environment.cmake")
file(GENERATE OUTPUT "${catch2_environment}"
     CONTENT "set(ENV{XDG_RUNTIME_DIR} [==[${CMAKE_CURRENT_BINARY_DIR}]==])\n")
set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES "${catch2_environment}")

catch_discover_tests(bg_tests DISCOVERY_MODE PRE_TEST)

find_package(Python3 3.10 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_custom_target(
        test-coverage-report
        COMMAND
            "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/test_coverage.py" "$<TARGET_FILE:bg_tests>"
            --dimensions "${PROJECT_SOURCE_DIR}/docs/testing.md"
            --output "${PROJECT_BINARY_DIR}/test-coverage.md"
            --check
        DEPENDS bg_tests
        VERBATIM
    )
endif()
