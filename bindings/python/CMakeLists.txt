find_package(Python 3.13 COMPONENTS Interpreter Development.Module REQUIRED)

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

nanobind_add_module(_bg NB_SHARED src/module.cpp)

add_library(
    bg_python SHARED
    src/object.cpp
    src/scalar_conversion.cpp
    src/node_definition.cpp
    src/graph_definition.cpp
    src/delta.cpp
    src/input_view.cpp
    src/python_node_injection.cpp
    src/dict_view.cpp
    src/higher_order.cpp
    src/operator.cpp
    src/wiring_argument.cpp
    src/test_graph_input.cpp
    src/python.cpp
)
add_library(bg::python ALIAS bg_python)
set_target_properties(
    bg_python
    PROPERTIES
        EXPORT_NAME python
        POSITION_INDEPENDENT_CODE ON
        WINDOWS_EXPORT_ALL_SYMBOLS ON
        VERSION ${PROJECT_VERSION}
        SOVERSION ${PROJECT_VERSION_MAJOR}
)
target_compile_features(bg_python PUBLIC cxx_std_23)
target_compile_definitions(bg_python PRIVATE BG_PYTHON_BUILDING PUBLIC NB_SHARED)
target_include_directories(
    bg_python
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
        $<BUILD_INTERFACE:${NB_DIR}/include>
)
target_link_libraries(
    bg_python
    PUBLIC bg::library Python::Module
    PRIVATE "$<BUILD_INTERFACE:$<TARGET_LINKER_FILE:nanobind>>"
)
add_dependencies(bg_python nanobind)

target_link_libraries(_bg PRIVATE bg::python)
target_compile_features(_bg PRIVATE cxx_std_23)

set(python_package_dir "${PROJECT_BINARY_DIR}/bindings/python/bg")
file(MAKE_DIRECTORY "${python_package_dir}")
configure_file(python/bg/__init__.py "${python_package_dir}/__init__.py" COPYONLY)
configure_file(python/bg/operators.py "${python_package_dir}/operators.py" COPYONLY)
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

set_target_properties(bg_python PROPERTIES INSTALL_RPATH "${python_library_install_rpath}")

set_target_properties(
    _bg
    PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY "${python_package_dir}"
        RUNTIME_OUTPUT_DIRECTORY "${python_package_dir}"
        BUILD_RPATH "${python_build_rpath}"
        INSTALL_RPATH "${python_install_rpath}"
)

if(WIN32)
    add_custom_command(
        TARGET _bg
        POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:bg>" "$<TARGET_FILE_DIR:_bg>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:bg_library>" "$<TARGET_FILE_DIR:_bg>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:bg_python>" "$<TARGET_FILE_DIR:_bg>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:nanobind>" "$<TARGET_FILE_DIR:_bg>"
        VERBATIM
    )
    install(FILES "$<TARGET_FILE:bg>" DESTINATION bg)
    install(FILES "$<TARGET_FILE:bg_library>" DESTINATION bg)
    install(FILES "$<TARGET_FILE:bg_python>" DESTINATION bg)
    install(FILES "$<TARGET_FILE:nanobind>" DESTINATION bg)
endif()

install(
    TARGETS bg_python
    EXPORT bgTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)
install(
    TARGETS nanobind
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)
install(TARGETS _bg LIBRARY DESTINATION bg RUNTIME DESTINATION bg)
install(FILES python/bg/__init__.py python/bg/operators.py DESTINATION bg)

if(BUILD_TESTING)
    nanobind_add_module(_bg_test_extension NB_SHARED tests/extension_module.cpp)
    target_link_libraries(_bg_test_extension PRIVATE bg::python)
    set_target_properties(
        _bg_test_extension
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
            "${Python_EXECUTABLE} -m pip install 'bg-graph[test]'"
        )
    endif()

    add_test(
        NAME python_tests
        COMMAND "${Python_EXECUTABLE}" -m pytest -q "${CMAKE_CURRENT_SOURCE_DIR}/tests"
    )
    set(
        python_test_environment
        "PYTHONPATH=${PROJECT_BINARY_DIR}/bindings/python"
        "BG_PYTHON_PACKAGE_DIR=${PROJECT_BINARY_DIR}/bindings/python/bg"
    )
    set_tests_properties(
        python_tests
        PROPERTIES
            ENVIRONMENT "${python_test_environment}"
    )
endif()
