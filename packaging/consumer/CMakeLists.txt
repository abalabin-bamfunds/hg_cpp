cmake_minimum_required(VERSION 3.31)

project(bg_installed_sdk_consumer LANGUAGES CXX)

find_package(bg CONFIG REQUIRED COMPONENTS python)

add_executable(bg_sdk_consumer main.cpp)
target_compile_features(bg_sdk_consumer PRIVATE cxx_std_23)
target_link_libraries(bg_sdk_consumer PRIVATE bg::library)

nanobind_add_module(bg_sdk_python_consumer NB_SHARED python_module.cpp)
target_link_libraries(bg_sdk_python_consumer PRIVATE bg::python)
