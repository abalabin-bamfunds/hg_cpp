cmake_minimum_required(VERSION 3.31)

project(hg_frame LANGUAGES CXX)

include(CTest)

option(HG_FRAME_BUILD_ARROW "Build the optional Arrow adapter" OFF)
option(HG_FRAME_BUILD_BENCHMARKS "Build optional frame benchmarks" OFF)

if(NOT TARGET hg::hg)
    find_package(hg CONFIG REQUIRED)
endif()

add_library(hg_frame SHARED src/aggregation_sum.cpp src/arithmetic.cpp src/column.cpp src/column_expression.cpp src/comparison.cpp
                            src/expression.cpp src/frame.cpp src/group_by.cpp src/literal.cpp src/logical.cpp src/reduction.cpp
                            src/reference_sample.cpp src/tick_capture.cpp src/queue_operations.cpp
                            src/with_columns.cpp src/collection.cpp
                            src/format.cpp)
add_library(hg::frame ALIAS hg_frame)

target_compile_features(hg_frame PUBLIC cxx_std_23)
target_link_libraries(hg_frame PUBLIC hg::hg hg::library)
target_compile_definitions(hg_frame PRIVATE HG_FRAME_BUILDING_LIBRARY)
set_target_properties(hg_frame PROPERTIES CXX_EXTENSIONS OFF WINDOWS_EXPORT_ALL_SYMBOLS ON)
target_include_directories(
    hg_frame
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

if(HG_FRAME_BUILD_ARROW)
    find_package(Arrow CONFIG REQUIRED)
    if(NOT TARGET Arrow::arrow_shared)
        message(FATAL_ERROR "The Arrow package must provide Arrow::arrow_shared")
    endif()

    add_library(hg_frame_arrow SHARED src/arrow.cpp)
    add_library(hg::frame_arrow ALIAS hg_frame_arrow)
    target_compile_features(hg_frame_arrow PUBLIC cxx_std_23)
    target_link_libraries(hg_frame_arrow PUBLIC hg::frame Arrow::arrow_shared)
    target_include_directories(hg_frame_arrow PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
    set_target_properties(hg_frame_arrow PROPERTIES CXX_EXTENSIONS OFF WINDOWS_EXPORT_ALL_SYMBOLS ON)

endif()

if(HG_FRAME_BUILD_BENCHMARKS)
    add_executable(hg_frame_expression_benchmark benchmarks/expression_benchmark.cpp)
    target_link_libraries(hg_frame_expression_benchmark PRIVATE hg::frame)

    add_executable(hg_frame_incremental_sum_benchmark benchmarks/incremental_sum_benchmark.cpp)
    target_link_libraries(hg_frame_incremental_sum_benchmark PRIVATE hg::frame)

    add_executable(hg_frame_group_by_benchmark benchmarks/group_by_benchmark.cpp)
    target_link_libraries(hg_frame_group_by_benchmark PRIVATE hg::frame)

    if(HG_FRAME_BUILD_ARROW)
        find_package(ArrowCompute CONFIG REQUIRED)
        find_package(ArrowAcero CONFIG REQUIRED)
        add_executable(hg_frame_group_by_arrow_benchmark benchmarks/group_by_benchmark.cpp)
        target_compile_definitions(hg_frame_group_by_arrow_benchmark PRIVATE HG_FRAME_GROUP_BY_ARROW_BENCHMARK)
        target_link_libraries(hg_frame_group_by_arrow_benchmark PRIVATE hg::frame_arrow
                                                                       ArrowCompute::arrow_compute_shared
                                                                       ArrowAcero::arrow_acero_shared)

        add_executable(hg_frame_arrow_benchmark benchmarks/arrow_benchmark.cpp)
        target_link_libraries(hg_frame_arrow_benchmark PRIVATE hg::frame_arrow ArrowCompute::arrow_compute_shared)
        add_executable(hg_frame_compaction_benchmark benchmarks/compaction_benchmark.cpp)
        target_link_libraries(hg_frame_compaction_benchmark PRIVATE hg::frame_arrow
                                                                    ArrowCompute::arrow_compute_shared)
    endif()
endif()

if(BUILD_TESTING)
    add_executable(hg_frame_column_test tests/column_test.cpp)
    target_link_libraries(hg_frame_column_test PRIVATE hg::frame)
    add_test(NAME hg_frame_column_test COMMAND hg_frame_column_test)

    add_executable(hg_frame_provider_integration_test tests/provider_integration_test.cpp)
    target_link_libraries(hg_frame_provider_integration_test PRIVATE hg::frame)
    add_test(NAME hg_frame_provider_integration_test COMMAND hg_frame_provider_integration_test)

    add_executable(hg_frame_tsc_test tests/tsc_test.cpp)
    target_link_libraries(hg_frame_tsc_test PRIVATE hg::frame)
    add_test(NAME hg_frame_tsc_test COMMAND hg_frame_tsc_test)

    add_executable(hg_frame_frame_test tests/frame_test.cpp)
    target_link_libraries(hg_frame_frame_test PRIVATE hg::frame)
    add_test(NAME hg_frame_frame_test COMMAND hg_frame_frame_test)

    add_executable(hg_frame_expression_test tests/expression_test.cpp)
    target_link_libraries(hg_frame_expression_test PRIVATE hg::frame)
    add_test(NAME hg_frame_expression_test COMMAND hg_frame_expression_test)

    find_package(Catch2 3 CONFIG REQUIRED)
    add_executable(hg_frame_with_columns_binding_test tests/with_columns_binding_test.cpp)
    target_link_libraries(hg_frame_with_columns_binding_test PRIVATE hg::frame Catch2::Catch2WithMain)
    add_test(NAME hg_frame_with_columns_binding_test COMMAND hg_frame_with_columns_binding_test)

    add_executable(hg_frame_reduction_test tests/reduction_test.cpp)
    target_link_libraries(hg_frame_reduction_test PRIVATE hg::frame Catch2::Catch2WithMain)
    add_test(NAME hg_frame_reduction_test COMMAND hg_frame_reduction_test)

    add_executable(hg_frame_with_columns_test tests/with_columns_test.cpp)
    target_link_libraries(hg_frame_with_columns_test PRIVATE hg::frame Catch2::Catch2WithMain)
    add_test(NAME hg_frame_with_columns_test COMMAND hg_frame_with_columns_test)

    add_executable(hg_frame_group_by_test tests/group_by_test.cpp)
    target_link_libraries(hg_frame_group_by_test PRIVATE hg::frame Catch2::Catch2WithMain)
    add_test(NAME hg_frame_group_by_test COMMAND hg_frame_group_by_test)

    add_executable(hg_frame_collection_test tests/collection_test.cpp)
    target_link_libraries(hg_frame_collection_test PRIVATE hg::frame Catch2::Catch2WithMain)
    add_test(NAME hg_frame_collection_test COMMAND hg_frame_collection_test)

    add_executable(hg_frame_copy_test tests/copy_test.cpp)
    target_link_libraries(hg_frame_copy_test PRIVATE hg::frame Catch2::Catch2WithMain)
    add_test(NAME hg_frame_copy_test COMMAND hg_frame_copy_test)

    add_executable(hg_frame_reference_sample_test tests/reference_sample_test.cpp)
    target_link_libraries(hg_frame_reference_sample_test PRIVATE hg::frame Catch2::Catch2WithMain)
    add_test(NAME hg_frame_reference_sample_test COMMAND hg_frame_reference_sample_test)

    add_executable(hg_frame_queue_test tests/queue_test.cpp)
    target_link_libraries(hg_frame_queue_test PRIVATE hg::frame Catch2::Catch2WithMain)
    add_test(NAME hg_frame_queue_test COMMAND hg_frame_queue_test)

    add_executable(hg_frame_format_test tests/format_test.cpp)
    target_link_libraries(hg_frame_format_test PRIVATE hg::frame Catch2::Catch2WithMain)
    add_test(NAME hg_frame_format_test COMMAND hg_frame_format_test)

    if(HG_FRAME_BUILD_ARROW)
        add_executable(hg_frame_arrow_test tests/arrow_test.cpp)
        target_link_libraries(hg_frame_arrow_test PRIVATE hg::frame_arrow)
        add_test(NAME hg_frame_arrow_test COMMAND hg_frame_arrow_test)
    endif()
endif()
