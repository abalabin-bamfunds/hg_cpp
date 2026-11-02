cmake_minimum_required(VERSION 3.31)

project(hg_frame_superbuild LANGUAGES CXX)

include(CTest)

set(HG_FRAME_HG_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/../../.." CACHE PATH "HG source directory")
set(HG_FRAME_TPACK_SOURCE_DIR "" CACHE PATH "Optional local tpack source directory")
set(HG_FRAME_DDVISITOR_SOURCE_DIR "" CACHE PATH "Optional local ddvisitor source directory")

if(HG_FRAME_TPACK_SOURCE_DIR)
    set(FETCHCONTENT_SOURCE_DIR_TPACK "${HG_FRAME_TPACK_SOURCE_DIR}" CACHE PATH "" FORCE)
endif()
if(HG_FRAME_DDVISITOR_SOURCE_DIR)
    set(FETCHCONTENT_SOURCE_DIR_DDVISITOR "${HG_FRAME_DDVISITOR_SOURCE_DIR}" CACHE PATH "" FORCE)
endif()

set(HG_BUILD_PYTHON OFF CACHE BOOL "" FORCE)
set(HG_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)

add_subdirectory("${HG_FRAME_HG_SOURCE_DIR}" hg)
add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/.." frame)
