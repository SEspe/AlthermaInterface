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

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(AlthermaInterface)
