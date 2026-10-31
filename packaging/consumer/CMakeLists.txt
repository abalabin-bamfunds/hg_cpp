cmake_minimum_required(VERSION 3.31)

project(bg_installed_sdk_consumer LANGUAGES CXX)

find_package(bg CONFIG REQUIRED COMPONENTS python)

set(BG_TEST_PACKAGE_DIR "" CACHE PATH "Installed bg package directory for the test extension")

add_executable(bg_sdk_consumer main.cpp)
target_compile_features(bg_sdk_consumer PRIVATE cxx_std_23)
target_link_libraries(bg_sdk_consumer PRIVATE bg::library)

nanobind_add_module(bg_sdk_python_consumer STABLE_ABI NB_SHARED python_module.cpp)
target_link_libraries(bg_sdk_python_consumer PRIVATE bg::python)

if(BG_TEST_PACKAGE_DIR)
    nanobind_add_module(
        _bg_test_extension STABLE_ABI NB_SHARED
        "${PROJECT_SOURCE_DIR}/../../bindings/python/tests/extension_module.cpp"
    )
    target_link_libraries(_bg_test_extension PRIVATE bg::python)
    set_target_properties(
        _bg_test_extension
        PROPERTIES
            LIBRARY_OUTPUT_DIRECTORY "${BG_TEST_PACKAGE_DIR}"
            RUNTIME_OUTPUT_DIRECTORY "${BG_TEST_PACKAGE_DIR}"
    )
endif()
