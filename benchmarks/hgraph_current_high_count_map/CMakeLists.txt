cmake_minimum_required(VERSION 3.26)
project(hgraph_current_high_count_map LANGUAGES CXX)

find_package(Python 3.12 COMPONENTS Interpreter Development.Embed REQUIRED)
find_package(hgraph CONFIG REQUIRED)

add_executable(hgraph_current_high_count_map native.cpp)
target_compile_features(hgraph_current_high_count_map PRIVATE cxx_std_23)
target_include_directories(hgraph_current_high_count_map PRIVATE "${CMAKE_CURRENT_LIST_DIR}/..")
target_link_libraries(hgraph_current_high_count_map PRIVATE hgraph::core)
