cmake_minimum_required(VERSION 3.22)
project(SDLCommunityMappingTests LANGUAGES C CXX)

# The generator of src/joystick/SDL_gamepad_db_community.h and its tests. Both
# call internal HIDAPI and GUID functions the shared library does not export,
# so they link a static SDL built from this tree.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    message(FATAL_ERROR "Use a separate build directory for the community mapping tests.")
endif()
if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Community mapping test configuration" FORCE)
endif()

get_filename_component(COMMUNITY_SDL_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(COMMUNITY_DB "${COMMUNITY_SDL_ROOT}/build-scripts/gamecontrollerdb/gamecontrollerdb.txt")
set(COMMUNITY_SOURCE "${COMMUNITY_SDL_ROOT}/build-scripts/gamecontrollerdb/source.txt")
set(COMMUNITY_GAMEPAD_DB "${COMMUNITY_SDL_ROOT}/src/joystick/SDL_gamepad_db.h")
set(COMMUNITY_HEADER "${COMMUNITY_SDL_ROOT}/src/joystick/SDL_gamepad_db_community.h")

# Configure it with the same libusb settings as the shared library, because
# the Switch 2 driver exists only with libusb (HAVE_LIBUSB). The generator
# refuses to run when that driver is missing.
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory("${COMMUNITY_SDL_ROOT}" sdl EXCLUDE_FROM_ALL)

function(community_program target source)
    add_executable(${target} "${source}")
    target_link_libraries(${target} PRIVATE SDL3::SDL3-static)
    set_target_properties(${target} PROPERTIES
        C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
    endif()
endfunction()

community_program(gen_community_mappings "${COMMUNITY_SDL_ROOT}/build-scripts/gen_community_mappings.c")
community_program(testcommunitymappings "${COMMUNITY_SDL_ROOT}/test/testcommunitymappings.c")

# cmake --build <dir> --target community-mappings-write regenerates the header.
add_custom_target(community-mappings-write
    COMMAND gen_community_mappings write "${COMMUNITY_DB}" "${COMMUNITY_SOURCE}" "${COMMUNITY_GAMEPAD_DB}" "${COMMUNITY_HEADER}"
    DEPENDS gen_community_mappings
    VERBATIM)

enable_testing()
add_test(NAME community-mappings.regenerate
    COMMAND gen_community_mappings check "${COMMUNITY_DB}" "${COMMUNITY_SOURCE}" "${COMMUNITY_GAMEPAD_DB}" "${COMMUNITY_HEADER}")
add_test(NAME community-mappings.rows
    COMMAND testcommunitymappings "${COMMUNITY_GAMEPAD_DB}")
set_tests_properties(community-mappings.regenerate community-mappings.rows PROPERTIES
    TIMEOUT 300 LABELS "community-mappings;offline")
