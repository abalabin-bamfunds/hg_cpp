cmake_minimum_required(VERSION 3.31)

project(literal_input_probe LANGUAGES CXX)

set(HG_BUILD_PYTHON OFF CACHE BOOL "" FORCE)
set(HG_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)

add_subdirectory(../.. hg)

add_executable(literal_input_probe probe.cpp ../../tests/value/scalar/test_scalar_provider.cpp)
target_compile_features(literal_input_probe PRIVATE cxx_std_23)
target_link_libraries(literal_input_probe PRIVATE hg::hg)
target_include_directories(literal_input_probe PRIVATE include ../../tests/value/scalar)
set_target_properties(literal_input_probe PROPERTIES CXX_EXTENSIONS OFF)

add_executable(literal_time_series_apply_probe time_series_apply_probe.cpp ../../tests/value/scalar/test_scalar_provider.cpp)
target_compile_features(literal_time_series_apply_probe PRIVATE cxx_std_23)
target_link_libraries(literal_time_series_apply_probe PRIVATE hg::hg)
target_include_directories(literal_time_series_apply_probe PRIVATE include ../../tests/value/scalar)
set_target_properties(literal_time_series_apply_probe PROPERTIES CXX_EXTENSIONS OFF)
