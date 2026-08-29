# Host build of the pure protocol code. Needs nothing but a C++20 compiler --
# no ESP-IDF, no network, no test framework to download.

cmake_minimum_required(VERSION 3.16)
project(bsh_host_tests C CXX)  # C is needed for cJSON; without it CMake silently drops .c files

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../components/dbus2_proto dbus2_proto)
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../components/dbus2_decoder dbus2_decoder)

enable_testing()

function(host_test name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE ${ARGN})
    target_compile_options(${name} PRIVATE -Wall -Wextra -g -fsanitize=address,undefined)
    target_link_options(${name} PRIVATE -fsanitize=address,undefined)
    add_test(NAME ${name} COMMAND ${name})
endfunction()

host_test(test_protocol dbus2_proto)
host_test(test_decoder dbus2_decoder)

if(DBUS2_HAVE_PARSER)
    host_test(test_profile dbus2_decoder_parser)

    # Not a unit test: a tool for checking generated profiles against the parser
    # that actually runs on the device.
    add_executable(validate_profiles validate_profiles.cpp)
    target_link_libraries(validate_profiles PRIVATE dbus2_decoder_parser)
else()
    message(STATUS "Skipping test_profile: cJSON unavailable")
endif()
