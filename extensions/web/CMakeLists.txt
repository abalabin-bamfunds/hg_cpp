find_package(CURL 7.85 REQUIRED)
find_package(OpenSSL REQUIRED)

FetchContent_Declare(
    certify
    URL https://github.com/djarek/certify/archive/b74d50d6d91d460f92defb33f32baf302ce82986.tar.gz
    URL_HASH SHA256=1cfca816c51ffee84b1a66a3698f2a6ef5536914cb5d36829d81c3dd7ffa70de
    SOURCE_SUBDIR include/boost/certify
)
# Consume only the headers. Upstream's CMake project adds unrelated Boost
# components and installs targets that are private to this implementation.
FetchContent_MakeAvailable(certify)

find_package(Boost 1.87 CONFIG QUIET COMPONENTS system url)
if(TARGET Boost::headers AND TARGET Boost::url)
    set(HG_WEB_BOOST_TARGETS Boost::headers Boost::url)
    if(TARGET Boost::system)
        list(APPEND HG_WEB_BOOST_TARGETS Boost::system)
    endif()
elseif(TARGET Boost::boost AND TARGET Boost::url)
    set(HG_WEB_BOOST_TARGETS Boost::boost Boost::url)
    if(TARGET Boost::system)
        list(APPEND HG_WEB_BOOST_TARGETS Boost::system)
    endif()
