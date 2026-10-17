cmake_minimum_required(VERSION 3.16.0)

# The version stamped into the app descriptor, worked out here rather than left
# to ESP-IDF's own `git describe --dirty`.
#
# The build touches the dependency lock without changing a byte of it, and a stat
# comparison then reports the tree as modified -- so a release built from a
# clean checkout stamped itself "-dirty" and the archived image disagreed with
# its tag. Refreshing the index first compares content instead of timestamps.
find_package(Git QUIET)
if(GIT_FOUND AND EXISTS ${CMAKE_CURRENT_LIST_DIR}/.git)
    execute_process(COMMAND ${GIT_EXECUTABLE} update-index -q --refresh
                    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
                    OUTPUT_QUIET ERROR_QUIET)
    execute_process(COMMAND ${GIT_EXECUTABLE} describe --tags --always --dirty
                    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
                    OUTPUT_VARIABLE BSH_VERSION
                    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(BSH_VERSION)
        set(PROJECT_VER "${BSH_VERSION}")
        message(STATUS "Firmware version: ${PROJECT_VER}")
    endif()
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# One lock file per chip. The component manager records the target it resolved
# for, so with a single file every build for the other board rewrote it and the
# tree never stayed clean -- and a release built from a tree that is not clean
# stamps itself "-dirty". The pinned versions are the same for both.
idf_build_set_property(DEPENDENCIES_LOCK dependencies.lock.${IDF_TARGET})

# Not renamed with the repository, deliberately. This string becomes
# project_name in the app descriptor, and the OTA handler refuses an image whose
# project_name differs from the running one -- the guard against flashing
# somebody else's firmware. Changing it here would make every deployed device
# reject the next update and need a cable, which is the one thing a board behind
# an appliance does not have.
project(bsh-dbus-idf)
