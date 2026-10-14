cmake_minimum_required(VERSION 3.31)

project(literal_input_probe LANGUAGES CXX)

set(BG_BUILD_PYTHON OFF CACHE BOOL "" FORCE)
set(BG_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)

add_subdirectory(../.. bg)

add_executable(literal_input_probe probe.cpp)
target_compile_features(literal_input_probe PRIVATE cxx_std_23)
target_link_libraries(literal_input_probe PRIVATE bg::bg)
target_include_directories(literal_input_probe PRIVATE include)
set_target_properties(literal_input_probe PROPERTIES CXX_EXTENSIONS OFF)
