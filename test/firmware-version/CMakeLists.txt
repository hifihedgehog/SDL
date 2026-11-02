cmake_minimum_required(VERSION 3.22)
project(SDLFirmwareVersionTests LANGUAGES C CXX)

# Offline tests of the firmware version five HIDAPI drivers report: GIP, Xbox
# One, Switch and Flydigi for hifihedgehog/SDL#42, and the PS5 driver's read
# after a Bluetooth DualSense leaves simple mode for #43. Each real driver
# runs against a scripted controller inside a real static SDL, whose joystick
# core and hints it uses. No HID device opens and SDL's HID layer never
# starts. The XInput driver and its lookup in GameInput's device information
# (#44) run against scripted XInput slots and a scripted GameInput.
if(NOT WIN32 OR NOT CMAKE_C_COMPILER_ID STREQUAL "MSVC" OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "The firmware version tests require native MSVC for 64-bit Windows.")
endif()
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    message(FATAL_ERROR "Use a separate build directory for the offline tests.")
endif()

option(SDL_FIRMWARE_VERSION_TEST_ASAN "Instrument the tests and the static SDL with AddressSanitizer" OFF)
if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Offline test configuration" FORCE)
endif()
if(SDL_FIRMWARE_VERSION_TEST_ASAN)
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

get_filename_component(FIRMWARE_SDL_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(FIRMWARE_TESTS "${FIRMWARE_SDL_ROOT}/test")

# Each test always includes the tree's driver from src/joystick/hidapi by its
# path relative to the test source. No cache entry can put another source in
# its place.

# This cache belongs only to the standalone test build, as in
# test/ps4-driver. The HIDAPI joystick backend is built, since the drivers
# need its headers and SDL_JOYSTICK_HIDAPI, and is never started. libusb
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
set(SDL_HIDAPI_JOYSTICK ON CACHE BOOL "The drivers' backend" FORCE)
set(SDL_HIDAPI_LIBUSB OFF CACHE BOOL "No native USB acquisition" FORCE)
add_subdirectory("${FIRMWARE_SDL_ROOT}" "${CMAKE_CURRENT_BINARY_DIR}/sdl" EXCLUDE_FROM_ALL)
if(SDL_FIRMWARE_VERSION_TEST_ASAN)
    target_compile_options(SDL3-static PRIVATE /fsanitize=address /Zi)
endif()

enable_testing()

get_filename_component(FIRMWARE_LINKER_DIR "${CMAKE_LINKER}" DIRECTORY)
find_program(FIRMWARE_DUMPBIN dumpbin HINTS "${FIRMWARE_LINKER_DIR}" NO_CACHE REQUIRED)

# The scripted controller, shared by the tests. It calls SDL only to
# lock the joystick list, mark the joystick record valid and free memory.
add_library(testfirmwarefake OBJECT "${FIRMWARE_TESTS}/testfirmwarefake.c")

# How a driver in a test reaches the scripted controller and not a device:
# - The test source includes testfirmwarefake.h, which defines every name
#   through which the driver reads, writes, queues a write on the rumble
#   thread, adds a joystick, looks one up, reads the clock or sleeps, then
#   includes the driver source.
# - The test's object defines the driver's table. The linker takes every
#   object named on its command line and searches a library only for a symbol
#   still undefined, so the HIDAPI layer's driver list binds to the test's
#   copy, and SDL's own object for the driver is never pulled. Were it pulled
#   for any other symbol, its definition would collide with the test's and
#   the link would fail with LNK2005.
# - Before the link, CheckSystem.cmake reads the test object's and the fake's
#   symbol tables and fails the build when either still imports a function
#   that reaches a HID device.
set(FIRMWARE_TEST_NAMES gip xboxone switch flydigi ps5)
foreach(name IN LISTS FIRMWARE_TEST_NAMES)
    add_library(test${name}firmware_main OBJECT "${FIRMWARE_TESTS}/test${name}firmware.c")
    add_executable(test${name}firmware $<TARGET_OBJECTS:test${name}firmware_main> $<TARGET_OBJECTS:testfirmwarefake>)
    target_link_libraries(test${name}firmware PRIVATE SDL3-static)
    # The map shows which object supplies the driver's table
    target_link_options(test${name}firmware PRIVATE /INCREMENTAL:NO "/MAP:${CMAKE_CURRENT_BINARY_DIR}/test${name}firmware.map")
    if(SDL_FIRMWARE_VERSION_TEST_ASAN)
        target_link_options(test${name}firmware PRIVATE /DEBUG)
    endif()
    add_custom_command(TARGET test${name}firmware PRE_LINK
        COMMAND "${CMAKE_COMMAND}" "-DDUMPBIN=${FIRMWARE_DUMPBIN}"
            "-DOBJECT=$<TARGET_OBJECTS:test${name}firmware_main>"
            "-DREPORT=${CMAKE_CURRENT_BINARY_DIR}/test${name}firmware-externals.txt"
            -P "${CMAKE_CURRENT_LIST_DIR}/CheckSystem.cmake"
        COMMAND "${CMAKE_COMMAND}" "-DDUMPBIN=${FIRMWARE_DUMPBIN}"
            "-DOBJECT=$<TARGET_OBJECTS:testfirmwarefake>"
            "-DREPORT=${CMAKE_CURRENT_BINARY_DIR}/testfirmwarefake-externals-${name}.txt"
            -P "${CMAKE_CURRENT_LIST_DIR}/CheckSystem.cmake"
        VERBATIM)
endforeach()

# The XInput driver and its firmware lookup (hifihedgehog/SDL#44).
# testxinputfirmware.c includes the driver and scripts the XInput slots.
# testxinputfirmware_gameinput.cpp includes SDL_xinput_firmware.cpp and
# scripts GameInput. Both objects also fail the build if they call the real
# GameInput start, the configuration manager lookup, the XInput loader,
# device notifications, or the functions that change SDL's joystick list.
set(XINPUT_FIRMWARE_FORBIDDEN "${CMAKE_CURRENT_BINARY_DIR}/xinput-firmware-forbidden.txt")
file(WRITE "${XINPUT_FIRMWARE_FORBIDDEN}"
    "SDL_InitGameInput|SDL_QuitGameInput|GameInputInitialize|GameInputCreate|WIN_GetDeviceInterfaceInstanceId|SDL_XINPUT_ReadSlotRecord|WIN_LoadXInputDLL|WIN_UnloadXInputDLL|WIN_InitDeviceNotification|WIN_QuitDeviceNotification|WIN_GetLastDeviceNotification|SDL_PrivateJoystickAdded|SDL_PrivateJoystickRemoved|SDL_JoystickHandledByAnotherDriver|^(__imp_)?XInput\n")
add_library(testxinputfirmware_main OBJECT "${FIRMWARE_TESTS}/testxinputfirmware.c")
add_library(testxinputfirmware_gameinput OBJECT "${FIRMWARE_TESTS}/testxinputfirmware_gameinput.cpp")
add_executable(testxinputfirmware $<TARGET_OBJECTS:testxinputfirmware_main> $<TARGET_OBJECTS:testxinputfirmware_gameinput>)
target_link_libraries(testxinputfirmware PRIVATE SDL3-static)
target_link_options(testxinputfirmware PRIVATE /INCREMENTAL:NO "/MAP:${CMAKE_CURRENT_BINARY_DIR}/testxinputfirmware.map")
if(SDL_FIRMWARE_VERSION_TEST_ASAN)
    target_link_options(testxinputfirmware PRIVATE /DEBUG)
endif()
add_custom_command(TARGET testxinputfirmware PRE_LINK
    COMMAND "${CMAKE_COMMAND}" "-DDUMPBIN=${FIRMWARE_DUMPBIN}"
        "-DOBJECT=$<TARGET_OBJECTS:testxinputfirmware_main>"
        "-DREPORT=${CMAKE_CURRENT_BINARY_DIR}/testxinputfirmware-externals.txt"
        "-DEXTRA_FILE=${XINPUT_FIRMWARE_FORBIDDEN}"
        -P "${CMAKE_CURRENT_LIST_DIR}/CheckSystem.cmake"
    COMMAND "${CMAKE_COMMAND}" "-DDUMPBIN=${FIRMWARE_DUMPBIN}"
        "-DOBJECT=$<TARGET_OBJECTS:testxinputfirmware_gameinput>"
        "-DREPORT=${CMAKE_CURRENT_BINARY_DIR}/testxinputfirmware-gameinput-externals.txt"
        "-DEXTRA_FILE=${XINPUT_FIRMWARE_FORBIDDEN}"
        -P "${CMAKE_CURRENT_LIST_DIR}/CheckSystem.cmake"
    VERBATIM)

set(FIRMWARE_OBJECTS testfirmwarefake testxinputfirmware_main testxinputfirmware_gameinput testxinputfirmware)
foreach(name IN LISTS FIRMWARE_TEST_NAMES)
    list(APPEND FIRMWARE_OBJECTS test${name}firmware_main test${name}firmware)
endforeach()
foreach(target IN LISTS FIRMWARE_OBJECTS)
    set_target_properties(${target} PROPERTIES
        C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO
        CXX_STANDARD 17 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
    target_compile_options(${target} PRIVATE /W4 /WX /utf-8)
    if(SDL_FIRMWARE_VERSION_TEST_ASAN)
        target_compile_options(${target} PRIVATE /fsanitize=address /Zi)
    endif()
endforeach()
foreach(target IN ITEMS testfirmwarefake testgipfirmware_main testxboxonefirmware_main testswitchfirmware_main testflydigifirmware_main testps5firmware_main
                        testxinputfirmware_main testxinputfirmware_gameinput)
    target_compile_definitions(${target} PRIVATE USING_GENERATED_CONFIG_H SDL_STATIC_LIB)
    target_include_directories(${target} PRIVATE
        "${FIRMWARE_SDL_ROOT}/src"
        "${CMAKE_CURRENT_BINARY_DIR}/sdl/include-config-$<LOWER_CASE:$<CONFIG>>/build_config"
        "${CMAKE_CURRENT_BINARY_DIR}/sdl/include-revision")
    target_link_libraries(${target} PRIVATE SDL3::Headers)
endforeach()
# SDL_gameinput.h includes the vendored GameInput header by its bare name
target_include_directories(testxinputfirmware_gameinput PRIVATE "${FIRMWARE_SDL_ROOT}/src/core/windows/gameinput")
# The drivers leave parameters of their entry points unused (C4100), and so
# do the scripted GameInput's methods. SDL's own /W3 build does not report
# it. Every other warning stays an error.
foreach(name IN LISTS FIRMWARE_TEST_NAMES)
    target_compile_options(test${name}firmware_main PRIVATE /wd4100)
endforeach()
target_compile_options(testxinputfirmware_main PRIVATE /wd4100)
target_compile_options(testxinputfirmware_gameinput PRIVATE /wd4100)

# The tests never start SDL's joystick subsystem. Every joystick driver is
# off all the same, so none could add a device were one started. The hints
# the five drivers read are cleared, and the GIP and PS5 tests set their own.
set(FIRMWARE_OFFLINE_ENVIRONMENT
    "SDL_JOYSTICK_HIDAPI=0"
    "SDL_JOYSTICK_DIRECTINPUT=0"
    "SDL_XINPUT_ENABLED=0"
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
set(FIRMWARE_CLEARED_ENVIRONMENT
    "SDL_JOYSTICK_SERIAL=unset:"
    "SDL_JOYSTICK_DJI_REMOTE_TCP_HOSTS=unset:"
    "SDL_JOYSTICK_HIDAPI_GIP=unset:"
    "SDL_JOYSTICK_HIDAPI_GIP_RESET_FOR_METADATA=unset:"
    "SDL_JOYSTICK_HIDAPI_XBOX=unset:"
    "SDL_JOYSTICK_HIDAPI_XBOX_ONE=unset:"
    "SDL_JOYSTICK_HIDAPI_XBOX_ONE_HOME_LED=unset:"
    "SDL_JOYSTICK_HIDAPI_SWITCH=unset:"
    "SDL_JOYSTICK_HIDAPI_SWITCH_HOME_LED=unset:"
    "SDL_JOYSTICK_HIDAPI_SWITCH_PLAYER_LED=unset:"
    "SDL_JOYSTICK_HIDAPI_JOYCON_RINGCON=unset:"
    "SDL_JOYSTICK_HIDAPI_FLYDIGI=unset:"
    "SDL_JOYSTICK_HIDAPI_PS5=unset:"
    "SDL_JOYSTICK_HIDAPI_PS5_PLAYER_LED=unset:"
    "SDL_JOYSTICK_ENHANCED_REPORTS=unset:")

foreach(name IN LISTS FIRMWARE_TEST_NAMES)
    add_test(NAME firmware-version.${name} COMMAND test${name}firmware)
    set_tests_properties(firmware-version.${name} PROPERTIES
        TIMEOUT 120
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
        ENVIRONMENT "${FIRMWARE_OFFLINE_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "${FIRMWARE_CLEARED_ENVIRONMENT}")
    set_property(TEST firmware-version.${name} PROPERTY LABELS firmware-version offline)
endforeach()

# The XInput test starts the XInput driver itself, against its fakes, so
# its hint is cleared rather than set to 0
set(XINPUT_FIRMWARE_ENVIRONMENT ${FIRMWARE_OFFLINE_ENVIRONMENT})
list(REMOVE_ITEM XINPUT_FIRMWARE_ENVIRONMENT "SDL_XINPUT_ENABLED=0")
set(XINPUT_FIRMWARE_CLEARED_ENVIRONMENT ${FIRMWARE_CLEARED_ENVIRONMENT} "SDL_XINPUT_ENABLED=unset:")
add_test(NAME firmware-version.xinput COMMAND testxinputfirmware)
set_tests_properties(firmware-version.xinput PROPERTIES
    TIMEOUT 120
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    ENVIRONMENT "${XINPUT_FIRMWARE_ENVIRONMENT}"
    ENVIRONMENT_MODIFICATION "${XINPUT_FIRMWARE_CLEARED_ENVIRONMENT}")
set_property(TEST firmware-version.xinput PROPERTY LABELS firmware-version offline)