else()
    include(FetchContent)
    set(HG_WEB_BUILD_TESTING ${BUILD_TESTING})
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
    set(BUILD_TESTING ${HG_WEB_BUILD_TESTING})
    set(BUILD_TESTING ${HG_WEB_BUILD_TESTING} CACHE BOOL "Build the testing tree." FORCE)
    unset(HG_WEB_BUILD_TESTING)
    set(HG_WEB_BOOST_TARGETS Boost::beast Boost::asio Boost::system Boost::url)
    set_target_properties(boost_url PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

add_library(
    hg_web SHARED
    src/client_runtime.cpp
    src/client_auth.cpp
    src/client_transports.cpp
    src/http_client.cpp
    src/http_server.cpp
    src/server_runtime.cpp
    src/websocket_client.cpp
    src/websocket_client_runtime.cpp
)
add_library(hg::web ALIAS hg_web)
target_compile_features(hg_web PUBLIC cxx_std_23)
target_compile_definitions(hg_web PRIVATE HG_BUILDING_WEB)
target_include_directories(
    hg_web
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_link_libraries(
    hg_web PUBLIC hg::hg PRIVATE hg::library CURL::libcurl ICU::uc OpenSSL::SSL OpenSSL::Crypto
                    ${HG_WEB_BOOST_TARGETS} Threads::Threads
)
target_include_directories(hg_web SYSTEM PRIVATE ${certify_SOURCE_DIR}/include)
if(WIN32)
    target_link_libraries(hg_web PRIVATE Crypt32)
elseif(APPLE)
    target_link_libraries(hg_web PRIVATE "-framework CoreFoundation" "-framework Security")
endif()
set_target_properties(
    hg_web
    PROPERTIES
        CXX_EXTENSIONS OFF
        WINDOWS_EXPORT_ALL_SYMBOLS ON
        EXPORT_NAME web
        INSTALL_RPATH "${native_library_install_rpath}"
        VERSION ${PROJECT_VERSION}
        SOVERSION ${PROJECT_VERSION_MAJOR}
)

install(
    TARGETS hg_web
    EXPORT hgTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)
install(DIRECTORY include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")

if(HG_BUILD_PYTHON)
    find_package(Python 3.12 COMPONENTS Interpreter Development.Module Development.SABIModule REQUIRED)
    if(APPLE)
        set(HG_WEB_PYTHON_BUILD_RPATH
            "@loader_path/../..;@loader_path/../../../../bindings/python;@loader_path/../../../..")
        set(HG_WEB_PYTHON_INSTALL_RPATH "@loader_path/../${CMAKE_INSTALL_LIBDIR}")
    elseif(WIN32)
        set(HG_WEB_PYTHON_BUILD_RPATH "")
        set(HG_WEB_PYTHON_INSTALL_RPATH "")
    else()
        set(HG_WEB_PYTHON_BUILD_RPATH "$ORIGIN/../..;$ORIGIN/../../../../bindings/python;$ORIGIN/../../../..")
        set(HG_WEB_PYTHON_INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}")
    endif()
    nanobind_add_module(_hg_web STABLE_ABI NB_SHARED src/python.cpp)
    target_compile_features(_hg_web PRIVATE cxx_std_23)
    target_link_libraries(_hg_web PRIVATE hg::python hg::web)
    set_target_properties(
        _hg_web
        PROPERTIES
            LIBRARY_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/extensions/web/python/hg_web"
            RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/extensions/web/python/hg_web"
            BUILD_RPATH "${HG_WEB_PYTHON_BUILD_RPATH}"
            INSTALL_RPATH "${HG_WEB_PYTHON_INSTALL_RPATH}"
    )
    file(MAKE_DIRECTORY "${PROJECT_BINARY_DIR}/extensions/web/python/hg_web")
    configure_file(python/hg_web/__init__.py "${PROJECT_BINARY_DIR}/extensions/web/python/hg_web/__init__.py" COPYONLY)
    configure_file(python/hg_web/auth.py "${PROJECT_BINARY_DIR}/extensions/web/python/hg_web/auth.py" COPYONLY)
    if(WIN32)
        add_custom_command(
            TARGET _hg_web
            POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:hg>" "$<TARGET_FILE_DIR:_hg_web>"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:hg_library>" "$<TARGET_FILE_DIR:_hg_web>"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:hg_python>" "$<TARGET_FILE_DIR:_hg_web>"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:hg_web>" "$<TARGET_FILE_DIR:_hg_web>"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:nanobind-abi3>" "$<TARGET_FILE_DIR:_hg_web>"
            VERBATIM
        )
    endif()
    install(TARGETS _hg_web LIBRARY DESTINATION hg_web RUNTIME DESTINATION hg_web)
    install(FILES python/hg_web/__init__.py python/hg_web/auth.py DESTINATION hg_web)
    if(WIN32)
        install(FILES
            "$<TARGET_FILE:hg>"
            "$<TARGET_FILE:hg_library>"
            "$<TARGET_FILE:hg_python>"
            "$<TARGET_FILE:hg_web>"
            "$<TARGET_FILE:nanobind-abi3>"
            DESTINATION hg_web
        )
    endif()
endif()

if(BUILD_TESTING)
    find_package(Catch2 3 REQUIRED)
    add_executable(hg_web_tests tests/http_client_test.cpp tests/http_server_test.cpp tests/websocket_client_test.cpp)
    target_compile_features(hg_web_tests PRIVATE cxx_std_23)
    target_link_libraries(
        hg_web_tests PRIVATE hg::web hg::library Catch2::Catch2WithMain OpenSSL::SSL OpenSSL::Crypto
                             ${HG_WEB_BOOST_TARGETS}
    )
    add_test(NAME hg_web_tests COMMAND hg_web_tests)
    set_tests_properties(
        hg_web_tests PROPERTIES TIMEOUT 10 ENVIRONMENT "NO_PROXY=127.0.0.1,localhost;no_proxy=127.0.0.1,localhost"
    )

    if(HG_BUILD_PYTHON)
        add_test(
            NAME hg_web_python_tests
            COMMAND "${Python_EXECUTABLE}" -m pytest -q "${CMAKE_CURRENT_SOURCE_DIR}/tests_python"
        )
        set_tests_properties(
            hg_web_python_tests
            PROPERTIES
                ENVIRONMENT_MODIFICATION
                    "PYTHONPATH=path_list_prepend:${PROJECT_BINARY_DIR}/extensions/web/python;PYTHONPATH=path_list_prepend:${PROJECT_BINARY_DIR}/bindings/python"
                ENVIRONMENT "NO_PROXY=127.0.0.1,localhost;no_proxy=127.0.0.1,localhost"
                TIMEOUT 15
        )
    endif()
endif()
