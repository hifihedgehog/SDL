cmake_minimum_required(VERSION 3.22)
project(SDLGameInputDriverTests LANGUAGES C CXX)

# Offline test of SDL's GameInput joystick driver for hifihedgehog/SDL#45: the
# real driver inside a real static SDL, against a scripted GameInput whose
# registration calls wait for a running callback, as GameInput 4.0.302 did on
# the bench. SDL's joystick subsystem never starts, and no device opens.
if(NOT WIN32 OR NOT CMAKE_C_COMPILER_ID STREQUAL "MSVC" OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "The GameInput driver test requires native MSVC for 64-bit Windows.")
endif()
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    message(FATAL_ERROR "Use a separate build directory for the offline tests.")
endif()

option(SDL_GAMEINPUT_DRIVER_TEST_ASAN "Instrument the test and the static SDL with AddressSanitizer" OFF)
if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Offline test configuration" FORCE)
endif()
if(SDL_GAMEINPUT_DRIVER_TEST_ASAN)
    # Debug's default /RTC1 conflicts with MSVC AddressSanitizer.
    if(CMAKE_CONFIGURATION_TYPES)
        set(CMAKE_CONFIGURATION_TYPES Release RelWithDebInfo CACHE STRING "ASan configurations" FORCE)
    elseif(NOT CMAKE_BUILD_TYPE MATCHES "^(Release|RelWithDebInfo)$")
        message(FATAL_ERROR "ASan requires Release or RelWithDebInfo.")
    endif()
    foreach(config RELEASE RELWITHDEBINFO)
        string(REPLACE "/O2" "/Od" CMAKE_C_FLAGS_${config} "${CMAKE_C_FLAGS_${config}}")
        string(REPLACE "/O2" "/Od" CMAKE_CXX_FLAGS_${config} "${CMAKE_CXX_FLAGS_${config}}")
    endforeach()
endif()

