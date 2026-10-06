cmake_minimum_required(VERSION 3.22)
project(SDLSwitch2DriverTests LANGUAGES C CXX)

# Offline tests of the Switch 2 driver's Windows handle search, for
# hifihedgehog/SDL#37.
# - testswitch2driver runs SDL_HIDAPI_DriverSwitch2 inside a real static SDL
#   against a scripted USB bus, where several units share the controller's
#   vendor and product ID.
# - testhidmaestroserial runs the HIDMaestro serial lookup the driver calls,
#   in the Windows HID backend built alone, against a scripted device tree.
# No USB or HID device opens and the HIDAPI layer never starts.
if(NOT WIN32 OR NOT CMAKE_C_COMPILER_ID STREQUAL "MSVC" OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "The driver test requires native MSVC for 64-bit Windows.")
endif()
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    message(FATAL_ERROR "Use a separate build directory for the offline tests.")
endif()
if(NOT LibUSB_INCLUDE_PATH)
    message(FATAL_ERROR "Set LibUSB_INCLUDE_PATH and LibUSB_LIBRARY, as for the shared library. The Switch 2 driver builds only with libusb.")
endif()

option(SDL_SWITCH2_DRIVER_TEST_ASAN "Instrument the tests and the static SDL with AddressSanitizer" OFF)
if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Offline test configuration" FORCE)
endif()
if(SDL_SWITCH2_DRIVER_TEST_ASAN)
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

