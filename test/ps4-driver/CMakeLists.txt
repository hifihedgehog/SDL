cmake_minimum_required(VERSION 3.22)
project(SDLPS4DriverTests LANGUAGES C CXX)

# An offline test of SDL_HIDAPI_DriverPS4, the HIDAPI PS4 driver, for
# hifihedgehog/SDL#41. The real driver runs against a scripted DualShock 4 on
# Bluetooth inside a real static SDL, whose joystick core and hints it uses.
# No HID device opens and SDL's HID layer never starts.
if(NOT WIN32 OR NOT CMAKE_C_COMPILER_ID STREQUAL "MSVC" OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "The driver test requires native MSVC for 64-bit Windows.")
endif()
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    message(FATAL_ERROR "Use a separate build directory for the offline tests.")
endif()

option(SDL_PS4_DRIVER_TEST_ASAN "Instrument the test and the static SDL with AddressSanitizer" OFF)
if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Offline test configuration" FORCE)
endif()
if(SDL_PS4_DRIVER_TEST_ASAN)
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

get_filename_component(PS4_SDL_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(PS4_TESTS "${PS4_SDL_ROOT}/test")

# The test always includes the tree's driver,
# src/joystick/hidapi/SDL_hidapi_ps4.c, by its path relative to
# testps4driver.c. No cache entry can put another source in its place.

# This cache belongs only to the standalone test build, as in
# test/wii-driver. The HIDAPI joystick backend is built, since the driver
# needs its headers and SDL_JOYSTICK_HIDAPI, and is never started. libusb
# stays out.
set(SDL_SHARED OFF CACHE BOOL "Offline static SDL dependency" FORCE)
set(SDL_STATIC ON CACHE BOOL "Offline static SDL dependency" FORCE)
set(SDL_TESTS OFF CACHE BOOL "Only register this sidecar's tests" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "No SDL test library needed" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "No example applications" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "No installation from this test build" FORCE)
set(SDL_LIBC ON CACHE BOOL "Use the native C runtime" FORCE)
set(SDL_XINPUT_PADDLES OFF CACHE BOOL "No production paddle acquisition" FORCE)
set(HAVE_GAMEINPUT_LIB OFF CACHE BOOL "Keep GameInput an optional dynamic dependency" FORCE)
set(SDL_HIDAPI ON CACHE BOOL "Required by the HIDAPI joystick backend" FORCE)
set(SDL_HIDAPI_JOYSTICK ON CACHE BOOL "The PS4 driver's backend" FORCE)
set(SDL_HIDAPI_LIBUSB OFF CACHE BOOL "No native USB acquisition" FORCE)
add_subdirectory("${PS4_SDL_ROOT}" "${CMAKE_CURRENT_BINARY_DIR}/sdl" EXCLUDE_FROM_ALL)
if(SDL_PS4_DRIVER_TEST_ASAN)
    target_compile_options(SDL3-static PRIVATE /fsanitize=address /Zi)
endif()

enable_testing()

# How the driver in the test reaches the scripted controller and not a
# device:
# - testps4driver.c defines every name through which the driver reads,
#   reads a feature report, writes through the rumble thread, adds a
#   joystick or looks one up, then includes the driver source.
# - The test's object defines SDL_HIDAPI_DriverPS4. The linker takes every
#   object named on its command line and searches a library only for a symbol
#   still undefined, so the HIDAPI layer's driver table binds to the test's
#   copy, and SDL's own SDL_hidapi_ps4.c object is never pulled. Were it
#   pulled for any other symbol, its definition would collide with the test's
#   and the link would fail with LNK2005.
# - Before the link, CheckSystem.cmake reads the test object's symbol table
#   and fails the build when it still imports a function that reaches a HID
#   device.
add_library(testps4driver_main OBJECT "${PS4_TESTS}/testps4driver.c")
target_compile_definitions(testps4driver_main PRIVATE USING_GENERATED_CONFIG_H SDL_STATIC_LIB)
target_include_directories(testps4driver_main PRIVATE
    "${PS4_SDL_ROOT}/src"
    "${CMAKE_CURRENT_BINARY_DIR}/sdl/include-config-$<LOWER_CASE:$<CONFIG>>/build_config"
    "${CMAKE_CURRENT_BINARY_DIR}/sdl/include-revision")
target_link_libraries(testps4driver_main PRIVATE SDL3::Headers)
# The driver leaves parameters of its HIDAPI entry points unused (C4100),
# which SDL's own /W3 build does not report. Every other warning stays an
# error.
target_compile_options(testps4driver_main PRIVATE /wd4100)

add_executable(testps4driver $<TARGET_OBJECTS:testps4driver_main>)
target_link_libraries(testps4driver PRIVATE SDL3-static)

foreach(target testps4driver_main testps4driver)
    set_target_properties(${target} PROPERTIES
        C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    target_compile_options(${target} PRIVATE /W4 /WX /utf-8)
    if(SDL_PS4_DRIVER_TEST_ASAN)
        target_compile_options(${target} PRIVATE /fsanitize=address /Zi)
    endif()
endforeach()
# The map shows which object supplies SDL_HIDAPI_DriverPS4
target_link_options(testps4driver PRIVATE /INCREMENTAL:NO "/MAP:${CMAKE_CURRENT_BINARY_DIR}/testps4driver.map")
if(SDL_PS4_DRIVER_TEST_ASAN)
    target_link_options(testps4driver PRIVATE /DEBUG)
endif()

get_filename_component(PS4_LINKER_DIR "${CMAKE_LINKER}" DIRECTORY)
find_program(PS4_DUMPBIN dumpbin HINTS "${PS4_LINKER_DIR}" NO_CACHE REQUIRED)
add_custom_command(TARGET testps4driver PRE_LINK
    COMMAND "${CMAKE_COMMAND}" "-DDUMPBIN=${PS4_DUMPBIN}"
        "-DOBJECT=$<TARGET_OBJECTS:testps4driver_main>"
        "-DREPORT=${CMAKE_CURRENT_BINARY_DIR}/testps4driver-externals.txt"
        -P "${CMAKE_CURRENT_LIST_DIR}/CheckSystem.cmake"
    VERBATIM)

# The test never starts SDL's joystick subsystem. Every joystick driver is
# off all the same, so none could add a device were one started. The PS4
# driver's own hints, which it reads, are cleared, and the test sets the
# enhanced-report hint itself.
set(PS4_DRIVER_OFFLINE_ENVIRONMENT
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
    "SDL_JOYSTICK_ICADE=0"
    "SDL_JOYSTICK_WINMM=0"
    "SDL_JOYSTICK_THREAD=0"
    "SDL_JOYSTICK_ROG_CHAKRAM=0")
set(PS4_DRIVER_CLEARED_ENVIRONMENT
    "SDL_JOYSTICK_SERIAL=unset:"
    "SDL_JOYSTICK_DJI_REMOTE_TCP_HOSTS=unset:"
    "SDL_JOYSTICK_HIDAPI_PS4=unset:"
    "SDL_JOYSTICK_HIDAPI_PS4_REPORT_INTERVAL=unset:"
    "SDL_JOYSTICK_ENHANCED_REPORTS=unset:")

add_test(NAME ps4-driver.bluetooth COMMAND testps4driver)
set_tests_properties(ps4-driver.bluetooth PROPERTIES
    TIMEOUT 120
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    ENVIRONMENT "${PS4_DRIVER_OFFLINE_ENVIRONMENT}"
    ENVIRONMENT_MODIFICATION "${PS4_DRIVER_CLEARED_ENVIRONMENT}")
set_property(TEST ps4-driver.bluetooth PROPERTY LABELS ps4 offline)