get_filename_component(GAMEINPUT_SDL_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(GAMEINPUT_TESTS "${GAMEINPUT_SDL_ROOT}/test")

# The cache of this standalone build only, as in test/firmware-version. The
# HIDAPI joystick backend is built, since the driver asks it for device
# names, and is never started.
set(SDL_SHARED OFF CACHE BOOL "Offline static SDL dependency" FORCE)
set(SDL_STATIC ON CACHE BOOL "Offline static SDL dependency" FORCE)
set(SDL_TESTS OFF CACHE BOOL "Only register this sidecar's test" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "No SDL test library needed" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "No example applications" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "No installation from this test build" FORCE)
set(SDL_LIBC ON CACHE BOOL "Use the native C runtime" FORCE)
set(SDL_XINPUT_PADDLES OFF CACHE BOOL "No production paddle acquisition" FORCE)
set(HAVE_GAMEINPUT_LIB OFF CACHE BOOL "Keep GameInput an optional dynamic dependency" FORCE)
set(SDL_HIDAPI ON CACHE BOOL "Required by the HIDAPI joystick backend" FORCE)
set(SDL_HIDAPI_JOYSTICK ON CACHE BOOL "The backend the driver asks for names" FORCE)
set(SDL_HIDAPI_LIBUSB OFF CACHE BOOL "No native USB acquisition" FORCE)
add_subdirectory("${GAMEINPUT_SDL_ROOT}" "${CMAKE_CURRENT_BINARY_DIR}/sdl" EXCLUDE_FROM_ALL)
if(SDL_GAMEINPUT_DRIVER_TEST_ASAN)
    target_compile_options(SDL3-static PRIVATE /fsanitize=address /Zi)
endif()

enable_testing()

get_filename_component(GAMEINPUT_LINKER_DIR "${CMAKE_LINKER}" DIRECTORY)
find_program(GAMEINPUT_DUMPBIN dumpbin HINTS "${GAMEINPUT_LINKER_DIR}" NO_CACHE REQUIRED)

# testgameinputdriver.cpp includes the driver after naming its own fakes for
# the GameInput start and quit and for the functions that change SDL's
# joystick list or ask the other drivers. Its object defines the driver's
# table, so SDL's own object for the driver is never pulled into the link.
# Before the link, the firmware tests' CheckSystem.cmake fails the build when
# the object still calls a real one, or anything that reaches a device.
set(GAMEINPUT_FORBIDDEN "${CMAKE_CURRENT_BINARY_DIR}/gameinput-driver-forbidden.txt")
file(WRITE "${GAMEINPUT_FORBIDDEN}"
    "SDL_InitGameInput|SDL_QuitGameInput|GameInputInitialize|GameInputCreate|SDL_PrivateJoystickAdded|SDL_PrivateJoystickRemoved|SDL_JoystickHandledByAnotherDriver\n")
add_library(testgameinputdriver_main OBJECT "${GAMEINPUT_TESTS}/testgameinputdriver.cpp")
add_executable(testgameinputdriver $<TARGET_OBJECTS:testgameinputdriver_main>)
target_link_libraries(testgameinputdriver PRIVATE SDL3-static)
target_link_options(testgameinputdriver PRIVATE /INCREMENTAL:NO "/MAP:${CMAKE_CURRENT_BINARY_DIR}/testgameinputdriver.map")
if(SDL_GAMEINPUT_DRIVER_TEST_ASAN)
    target_link_options(testgameinputdriver PRIVATE /DEBUG)
endif()
add_custom_command(TARGET testgameinputdriver PRE_LINK
    COMMAND "${CMAKE_COMMAND}" "-DDUMPBIN=${GAMEINPUT_DUMPBIN}"
        "-DOBJECT=$<TARGET_OBJECTS:testgameinputdriver_main>"
        "-DREPORT=${CMAKE_CURRENT_BINARY_DIR}/testgameinputdriver-externals.txt"
        "-DEXTRA_FILE=${GAMEINPUT_FORBIDDEN}"
        -P "${GAMEINPUT_TESTS}/firmware-version/CheckSystem.cmake"
    VERBATIM)

foreach(target IN ITEMS testgameinputdriver_main testgameinputdriver)
    set_target_properties(${target} PROPERTIES
        CXX_STANDARD 17 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
    target_compile_options(${target} PRIVATE /W4 /WX /utf-8)
    if(SDL_GAMEINPUT_DRIVER_TEST_ASAN)
        target_compile_options(${target} PRIVATE /fsanitize=address /Zi)
    endif()
endforeach()
target_compile_definitions(testgameinputdriver_main PRIVATE USING_GENERATED_CONFIG_H SDL_STATIC_LIB)
target_include_directories(testgameinputdriver_main PRIVATE
    "${GAMEINPUT_SDL_ROOT}/src"
    "${GAMEINPUT_SDL_ROOT}/src/core/windows/gameinput"
    "${CMAKE_CURRENT_BINARY_DIR}/sdl/include-config-$<LOWER_CASE:$<CONFIG>>/build_config"
    "${CMAKE_CURRENT_BINARY_DIR}/sdl/include-revision")
target_link_libraries(testgameinputdriver_main PRIVATE SDL3::Headers)
# The driver leaves parameters of its entry points unused (C4100), and so do
# the scripted GameInput's methods. SDL's own /W3 build does not report it.
target_compile_options(testgameinputdriver_main PRIVATE /wd4100)

# SDL's joystick subsystem never starts here. Every joystick driver is off all
# the same, so none could add a device were one started. GameInput's raw
# reports are on and its gamepads off, as in PadForge's build, and the test
# sets the two hints itself.
set(GAMEINPUT_OFFLINE_ENVIRONMENT
    "SDL_JOYSTICK_HIDAPI=0"
    "SDL_JOYSTICK_DIRECTINPUT=0"
    "SDL_XINPUT_ENABLED=0"
    "SDL_JOYSTICK_BLE=0"
    "SDL_JOYSTICK_BLE_SWITCH2=0"
    "SDL_JOYSTICK_SERIAL_AUTO=0"
    "SDL_JOYSTICK_RFCOMM=0"
    "SDL_JOYSTICK_ICADE=0"
    "SDL_JOYSTICK_WINMM=0"
    "SDL_JOYSTICK_THREAD=0"
    "SDL_JOYSTICK_ROG_CHAKRAM=0")
# The serial port and DJI remote hints are lists, so they are unset
set(GAMEINPUT_CLEARED_ENVIRONMENT
    "SDL_JOYSTICK_SERIAL=unset:"
    "SDL_JOYSTICK_DJI_REMOTE_TCP_HOSTS=unset:"
    "SDL_JOYSTICK_GAMEINPUT=unset:"
    "SDL_JOYSTICK_GAMEINPUT_RAW=unset:"
    "SDL_JOYSTICK_IGNORE_DEVICES=unset:"
    "SDL_JOYSTICK_IGNORE_DEVICES_EXCEPT=unset:"
    "SDL_GAMECONTROLLER_IGNORE_DEVICES=unset:"
    "SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT=unset:")
# gameinput-driver.hotplug runs only the scenarios the driver before #45 also
# completes, so the same checks can run against it
add_test(NAME gameinput-driver COMMAND testgameinputdriver)
add_test(NAME gameinput-driver.hotplug COMMAND testgameinputdriver --hotplug)
foreach(test IN ITEMS gameinput-driver gameinput-driver.hotplug)
    set_tests_properties(${test} PROPERTIES
        TIMEOUT 180
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
        ENVIRONMENT "${GAMEINPUT_OFFLINE_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "${GAMEINPUT_CLEARED_ENVIRONMENT}")
    set_property(TEST ${test} PROPERTY LABELS gameinput-driver offline)
endforeach()
