cmake_minimum_required(VERSION 3.31)

project(hg_installed_sdk_consumer LANGUAGES CXX)

find_package(hg CONFIG REQUIRED COMPONENTS python)

set(HG_TEST_PACKAGE_DIR "" CACHE PATH "Installed hg package directory for the test extension")

add_executable(hg_sdk_consumer main.cpp)
target_compile_features(hg_sdk_consumer PRIVATE cxx_std_23)
target_link_libraries(hg_sdk_consumer PRIVATE hg::library)

nanobind_add_module(hg_sdk_python_consumer STABLE_ABI NB_SHARED python_module.cpp)
target_link_libraries(hg_sdk_python_consumer PRIVATE hg::python)

if(HG_TEST_PACKAGE_DIR)
    nanobind_add_module(
        _hg_test_extension STABLE_ABI NB_SHARED
        "${PROJECT_SOURCE_DIR}/../../bindings/python/tests/extension_module.cpp"
    )
    target_link_libraries(_hg_test_extension PRIVATE hg::python)
    set_target_properties(
        _hg_test_extension
        PROPERTIES
            LIBRARY_OUTPUT_DIRECTORY "${HG_TEST_PACKAGE_DIR}"
            RUNTIME_OUTPUT_DIRECTORY "${HG_TEST_PACKAGE_DIR}"
    )
endif()
