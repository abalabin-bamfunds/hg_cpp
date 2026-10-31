cmake_minimum_required(VERSION 3.31)

project(bg_frame_superbuild LANGUAGES CXX)

include(CTest)

set(BG_FRAME_BG_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/../../.." CACHE PATH "BG source directory")
set(BG_FRAME_TPACK_SOURCE_DIR "" CACHE PATH "Optional local tpack source directory")
set(BG_FRAME_DDVISITOR_SOURCE_DIR "" CACHE PATH "Optional local ddvisitor source directory")

if(BG_FRAME_TPACK_SOURCE_DIR)
    set(FETCHCONTENT_SOURCE_DIR_TPACK "${BG_FRAME_TPACK_SOURCE_DIR}" CACHE PATH "" FORCE)
endif()
if(BG_FRAME_DDVISITOR_SOURCE_DIR)
    set(FETCHCONTENT_SOURCE_DIR_DDVISITOR "${BG_FRAME_DDVISITOR_SOURCE_DIR}" CACHE PATH "" FORCE)
endif()

set(BG_BUILD_PYTHON OFF CACHE BOOL "" FORCE)
set(BG_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)

add_subdirectory("${BG_FRAME_BG_SOURCE_DIR}" bg)
add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/.." frame)
