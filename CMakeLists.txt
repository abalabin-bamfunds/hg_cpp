cmake_minimum_required(VERSION 3.31)

project(
    bg
    VERSION 0.1.0
    DESCRIPTION "An engine for forward propagation through computation graphs"
    LANGUAGES CXX
)

include(CTest)
include(FetchContent)
include(GNUInstallDirs)

find_package(Threads REQUIRED)

option(BG_BUILD_PYTHON "Build the Python extension module" OFF)
option(BG_BUILD_BENCHMARKS "Build performance benchmarks" OFF)

FetchContent_Declare(
    tpack
    GIT_REPOSITORY https://github.com/uentity/tpack.git
    GIT_TAG v0.3.4
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(tpack)

FetchContent_Declare(
    ddvisitor
    GIT_REPOSITORY https://github.com/uentity/ddvisitor.git
    GIT_TAG v0.9.6
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(ddvisitor)

add_library(
    bg SHARED
    src/compiled_graph.cpp
    src/graph_wiring.cpp
    src/nested_graph.cpp
    src/nested.cpp
    src/guarantee_parameter.cpp
    src/runtime_graph.cpp
    src/scalar/allocation.cpp
    src/scalar/dict/type.cpp
    src/scalar/list/type.cpp
    src/scalar/set/type.cpp
    src/scalar/storage_representation.cpp
    src/scalar/type.cpp
    src/signature.cpp
    src/simulation_evaluation_engine.cpp
    src/symbol.cpp
    src/time_series/tsl/tsl_type.cpp
    src/time_series/tsb/tsb_type.cpp
    src/time_series/ref/ref_notification.cpp
    src/time_series/ref/ref_type.cpp
    src/time_series/ref/ref_node.cpp
    src/time_series/tsd/tsd_type.cpp
    src/time_series/tss/tss_type.cpp
    src/time_series/time_series_storage_representation.cpp
    src/time_series_type.cpp
    src/time_series_variable.cpp
    src/time_series_value_type.cpp
    src/thread_local_pointer.cpp
    src/type.cpp
    src/type_of.cpp
    src/version.cpp
    src/wiring_context.cpp
)
add_library(bg::bg ALIAS bg)

target_compile_features(bg PUBLIC cxx_std_23)
target_compile_definitions(bg PRIVATE BG_VERSION="${PROJECT_VERSION}")
target_link_libraries(bg PUBLIC ddv::ddvisitor PRIVATE Threads::Threads)
target_include_directories(
    bg
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

set_target_properties(
    bg
    PROPERTIES
        CXX_EXTENSIONS OFF
        WINDOWS_EXPORT_ALL_SYMBOLS ON
        VERSION ${PROJECT_VERSION}
        SOVERSION ${PROJECT_VERSION_MAJOR}
)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

if(BG_BUILD_PYTHON)
    add_subdirectory(bindings/python)
endif()

if(BG_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()

include(CMakePackageConfigHelpers)

configure_package_config_file(
    cmake/bgConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/bgConfig.cmake"
    INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/bg"
)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/bgConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(
    TARGETS bg
    EXPORT bgTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
)

install(DIRECTORY include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")

install(
    EXPORT bgTargets
    FILE bgTargets.cmake
    NAMESPACE bg::
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/bg"
)

install(
    FILES
        "${CMAKE_CURRENT_BINARY_DIR}/bgConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/bgConfigVersion.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/bg"
)
