#-------------------------------------------------------------------------------
# padnote-- — Qt 6 Linux text editor
#
# Top-level CMake build. Produces an executable `padnote` (the brand
# is "padnote--", the executable filename drops the trailing dashes
# for shell-friendliness).
#-------------------------------------------------------------------------------

cmake_minimum_required(VERSION 3.20)

project(padnote
    VERSION 1.0.0
    DESCRIPTION "padnote-- — Qt 6 Linux text editor"
    HOMEPAGE_URL ""
    LANGUAGES C CXX
)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
include(NppCommon)

#-- Build options --------------------------------------------------------------

option(NPP_BUILD_TESTS  "Build unit tests"               OFF)

#-- Qt 6 -----------------------------------------------------------------------

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt6 6.4 REQUIRED COMPONENTS Core Gui Widgets Core5Compat PrintSupport DBus)

#-- Subdirectories -------------------------------------------------------------

add_subdirectory(scintilla)
add_subdirectory(lexilla)
add_subdirectory(PowerEditor/src/linux)
add_subdirectory(plugins)

#-- Build summary --------------------------------------------------------------

message(STATUS "")
message(STATUS "padnote-- build configuration")
message(STATUS "  CMake version       : ${CMAKE_VERSION}")
message(STATUS "  Build type          : ${CMAKE_BUILD_TYPE}")
message(STATUS "  C++ compiler        : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  C++ standard        : ${CMAKE_CXX_STANDARD}")
message(STATUS "  Qt6 version         : ${Qt6_VERSION}")
message(STATUS "  Qt6 install prefix  : ${QT6_INSTALL_PREFIX}")
message(STATUS "  Tests enabled       : ${NPP_BUILD_TESTS}")
message(STATUS "")
