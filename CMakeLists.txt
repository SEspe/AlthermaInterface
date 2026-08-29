cmake_minimum_required(VERSION 3.16)

# Keep the ESP-IDF app descriptor in step with FIRMWARE_VERSION.
#
# Without this, PROJECT_VER comes from `git describe`, which gives three
# different answers for the same firmware: a stale tag plus a commit count on a
# development machine, a bare commit hash in CI (the checkout is shallow and has
# no tags), and something else again after tagging. The web installer and the
# boot banner read the descriptor while /api/status reads FIRMWARE_VERSION, so
# the two disagreed - a build reported as "1.7.0" by the device showed as
# "1.6.3" in the web installer purely because it was compiled before the tag was
# pushed.
#
# main/version.h is the single source of truth, and the release contract already
# requires bumping it with every functional change.
file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/main/version.h" _alt_ver_line
     REGEX "^[ 	]*#define[ 	]+FIRMWARE_VERSION")
if(NOT _alt_ver_line)
    message(FATAL_ERROR "FIRMWARE_VERSION not found in main/version.h")
endif()
string(REGEX MATCH "\"([^\"]+)\"" _alt_ver_match "${_alt_ver_line}")
set(PROJECT_VER "${CMAKE_MATCH_1}")
message(STATUS "AlthermaInterface version ${PROJECT_VER} (from main/version.h)")

# file(STRINGS) above runs at CONFIGURE time only. Without this, editing
# main/version.h rebuilds the app but never re-reads the version: CMake has no
# reason to re-run, PROJECT_VER keeps its previous value, and the app descriptor
# ships the OLD number while /api/status reports the new one. That is the exact
# split v1.14 set out to remove, reintroduced one layer down - firmware 1.8.0
# first built as 1.8.0 in FIRMWARE_VERSION and 1.7.1 in the descriptor.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             "${CMAKE_CURRENT_LIST_DIR}/main/version.h")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(AlthermaInterface)
