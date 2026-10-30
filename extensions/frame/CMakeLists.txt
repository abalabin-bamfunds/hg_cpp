cmake_minimum_required(VERSION 3.31)

project(bg_frame LANGUAGES CXX)

include(CTest)

option(BG_FRAME_BUILD_ARROW "Build the optional Arrow adapter" OFF)
option(BG_FRAME_BUILD_BENCHMARKS "Build optional frame benchmarks" OFF)

if(NOT TARGET bg::bg)
    find_package(bg CONFIG REQUIRED)
endif()

add_library(bg_frame SHARED src/arithmetic.cpp src/column.cpp src/column_expression.cpp src/comparison.cpp
                            src/expression.cpp src/frame.cpp src/literal.cpp src/logical.cpp src/reduction.cpp
                            src/reference_sample.cpp src/tick_capture.cpp src/with_columns.cpp src/collection.cpp
                            src/format.cpp)
add_library(bg::frame ALIAS bg_frame)

target_compile_features(bg_frame PUBLIC cxx_std_23)
target_link_libraries(bg_frame PUBLIC bg::bg bg::library)
target_compile_definitions(bg_frame PRIVATE BG_FRAME_BUILDING_LIBRARY)
set_target_properties(bg_frame PROPERTIES CXX_EXTENSIONS OFF WINDOWS_EXPORT_ALL_SYMBOLS ON)
target_include_directories(
    bg_frame
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

if(BG_FRAME_BUILD_ARROW)
    find_package(Arrow CONFIG REQUIRED)
    if(NOT TARGET Arrow::arrow_shared)
        message(FATAL_ERROR "The Arrow package must provide Arrow::arrow_shared")
    endif()

    add_library(bg_frame_arrow SHARED src/arrow.cpp)
    add_library(bg::frame_arrow ALIAS bg_frame_arrow)
    target_compile_features(bg_frame_arrow PUBLIC cxx_std_23)
    target_link_libraries(bg_frame_arrow PUBLIC bg::frame Arrow::arrow_shared)
    target_include_directories(bg_frame_arrow PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
    set_target_properties(bg_frame_arrow PROPERTIES CXX_EXTENSIONS OFF WINDOWS_EXPORT_ALL_SYMBOLS ON)

endif()

if(BG_FRAME_BUILD_BENCHMARKS)
    add_executable(bg_frame_expression_benchmark benchmarks/expression_benchmark.cpp)
    target_link_libraries(bg_frame_expression_benchmark PRIVATE bg::frame)

    add_executable(bg_frame_incremental_sum_benchmark benchmarks/incremental_sum_benchmark.cpp)
    target_link_libraries(bg_frame_incremental_sum_benchmark PRIVATE bg::frame)

    if(BG_FRAME_BUILD_ARROW)
        find_package(ArrowCompute CONFIG REQUIRED)
        add_executable(bg_frame_arrow_benchmark benchmarks/arrow_benchmark.cpp)
        target_link_libraries(bg_frame_arrow_benchmark PRIVATE bg::frame_arrow ArrowCompute::arrow_compute_shared)
        add_executable(bg_frame_compaction_benchmark benchmarks/compaction_benchmark.cpp)
        target_link_libraries(bg_frame_compaction_benchmark PRIVATE bg::frame_arrow
                                                                    ArrowCompute::arrow_compute_shared)
    endif()
endif()

if(BUILD_TESTING)
    add_executable(bg_frame_column_test tests/column_test.cpp)
    target_link_libraries(bg_frame_column_test PRIVATE bg::frame)
    add_test(NAME bg_frame_column_test COMMAND bg_frame_column_test)

    add_executable(bg_frame_provider_integration_test tests/provider_integration_test.cpp)
    target_link_libraries(bg_frame_provider_integration_test PRIVATE bg::frame)
    add_test(NAME bg_frame_provider_integration_test COMMAND bg_frame_provider_integration_test)

    add_executable(bg_frame_tsc_test tests/tsc_test.cpp)
    target_link_libraries(bg_frame_tsc_test PRIVATE bg::frame)
    add_test(NAME bg_frame_tsc_test COMMAND bg_frame_tsc_test)

    add_executable(bg_frame_frame_test tests/frame_test.cpp)
    target_link_libraries(bg_frame_frame_test PRIVATE bg::frame)
    add_test(NAME bg_frame_frame_test COMMAND bg_frame_frame_test)

    add_executable(bg_frame_expression_test tests/expression_test.cpp)
    target_link_libraries(bg_frame_expression_test PRIVATE bg::frame)
    add_test(NAME bg_frame_expression_test COMMAND bg_frame_expression_test)

    find_package(Catch2 3 CONFIG REQUIRED)
    add_executable(bg_frame_with_columns_binding_test tests/with_columns_binding_test.cpp)
    target_link_libraries(bg_frame_with_columns_binding_test PRIVATE bg::frame Catch2::Catch2WithMain)
    add_test(NAME bg_frame_with_columns_binding_test COMMAND bg_frame_with_columns_binding_test)

    add_executable(bg_frame_reduction_test tests/reduction_test.cpp)
    target_link_libraries(bg_frame_reduction_test PRIVATE bg::frame Catch2::Catch2WithMain)
    add_test(NAME bg_frame_reduction_test COMMAND bg_frame_reduction_test)

    add_executable(bg_frame_with_columns_test tests/with_columns_test.cpp)
    target_link_libraries(bg_frame_with_columns_test PRIVATE bg::frame Catch2::Catch2WithMain)
    add_test(NAME bg_frame_with_columns_test COMMAND bg_frame_with_columns_test)

    add_executable(bg_frame_collection_test tests/collection_test.cpp)
    target_link_libraries(bg_frame_collection_test PRIVATE bg::frame Catch2::Catch2WithMain)
    add_test(NAME bg_frame_collection_test COMMAND bg_frame_collection_test)

    add_executable(bg_frame_copy_test tests/copy_test.cpp)
    target_link_libraries(bg_frame_copy_test PRIVATE bg::frame Catch2::Catch2WithMain)
    add_test(NAME bg_frame_copy_test COMMAND bg_frame_copy_test)

    add_executable(bg_frame_reference_sample_test tests/reference_sample_test.cpp)
    target_link_libraries(bg_frame_reference_sample_test PRIVATE bg::frame Catch2::Catch2WithMain)
    add_test(NAME bg_frame_reference_sample_test COMMAND bg_frame_reference_sample_test)

    add_executable(bg_frame_format_test tests/format_test.cpp)
    target_link_libraries(bg_frame_format_test PRIVATE bg::frame Catch2::Catch2WithMain)
    add_test(NAME bg_frame_format_test COMMAND bg_frame_format_test)

    if(BG_FRAME_BUILD_ARROW)
        add_executable(bg_frame_arrow_test tests/arrow_test.cpp)
        target_link_libraries(bg_frame_arrow_test PRIVATE bg::frame_arrow)
        add_test(NAME bg_frame_arrow_test COMMAND bg_frame_arrow_test)
    endif()
endif()
