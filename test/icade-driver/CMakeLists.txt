cmake_minimum_required(VERSION 3.22)
project(SDLICadeDriverTests LANGUAGES C CXX)

# An offline test of SDL_ICADE_JoystickDriver, the iCade driver of
# hifihedgehog/SDL#33 Part 16. The real driver runs inside a real static SDL
# against a fake Win32 system, so its thread, its message-only window, the
# Raw Input records, the device list, the host feed, the joysticks and the
# gamepad mappings all run without a keyboard and without a real Raw Input
# registration.
if(NOT WIN32 OR NOT CMAKE_C_COMPILER_ID STREQUAL "MSVC" OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "The driver test requires native MSVC for 64-bit Windows.")
endif()
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    message(FATAL_ERROR "Use a separate build directory for the offline tests.")
endif()

option(SDL_ICADE_DRIVER_TEST_ASAN "Instrument the test and the static SDL with AddressSanitizer" OFF)
if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Offline test configuration" FORCE)
endif()
if(SDL_ICADE_DRIVER_TEST_ASAN)
    # Debug's default /RTC1 conflicts with MSVC AddressSanitizer.
    if(CMAKE_CONFIGURATION_TYPES)
        set(CMAKE_CONFIGURATION_TYPES Release RelWithDebInfo CACHE STRING "ASan configurations" FORCE)
    elseif(NOT CMAKE_BUILD_TYPE MATCHES "^(Release|RelWithDebInfo)$")
        message(FATAL_ERROR "ASan requires Release or RelWithDebInfo.")
    endif()
    # Without optimization every load stays where the source puts it, so a
    # read before a length check reaches the sanitizer. This also applies to
    # the static SDL below.
    foreach(config RELEASE RELWITHDEBINFO)
        string(REPLACE "/O2" "/Od" CMAKE_C_FLAGS_${config} "${CMAKE_C_FLAGS_${config}}")
    endforeach()
endif()

