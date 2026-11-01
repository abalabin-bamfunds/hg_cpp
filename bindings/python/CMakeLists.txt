find_package(Python 3.12 COMPONENTS Interpreter Development.Module Development.SABIModule REQUIRED)

execute_process(
    COMMAND "${Python_EXECUTABLE}" -m nanobind --cmake_dir
    OUTPUT_VARIABLE nanobind_cmake_dir
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE nanobind_cmake_dir_result
)
if(NOT nanobind_cmake_dir_result EQUAL 0)
    message(FATAL_ERROR "nanobind is required. Install it with: ${Python_EXECUTABLE} -m pip install nanobind")
endif()

list(PREPEND CMAKE_PREFIX_PATH "${nanobind_cmake_dir}")
find_package(nanobind CONFIG REQUIRED)

nanobind_add_module(_hg STABLE_ABI NB_SHARED src/module.cpp)

add_library(
    hg_python SHARED
    src/object.cpp
    src/scalar_conversion.cpp
    src/node_definition.cpp
    src/graph_definition.cpp
    src/delta.cpp
    src/input_view.cpp
    src/python_node_injection.cpp
    src/python_queue.cpp
    src/python_literal_builder.cpp
    src/dict_view.cpp
    src/higher_order.cpp
    src/operator.cpp
    src/python_object_operators.cpp
    src/wiring_argument.cpp
    src/python_wiring_error.cpp
    src/graph_run_input.cpp
    src/graph_run_output.cpp
    src/python.cpp
    src/gil.cpp
)
add_library(hg::python ALIAS hg_python)
set_target_properties(
    hg_python
    PROPERTIES
        EXPORT_NAME python
        POSITION_INDEPENDENT_CODE ON
        WINDOWS_EXPORT_ALL_SYMBOLS ON
        VERSION ${PROJECT_VERSION}
        SOVERSION ${PROJECT_VERSION_MAJOR}
)
target_compile_features(hg_python PUBLIC cxx_std_23)
target_compile_definitions(hg_python PRIVATE HG_PYTHON_BUILDING PUBLIC NB_SHARED Py_LIMITED_API=0x030C0000)
if(APPLE)
    target_compile_options(hg_python INTERFACE -fvisibility=default)
endif()
target_include_directories(
    hg_python
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
        $<BUILD_INTERFACE:${NB_DIR}/include>
)
target_link_libraries(
    hg_python
    PUBLIC hg::library Python::SABIModule
    PRIVATE "$<BUILD_INTERFACE:$<TARGET_LINKER_FILE:nanobind-abi3>>"
)
add_dependencies(hg_python nanobind-abi3)

target_link_libraries(_hg PRIVATE hg::python)
target_compile_features(_hg PRIVATE cxx_std_23)

set(python_package_dir "${PROJECT_BINARY_DIR}/bindings/python/hg")
file(MAKE_DIRECTORY "${python_package_dir}")
configure_file(python/hg/__init__.py "${python_package_dir}/__init__.py" COPYONLY)
configure_file(python/hg/operators.py "${python_package_dir}/operators.py" COPYONLY)
if(APPLE)
    set(python_build_rpath "@loader_path/..;@loader_path/../../..")
    set(python_install_rpath "@loader_path/../${CMAKE_INSTALL_LIBDIR}")
    set(python_library_install_rpath "@loader_path")
elseif(WIN32)
    set(python_build_rpath "")
    set(python_install_rpath "")
    set(python_library_install_rpath "")
else()
    set(python_build_rpath "$ORIGIN/..;$ORIGIN/../../..")
    set(python_install_rpath "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}")
    set(python_library_install_rpath "$ORIGIN")
endif()

set_target_properties(
    hg_python
    PROPERTIES
        INSTALL_RPATH "${python_library_install_rpath}"
)

set_target_properties(
    _hg
    PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY "${python_package_dir}"
        RUNTIME_OUTPUT_DIRECTORY "${python_package_dir}"
        BUILD_RPATH "${python_build_rpath}"
        INSTALL_RPATH "${python_install_rpath}"
)

if(WIN32)
    add_custom_command(
        TARGET _hg
        POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:hg>" "$<TARGET_FILE_DIR:_hg>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:hg_library>" "$<TARGET_FILE_DIR:_hg>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:hg_python>" "$<TARGET_FILE_DIR:_hg>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:nanobind-abi3>" "$<TARGET_FILE_DIR:_hg>"
        VERBATIM
    )
    install(FILES "$<TARGET_FILE:hg>" DESTINATION hg)
    install(FILES "$<TARGET_FILE:hg_library>" DESTINATION hg)
    install(FILES "$<TARGET_FILE:hg_python>" DESTINATION hg)
    install(FILES "$<TARGET_FILE:nanobind-abi3>" DESTINATION hg)
endif()

install(
    TARGETS hg_python
    EXPORT hgTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)
install(
    TARGETS nanobind-abi3
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)
install(TARGETS _hg LIBRARY DESTINATION hg RUNTIME DESTINATION hg)
install(FILES python/hg/__init__.py python/hg/operators.py DESTINATION hg)

if(BUILD_TESTING)
    nanobind_add_module(_hg_test_extension STABLE_ABI NB_SHARED tests/extension_module.cpp)
    target_link_libraries(_hg_test_extension PRIVATE hg::python)
    set_target_properties(
        _hg_test_extension
        PROPERTIES
            LIBRARY_OUTPUT_DIRECTORY "${python_package_dir}"
            RUNTIME_OUTPUT_DIRECTORY "${python_package_dir}"
            BUILD_RPATH "${python_build_rpath}"
    )

    execute_process(
        COMMAND "${Python_EXECUTABLE}" -c "import pytest"
        RESULT_VARIABLE python_pytest_result
    )
    if(NOT python_pytest_result EQUAL 0)
        message(
            FATAL_ERROR
            "pytest is required for Python tests. Install it with: "
            "${Python_EXECUTABLE} -m pip install 'hg_cpp[test]'"
        )
    endif()

    add_test(
        NAME python_tests
        COMMAND "${Python_EXECUTABLE}" -m pytest -q "${CMAKE_CURRENT_SOURCE_DIR}/tests"
    )
    set(
        python_test_environment
        "PYTHONPATH=${PROJECT_BINARY_DIR}/bindings/python"
        "HG_PYTHON_PACKAGE_DIR=${PROJECT_BINARY_DIR}/bindings/python/hg"
    )
    set_tests_properties(
        python_tests
        PROPERTIES
            ENVIRONMENT "${python_test_environment}"
    )
endif()
