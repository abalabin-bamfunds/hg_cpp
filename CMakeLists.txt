cmake_minimum_required(VERSION 3.31)

project(
    bg
    VERSION 0.1.0
    DESCRIPTION "An engine for forward propagation through computation graphs"
    LANGUAGES CXX
)

include(CTest)
include(FetchContent)
include(GNUInstallDirs)

set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

find_package(Threads REQUIRED)
find_package(ICU REQUIRED COMPONENTS uc i18n)
find_package(fmt CONFIG REQUIRED)

option(BG_BUILD_PYTHON "Build the Python extension module" OFF)
option(BG_BUILD_BENCHMARKS "Build performance benchmarks" OFF)

FetchContent_Declare(
    tpack
    GIT_REPOSITORY https://github.com/uentity/tpack.git
    GIT_TAG v0.3.4
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(tpack)

FetchContent_Declare(
    ddvisitor
    GIT_REPOSITORY https://github.com/uentity/ddvisitor.git
    GIT_TAG v0.9.6
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(ddvisitor)

add_library(
    bg SHARED
    src/compiled_graph.cpp
    src/context.cpp
    src/evaluation_profile.cpp
    src/evaluation_profiler.cpp
    src/global_state.cpp
    src/graph_instance_id.cpp
    src/graph_argument.cpp
    src/graph_wiring.cpp
    src/dispatch_cast.cpp
    src/dispatch.cpp
    src/dispatch_selector.cpp
    src/graph_wiring_structure.cpp
    src/nested_graph.cpp
    src/nested.cpp
    src/map_graph_argument.cpp
    src/map_node_definition.cpp
    src/map_entries.cpp
    src/map_node.cpp
    src/map.cpp
    src/reduce_graph_argument.cpp
    src/reduce_node.cpp
    src/reduce_node_definition.cpp
    src/reduce.cpp
    src/mesh_node.cpp
    src/mesh.cpp
    src/switch_node.cpp
    src/switch.cpp
    src/normalized_wiring_arguments.cpp
    src/operator.cpp
    src/operators.cpp
    src/queue.cpp
    src/node/aggregate_node_template_base.cpp
    src/node/node_template.cpp
    src/guarantee_parameter.cpp
    src/runtime_type.cpp
    src/runtime_graph.cpp
    src/graph/dynamic_node_subscriber_storage.cpp
    src/scratch_pad.cpp
    src/service.cpp
    src/service_catalogue.cpp
    src/scalar/allocation.cpp
    src/scalar/buffer_operations.cpp
    src/scalar/literal_operations.cpp
    src/scalar/literal_sentinel.cpp
    src/scalar/allocation_pool.cpp
    src/scalar/bundle/runtime_type.cpp
    src/scalar/dict/type.cpp
    src/scalar/string_storage.cpp
    src/scalar/bytes_storage.cpp
    src/scalar/cpp_runtime_type.cpp
    src/scalar/bundle/type.cpp
    src/scalar/list/type.cpp
    src/scalar/set/type.cpp
    src/scalar/struct/type.cpp
    src/scalar/tuple/runtime_type.cpp
    src/scalar/tuple/type.cpp
    src/scalar/runtime_type.cpp
    src/scalar/type.cpp
    src/signature.cpp
    src/signature_specificity.cpp
    src/realtime_evaluation_engine.cpp
    src/simulation_evaluation_engine.cpp
    src/symbol.cpp
    src/time_series/tsl/tsl_type.cpp
    src/time_series/tsl/tsl_runtime_type.cpp
    src/time_series/tsl/wiring.cpp
    src/time_series/tsl/operators.cpp
    src/time_series/fixed/fixed_reference_representation.cpp
    src/time_series/tsb/tsb_type.cpp
    src/time_series/tsb/tsb_runtime_type.cpp
    src/time_series/tsb/wiring.cpp
    src/time_series/tsb/operators.cpp
    src/time_series/ref/dynamic_reference_input.cpp
    src/time_series/dynamic_value_input.cpp
    src/time_series/ref/fixed_item_selection.cpp
    src/time_series/ref/ref_item_notifications.cpp
    src/time_series/ref/ref_notification.cpp
    src/time_series/ref/ref_input_sample.cpp
    src/time_series/ref/ref_provider.cpp
    src/time_series/ref/ref_runtime_type.cpp
    src/time_series/ref/ref_type.cpp
    src/time_series/ref/ref_node.cpp
    src/time_series/tsd/tsd_type.cpp
    src/time_series/tsd/tsd_key_subscription.cpp
    src/time_series/tsd/tsd_runtime_type.cpp
    src/time_series/tsd/tsd_dereferenced_representation.cpp
    src/time_series/tsd/tsd_item_reference_representation.cpp
    src/time_series/tsd/tsd_reference_representation.cpp
    src/time_series/tsd/tsd_reference_sample.cpp
    src/time_series/tsd/operators.cpp
    src/time_series/tss/tss_type.cpp
    src/time_series/tss/tss_runtime_type.cpp
    src/time_series/tss/tss_reference_sample.cpp
    src/time_series/collection_membership.cpp
    src/time_series/output_path.cpp
    src/testing/time_series_tick_comparison.cpp
    src/time_series/time_series_runtime_type.cpp
    src/time_series/literal_operations.cpp
    src/time_series/tick_serializer.cpp
    src/time_series_type.cpp
    src/time_series_variable.cpp
    src/time_series_value_type.cpp
    src/thread_local_pointer.cpp
    src/type.cpp
    src/type/schema.cpp
    src/type/schema_metadata.cpp
    src/type/type_registration.cpp
    src/type_of.cpp
    src/type_registry.cpp
    src/variadic_wiring_record.cpp
    src/variable.cpp
    src/variable_registry.cpp
    src/version.cpp
    src/wiring_context.cpp
    src/wiring_error.cpp
    src/wiring_materialisation.cpp
    src/wiring_record.cpp
    src/wired_output.cpp
    src/wiring_template.cpp
)
add_library(bg::bg ALIAS bg)

target_compile_features(bg PUBLIC cxx_std_23)
target_compile_definitions(bg PRIVATE BG_BUILDING_ENGINE BG_VERSION="${PROJECT_VERSION}")
target_link_libraries(bg PUBLIC ddv::ddvisitor PRIVATE Threads::Threads)
target_include_directories(
    bg
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

set_target_properties(
    bg
    PROPERTIES
        CXX_EXTENSIONS OFF
        WINDOWS_EXPORT_ALL_SYMBOLS ON
        VERSION ${PROJECT_VERSION}
        SOVERSION ${PROJECT_VERSION_MAJOR}
)

add_library(
    bg_library SHARED
    src/library/logical.cpp
    src/library/numeric.cpp
    src/library/operators.cpp
    src/library/collection.cpp
    src/library/tss.cpp
    src/library/string.cpp
    src/library/string_regex.cpp
    src/library/string_collection.cpp
    src/library/string_format.cpp
    src/library/stream.cpp
    src/library/timed.cpp
    src/library/running.cpp
    src/library/collection_reduction.cpp
)
add_library(bg::library ALIAS bg_library)

target_compile_features(bg_library PUBLIC cxx_std_23)
target_compile_definitions(bg_library PRIVATE BG_BUILDING_LIBRARY)
target_link_libraries(bg_library PUBLIC bg::bg PRIVATE ICU::uc ICU::i18n fmt::fmt)
target_include_directories(
    bg_library
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

set_target_properties(
    bg_library
    PROPERTIES
        CXX_EXTENSIONS OFF
        EXPORT_NAME library
        VERSION ${PROJECT_VERSION}
        SOVERSION ${PROJECT_VERSION_MAJOR}
)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

if(BG_BUILD_PYTHON)
    add_subdirectory(bindings/python)
endif()

if(BG_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()

include(CMakePackageConfigHelpers)

configure_package_config_file(
    cmake/bgConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/bgConfig.cmake"
    INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/bg"
)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/bgConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(
    TARGETS bg bg_library
    EXPORT bgTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
)

install(DIRECTORY include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")

install(
    EXPORT bgTargets
    FILE bgTargets.cmake
    NAMESPACE bg::
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/bg"
)

install(
    FILES
        "${CMAKE_CURRENT_BINARY_DIR}/bgConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/bgConfigVersion.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/bg"
)
