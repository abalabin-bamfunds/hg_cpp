find_package(CURL 7.85 REQUIRED)
find_package(Boost 1.87 CONFIG QUIET COMPONENTS system url)
if(TARGET Boost::headers AND TARGET Boost::url)
    set(BG_WEB_BOOST_TARGETS Boost::headers Boost::url)
    if(TARGET Boost::system)
        list(APPEND BG_WEB_BOOST_TARGETS Boost::system)
    endif()
elseif(TARGET Boost::boost AND TARGET Boost::url)
    set(BG_WEB_BOOST_TARGETS Boost::boost Boost::url)
    if(TARGET Boost::system)
        list(APPEND BG_WEB_BOOST_TARGETS Boost::system)
    endif()
else()
    include(FetchContent)
    set(BG_WEB_BUILD_TESTING ${BUILD_TESTING})
    set(BUILD_TESTING OFF)
    set(BOOST_INCLUDE_LIBRARIES beast asio system url)
    set(BOOST_ENABLE_CMAKE ON)
    set(BOOST_SKIP_INSTALL_RULES ON)
    FetchContent_Declare(
        boost
        URL https://github.com/boostorg/boost/releases/download/boost-1.87.0/boost-1.87.0-cmake.tar.xz
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(boost)
    set(BUILD_TESTING ${BG_WEB_BUILD_TESTING})
    set(BUILD_TESTING ${BG_WEB_BUILD_TESTING} CACHE BOOL "Build the testing tree." FORCE)
    unset(BG_WEB_BUILD_TESTING)
    set(BG_WEB_BOOST_TARGETS Boost::beast Boost::asio Boost::system Boost::url)
    set_target_properties(boost_url PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

add_library(
    bg_web SHARED
    src/client_runtime.cpp
    src/client_auth.cpp
    src/client_transports.cpp
    src/http_client.cpp
    src/http_server.cpp
    src/server_runtime.cpp
    src/websocket_client.cpp
    src/websocket_client_runtime.cpp
)
add_library(bg::web ALIAS bg_web)
target_compile_features(bg_web PUBLIC cxx_std_23)
target_compile_definitions(bg_web PRIVATE BG_BUILDING_WEB)
target_include_directories(
    bg_web
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_link_libraries(bg_web PUBLIC bg::bg PRIVATE bg::library CURL::libcurl ICU::uc ${BG_WEB_BOOST_TARGETS} Threads::Threads)
set_target_properties(
    bg_web
    PROPERTIES
        CXX_EXTENSIONS OFF
        WINDOWS_EXPORT_ALL_SYMBOLS ON
        EXPORT_NAME web
        VERSION ${PROJECT_VERSION}
        SOVERSION ${PROJECT_VERSION_MAJOR}
)

install(
    TARGETS bg_web
    EXPORT bgTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)
install(DIRECTORY include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")

if(BG_BUILD_PYTHON)
    find_package(Python 3.13 COMPONENTS Interpreter Development.Module Development.SABIModule REQUIRED)
    if(APPLE)
        set(BG_WEB_PYTHON_BUILD_RPATH
            "@loader_path/../..;@loader_path/../../../../bindings/python;@loader_path/../../../..")
        set(BG_WEB_PYTHON_INSTALL_RPATH "@loader_path/../${CMAKE_INSTALL_LIBDIR}")
    elseif(WIN32)
        set(BG_WEB_PYTHON_BUILD_RPATH "")
        set(BG_WEB_PYTHON_INSTALL_RPATH "")
    else()
        set(BG_WEB_PYTHON_BUILD_RPATH "$ORIGIN/../..;$ORIGIN/../../../../bindings/python;$ORIGIN/../../../..")
        set(BG_WEB_PYTHON_INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}")
    endif()
    nanobind_add_module(_bg_web STABLE_ABI NB_SHARED src/python.cpp)
    target_compile_features(_bg_web PRIVATE cxx_std_23)
    target_link_libraries(_bg_web PRIVATE bg::python bg::web)
    set_target_properties(
        _bg_web
        PROPERTIES
            LIBRARY_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/extensions/web/python/bg_web"
            RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/extensions/web/python/bg_web"
            BUILD_RPATH "${BG_WEB_PYTHON_BUILD_RPATH}"
            INSTALL_RPATH "${BG_WEB_PYTHON_INSTALL_RPATH}"
    )
    file(MAKE_DIRECTORY "${PROJECT_BINARY_DIR}/extensions/web/python/bg_web")
    configure_file(python/bg_web/__init__.py "${PROJECT_BINARY_DIR}/extensions/web/python/bg_web/__init__.py" COPYONLY)
    configure_file(python/bg_web/auth.py "${PROJECT_BINARY_DIR}/extensions/web/python/bg_web/auth.py" COPYONLY)
    if(WIN32)
        add_custom_command(
            TARGET _bg_web
            POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:bg>" "$<TARGET_FILE_DIR:_bg_web>"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:bg_library>" "$<TARGET_FILE_DIR:_bg_web>"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:bg_python>" "$<TARGET_FILE_DIR:_bg_web>"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:bg_web>" "$<TARGET_FILE_DIR:_bg_web>"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:nanobind-abi3>" "$<TARGET_FILE_DIR:_bg_web>"
            VERBATIM
        )
    endif()
    install(TARGETS _bg_web LIBRARY DESTINATION bg_web RUNTIME DESTINATION bg_web)
    install(FILES python/bg_web/__init__.py python/bg_web/auth.py DESTINATION bg_web)
    if(WIN32)
        install(FILES
            "$<TARGET_FILE:bg>"
            "$<TARGET_FILE:bg_library>"
            "$<TARGET_FILE:bg_python>"
            "$<TARGET_FILE:bg_web>"
            "$<TARGET_FILE:nanobind-abi3>"
            DESTINATION bg_web
        )
    endif()
endif()

if(BUILD_TESTING)
    find_package(Catch2 3 REQUIRED)
    add_executable(bg_web_tests tests/http_client_test.cpp tests/http_server_test.cpp tests/websocket_client_test.cpp)
    target_compile_features(bg_web_tests PRIVATE cxx_std_23)
    target_link_libraries(bg_web_tests PRIVATE bg::web bg::library Catch2::Catch2WithMain ${BG_WEB_BOOST_TARGETS})
    add_test(NAME bg_web_tests COMMAND bg_web_tests)
    set_tests_properties(
        bg_web_tests PROPERTIES TIMEOUT 10 ENVIRONMENT "NO_PROXY=127.0.0.1,localhost;no_proxy=127.0.0.1,localhost"
    )

    if(BG_BUILD_PYTHON)
        add_test(
            NAME bg_web_python_tests
            COMMAND "${Python_EXECUTABLE}" -m pytest -q "${CMAKE_CURRENT_SOURCE_DIR}/tests_python"
        )
        set_tests_properties(
            bg_web_python_tests
            PROPERTIES
                ENVIRONMENT_MODIFICATION
                    "PYTHONPATH=path_list_prepend:${PROJECT_BINARY_DIR}/extensions/web/python;PYTHONPATH=path_list_prepend:${PROJECT_BINARY_DIR}/bindings/python"
                ENVIRONMENT "NO_PROXY=127.0.0.1,localhost;no_proxy=127.0.0.1,localhost"
                TIMEOUT 15
        )
    endif()
endif()
