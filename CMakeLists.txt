##
## Audio Workstation - top level build.
##
## The build is deliberately split into one static library per architectural
## layer.  A layer may only link against layers *below* it (see docs/architecture.md).
## CMake enforces that rule mechanically: if a module does not list a dependency
## it simply cannot see its headers, which makes accidental coupling a build
## error instead of a code review comment.
##
cmake_minimum_required(VERSION 3.22)

project(audio-workstation
        VERSION 0.1.0
        DESCRIPTION "Standalone hardware multitrack recorder/editor"
        LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

include(AwPlatform)
include(AwCompilerOptions)

option(AW_BUILD_TESTS "Build the unit test suite" ON)

add_subdirectory(src)

if(AW_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

message(STATUS "")
message(STATUS "audio-workstation ${PROJECT_VERSION}")
message(STATUS "  build type ....... ${CMAKE_BUILD_TYPE}")
message(STATUS "  platform backend . ${AW_PLATFORM}")
message(STATUS "  unit tests ....... ${AW_BUILD_TESTS}")
message(STATUS "")