get_filename_component(SWITCH2_SDL_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(SWITCH2_TESTS "${SWITCH2_SDL_ROOT}/test")

# The tests always include the tree's sources,
# src/joystick/hidapi/SDL_hidapi_switch2.c and src/hidapi/windows/hid.c, by
# their paths relative to the test files. No cache entry can put another
# source in their place.

# This cache belongs only to the standalone test build, as in
# test/wii-driver. SDL_HIDAPI_LIBUSB, SDL_HIDAPI_LIBUSB_SHARED and the libusb
# paths come from the command line, as for the shared library: the driver and
# its SDL_JOYSTICK_HIDAPI_SWITCH2 exist only with libusb. The HIDAPI joystick
# backend is built and never started, and libusb is never loaded.
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
set(SDL_HIDAPI_JOYSTICK ON CACHE BOOL "The Switch 2 driver's backend" FORCE)
add_subdirectory("${SWITCH2_SDL_ROOT}" "${CMAKE_CURRENT_BINARY_DIR}/sdl" EXCLUDE_FROM_ALL)
if(SDL_SWITCH2_DRIVER_TEST_ASAN)
    target_compile_options(SDL3-static PRIVATE /fsanitize=address /Zi)
endif()

enable_testing()

# How the driver in the test reaches the scripted bus and not a device:
# - testswitch2driver.c defines every name through which the driver gets its
#   libusb function table, reads the HID serial number, asks about
#   HIDMaestro, queues a rumble write or adds a joystick, then includes the
#   driver source.
# - The test's object defines SDL_HIDAPI_DriverSwitch2. The linker takes
#   every object named on its command line and searches a library only for a
#   symbol still undefined, so the HIDAPI layer's driver table binds to the
#   test's copy, and SDL's own SDL_hidapi_switch2.c object is never pulled.
#   Were it pulled for any other symbol, its definition would collide with
#   the test's and the link would fail with LNK2005.
# - Before the link, CheckSystem.cmake reads the test object's symbol table
#   and fails the build when it still imports a function that reaches a
#   device.
add_library(testswitch2driver_main OBJECT "${SWITCH2_TESTS}/testswitch2driver.c")
target_compile_definitions(testswitch2driver_main PRIVATE USING_GENERATED_CONFIG_H SDL_STATIC_LIB)
target_include_directories(testswitch2driver_main PRIVATE
    "${SWITCH2_SDL_ROOT}/src"
    "${CMAKE_CURRENT_BINARY_DIR}/sdl/include-config-$<LOWER_CASE:$<CONFIG>>/build_config"
    "${CMAKE_CURRENT_BINARY_DIR}/sdl/include-revision"
    "${LibUSB_INCLUDE_PATH}")
target_link_libraries(testswitch2driver_main PRIVATE SDL3::Headers)

add_executable(testswitch2driver $<TARGET_OBJECTS:testswitch2driver_main>)
target_link_libraries(testswitch2driver PRIVATE SDL3-static)

# The Windows HID backend as hidapi builds it alone, with no SDL. The test
# includes it, fills the function pointers it would resolve from hid.dll and
# cfgmgr32.dll, and defines the two Win32 calls it makes on a device path.
add_library(testhidmaestroserial_main OBJECT "${SWITCH2_TESTS}/testhidmaestroserial.c")
add_executable(testhidmaestroserial $<TARGET_OBJECTS:testhidmaestroserial_main>)

foreach(target testswitch2driver_main testswitch2driver testhidmaestroserial_main testhidmaestroserial)
    set_target_properties(${target} PROPERTIES
        C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    target_compile_options(${target} PRIVATE /W4 /WX /utf-8)
    if(SDL_SWITCH2_DRIVER_TEST_ASAN)
        target_compile_options(${target} PRIVATE /fsanitize=address /Zi)
    endif()
endforeach()
# The map shows which object supplies SDL_HIDAPI_DriverSwitch2
target_link_options(testswitch2driver PRIVATE /INCREMENTAL:NO "/MAP:${CMAKE_CURRENT_BINARY_DIR}/testswitch2driver.map")
target_link_options(testhidmaestroserial PRIVATE /INCREMENTAL:NO)
if(SDL_SWITCH2_DRIVER_TEST_ASAN)
    target_link_options(testswitch2driver PRIVATE /DEBUG)
    target_link_options(testhidmaestroserial PRIVATE /DEBUG)
endif()

get_filename_component(SWITCH2_LINKER_DIR "${CMAKE_LINKER}" DIRECTORY)
find_program(SWITCH2_DUMPBIN dumpbin HINTS "${SWITCH2_LINKER_DIR}" NO_CACHE REQUIRED)
add_custom_command(TARGET testswitch2driver PRE_LINK
    COMMAND "${CMAKE_COMMAND}" "-DDUMPBIN=${SWITCH2_DUMPBIN}" -DKIND=driver
        "-DOBJECT=$<TARGET_OBJECTS:testswitch2driver_main>"
        "-DREPORT=${CMAKE_CURRENT_BINARY_DIR}/testswitch2driver-externals.txt"
        -P "${CMAKE_CURRENT_LIST_DIR}/CheckSystem.cmake"
    VERBATIM)
add_custom_command(TARGET testhidmaestroserial PRE_LINK
    COMMAND "${CMAKE_COMMAND}" "-DDUMPBIN=${SWITCH2_DUMPBIN}" -DKIND=backend
        "-DOBJECT=$<TARGET_OBJECTS:testhidmaestroserial_main>"
        "-DREPORT=${CMAKE_CURRENT_BINARY_DIR}/testhidmaestroserial-externals.txt"
        -P "${CMAKE_CURRENT_LIST_DIR}/CheckSystem.cmake"
    VERBATIM)

# The driver test never starts SDL's joystick subsystem. Every joystick
# driver is off all the same, so none could add a device were one started.
# The Switch 2 driver's own hints are cleared.
set(SWITCH2_DRIVER_OFFLINE_ENVIRONMENT
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
    "SDL_JOYSTICK_ROG_CHAKRAM=0"
    "SDL_HIDAPI_LIBUSB=0")
set(SWITCH2_DRIVER_CLEARED_ENVIRONMENT
    "SDL_JOYSTICK_SERIAL=unset:"
    "SDL_JOYSTICK_DJI_REMOTE_TCP_HOSTS=unset:"
    "SDL_JOYSTICK_HIDAPI_SWITCH2=unset:"
    "SDL_JOYSTICK_HIDAPI_SWITCH_PLAYER_LED=unset:")

add_test(NAME switch2-driver.search COMMAND testswitch2driver)
set_tests_properties(switch2-driver.search PROPERTIES
    TIMEOUT 120
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    ENVIRONMENT "${SWITCH2_DRIVER_OFFLINE_ENVIRONMENT}"
    ENVIRONMENT_MODIFICATION "${SWITCH2_DRIVER_CLEARED_ENVIRONMENT}")
set_property(TEST switch2-driver.search PROPERTY LABELS switch2 offline)

add_test(NAME switch2-driver.hidmaestro-serial COMMAND testhidmaestroserial)
set_tests_properties(switch2-driver.hidmaestro-serial PROPERTIES
    TIMEOUT 120
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
set_property(TEST switch2-driver.hidmaestro-serial PROPERTY LABELS switch2 offline)
