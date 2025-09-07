cmake_minimum_required(VERSION 3.22)
project(ff_player)

set(CMAKE_C_STANDARD 11)

option(FF_PLAYER_STATIC "Build static library" ON)
option(FF_PLAYER_BUILD_EXAMPLES "Build examples" ON)

if (FF_PLAYER_STATIC)
    set(BUILD_TYPE STATIC)
else()
    set(BUILD_TYPE SHARED)
endif()

set(FF_PLAYER_INCLUDE_DIRS ${CMAKE_CURRENT_LIST_DIR}/include)

file(GLOB SOURCES
        "${CMAKE_CURRENT_LIST_DIR}/include/*.h"
        "${CMAKE_CURRENT_LIST_DIR}/src/*.c"
)

add_library(ff_player ${BUILD_TYPE} ${SOURCES})

if (FF_PLAYER_STATIC)
    set_target_properties(ff_player PROPERTIES LINK_SEARCH_START_STATIC 1)
    set_target_properties(ff_player PROPERTIES LINK_SEARCH_END_STATIC 1)
else()
    set_property(TARGET ff_player PROPERTY POSITION_INDEPENDENT_CODE True)
endif()

set_property(TARGET ff_player APPEND PROPERTY INCLUDE_DIRECTORIES "${FF_PLAYER_INCLUDE_DIRS}")
set_property(TARGET ff_player APPEND PROPERTY INTERFACE_INCLUDE_DIRECTORIES "${FF_PLAYER_INCLUDE_DIRS}")

find_package(ffmpeg QUIET)
if (NOT ffmpeg_FOUND)
    find_package(PkgConfig REQUIRED)

    pkg_check_modules(AVCODEC REQUIRED IMPORTED_TARGET libavcodec)
    pkg_check_modules(AVFORMAT REQUIRED IMPORTED_TARGET libavformat)
    pkg_check_modules(AVFILTER REQUIRED IMPORTED_TARGET libavfilter)
    pkg_check_modules(AVDEVICE REQUIRED IMPORTED_TARGET libavdevice)
    pkg_check_modules(AVUTIL REQUIRED IMPORTED_TARGET libavutil)
    pkg_check_modules(SWRESAMPLE REQUIRED IMPORTED_TARGET libswresample)
    pkg_check_modules(SWSCALE REQUIRED IMPORTED_TARGET libswscale)

    set(FFMPEG_INCLUDE_DIRS
            ${AVCODEC_INCLUDE_DIRS}
            ${AVFORMAT_INCLUDE_DIRS}
            ${AVFILTER_INCLUDE_DIRS}
            ${AVDEVICE_INCLUDE_DIRS}
            ${AVUTIL_INCLUDE_DIRS}
            ${SWRESAMPLE_INCLUDE_DIRS}
            ${SWSCALE_INCLUDE_DIRS}
    )
    set(FFMPEG_LIBRARIES
            ${AVCODEC_LIBRARIES}
            ${AVFORMAT_LIBRARIES}
            ${AVFILTER_LIBRARIES}
            ${AVDEVICE_LIBRARIES}
            ${AVUTIL_LIBRARIES}
            ${SWRESAMPLE_LIBRARIES}
            ${SWSCALE_LIBRARIES}
    )
    set(FFMPEG_LIBRARY_DIRS
            ${AVCODEC_LIBRARY_DIRS}
            ${AVFORMAT_LIBRARY_DIRS}
            ${AVFILTER_LIBRARY_DIRS}
            ${AVDEVICE_LIBRARY_DIRS}
            ${AVUTIL_LIBRARY_DIRS}
            ${SWRESAMPLE_LIBRARY_DIRS}
            ${SWSCALE_LIBRARY_DIRS}
    )
endif()

include(CheckIncludeFiles)

check_include_files(thread.h HAVE_THREAD_H)
if(NOT HAVE_THREAD_H)
    set(THIRD_PARTY_DIR ${CMAKE_CURRENT_LIST_DIR}/third_party)
    target_sources(ff_player PRIVATE ${THIRD_PARTY_DIR}/tinycthread/tinycthread.c)
    target_include_directories(ff_player PRIVATE ${THIRD_PARTY_DIR})
else()
    target_compile_definitions(ff_player PRIVATE HAVE_THREAD_H)
endif()

if(MSVC)
    target_compile_options(ff_player PRIVATE /experimental:c11atomics)
endif()

target_include_directories(ff_player PUBLIC ${FFMPEG_INCLUDE_DIRS})
target_link_directories(ff_player PUBLIC ${FFMPEG_LIBRARY_DIRS})
target_link_libraries(ff_player PUBLIC ${FFMPEG_LIBRARIES})

if (FF_PLAYER_BUILD_EXAMPLES)
   add_subdirectory(examples)
endif()