get_filename_component(ICADE_SDL_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(ICADE_TESTS "${ICADE_SDL_ROOT}/test")

# The test always includes the tree's driver,
# src/joystick/windows/SDL_icadejoystick.c, by its path relative to
# testicade.c. No cache entry can put another source in its place.

# This cache belongs only to the standalone test build, as in
# test/xinput-paddles. The real SDL archive supplies the joystick, gamepad,
# event, hint and thread code the driver runs on.
set(SDL_SHARED OFF CACHE BOOL "Offline static SDL dependency" FORCE)
set(SDL_STATIC ON CACHE BOOL "Offline static SDL dependency" FORCE)
set(SDL_TESTS OFF CACHE BOOL "Only register this sidecar's tests" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "No SDL test library needed" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "No example applications" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "No installation from this test build" FORCE)
set(SDL_LIBC ON CACHE BOOL "Use the native C runtime" FORCE)
set(SDL_XINPUT_PADDLES OFF CACHE BOOL "No production paddle acquisition" FORCE)
set(HAVE_GAMEINPUT_LIB OFF CACHE BOOL "Keep GameInput an optional dynamic dependency" FORCE)
# Virtual joysticks depend on SDL_HIDAPI in this tree. Keep the subsystem but
# disable both its joystick backend and libusb acquisition.
set(SDL_HIDAPI ON CACHE BOOL "Required by the virtual joystick option" FORCE)
set(SDL_HIDAPI_JOYSTICK OFF CACHE BOOL "No HIDAPI device acquisition" FORCE)
set(SDL_HIDAPI_LIBUSB OFF CACHE BOOL "No native USB acquisition" FORCE)
set(SDL_VIRTUAL_JOYSTICK ON CACHE BOOL "Virtual input" FORCE)
add_subdirectory("${ICADE_SDL_ROOT}" "${CMAKE_CURRENT_BINARY_DIR}/sdl" EXCLUDE_FROM_ALL)
if(SDL_ICADE_DRIVER_TEST_ASAN)
    target_compile_options(SDL3-static PRIVATE /fsanitize=address /Zi)
endif()

# The pure decoder comes from the static SDL, whose Windows build takes
# src/joystick/windows.
get_target_property(ICADE_SDL_SOURCES SDL3-static SOURCES)
set(ICADE_SDL_MODULE)
foreach(source IN LISTS ICADE_SDL_SOURCES)
    if(source MATCHES "/src/joystick/windows/SDL_icade_proto\\.c$")
        set(ICADE_SDL_MODULE "${source}")
    endif()
endforeach()
if(NOT ICADE_SDL_MODULE)
    message(FATAL_ERROR "The static SDL does not build src/joystick/windows/SDL_icade_proto.c.")
endif()

enable_testing()

# How the driver in the test reaches the fake system and not Windows:
# - testicade.c defines every name through which the driver reaches a
#   device, hid.dll, its own module or the Raw Input registration, then
#   includes the driver source.
# - The test's object defines SDL_ICADE_JoystickDriver and the host feed.
#   The linker takes every object named on its command line and searches a
#   library only for a symbol still undefined, so SDL_joystick.c's driver
#   table and the dynamic API bind to the test's copy, and SDL's own
#   SDL_icadejoystick.c object is never pulled. Were it pulled for any other
#   symbol, its definitions would collide with the test's and the link would
#   fail with LNK2005.
# - Before the link, CheckSystem.cmake reads the test object's symbol table
#   and fails the build when it still imports a Win32 function that reaches
#   a device, loads a library or registers, or uses SDL's shared hid.dll
#   state.
add_library(testicade_main OBJECT "${ICADE_TESTS}/testicade.c")
target_compile_definitions(testicade_main PRIVATE USING_GENERATED_CONFIG_H SDL_STATIC_LIB)
target_include_directories(testicade_main PRIVATE
    "${ICADE_SDL_ROOT}/src"
    "${CMAKE_CURRENT_BINARY_DIR}/sdl/include-config-$<LOWER_CASE:$<CONFIG>>/build_config"
    "${CMAKE_CURRENT_BINARY_DIR}/sdl/include-revision")
target_link_libraries(testicade_main PRIVATE SDL3::Headers)
# core/windows/SDL_hid.h, which the driver includes, declares nameless
# unions (C4201). Every other warning stays an error.
target_compile_options(testicade_main PRIVATE /wd4201)

add_executable(testicade $<TARGET_OBJECTS:testicade_main> "${ICADE_TESTS}/testicadehost.c")
target_link_libraries(testicade PRIVATE SDL3-static)

foreach(target testicade_main testicade)
    set_target_properties(${target} PROPERTIES
        C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    target_compile_options(${target} PRIVATE /W4 /WX /utf-8)
    if(SDL_ICADE_DRIVER_TEST_ASAN)
        target_compile_options(${target} PRIVATE /fsanitize=address /Zi)
    endif()
endforeach()
# The map shows which object supplies SDL_ICADE_JoystickDriver
target_link_options(testicade PRIVATE /INCREMENTAL:NO "/MAP:${CMAKE_CURRENT_BINARY_DIR}/testicade.map")
if(SDL_ICADE_DRIVER_TEST_ASAN)
    target_link_options(testicade PRIVATE /DEBUG)
endif()

get_filename_component(ICADE_LINKER_DIR "${CMAKE_LINKER}" DIRECTORY)
find_program(ICADE_DUMPBIN dumpbin HINTS "${ICADE_LINKER_DIR}" NO_CACHE REQUIRED)
add_custom_command(TARGET testicade PRE_LINK
    COMMAND "${CMAKE_COMMAND}" "-DDUMPBIN=${ICADE_DUMPBIN}"
        "-DOBJECT=$<TARGET_OBJECTS:testicade_main>"
        "-DREPORT=${CMAKE_CURRENT_BINARY_DIR}/testicade-externals.txt"
        -P "${CMAKE_CURRENT_LIST_DIR}/CheckSystem.cmake"
    VERBATIM)

# Every other joystick driver is off, so only the driver under test can add
# a joystick. The test also sets these hints at override priority before
# each SDL_Init. The driver's own hints are cleared, since the test sets them.
set(ICADE_DRIVER_OFFLINE_ENVIRONMENT
    "SDL_JOYSTICK_HIDAPI=0"
    "SDL_JOYSTICK_RAWINPUT=0"
    "SDL_JOYSTICK_DIRECTINPUT=0"
    "SDL_XINPUT_ENABLED=0"
    "SDL_JOYSTICK_WGI=0"
    "SDL_JOYSTICK_GAMEINPUT=0"
    "SDL_JOYSTICK_GAMEINPUT_RAW=0"
    "SDL_JOYSTICK_BLE=0"
    "SDL_JOYSTICK_BLE_SWITCH2=0"
    "SDL_JOYSTICK_SERIAL_AUTO=0"
    "SDL_JOYSTICK_RFCOMM=0"
    "SDL_JOYSTICK_WINMM=0"
    "SDL_JOYSTICK_THREAD=0"
    "SDL_JOYSTICK_ROG_CHAKRAM=0")
set(ICADE_DRIVER_CLEARED_ENVIRONMENT
    "SDL_JOYSTICK_SERIAL=unset:"
    "SDL_JOYSTICK_DJI_REMOTE_TCP_HOSTS=unset:"
    "SDL_JOYSTICK_ICADE=unset:"
    "SDL_JOYSTICK_ICADE_RAWINPUT=unset:"
    "SDL_JOYSTICK_ICADE_DEVICES=unset:")

add_test(NAME icade-driver.raw COMMAND testicade)
set_tests_properties(icade-driver.raw PROPERTIES
    TIMEOUT 300
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    ENVIRONMENT "${ICADE_DRIVER_OFFLINE_ENVIRONMENT}"
    ENVIRONMENT_MODIFICATION "${ICADE_DRIVER_CLEARED_ENVIRONMENT}")
set_property(TEST icade-driver.raw PROPERTY LABELS icade-joystick offline)
