cmake_minimum_required(VERSION 3.22)
project(SDLSerialDriverTests LANGUAGES C CXX)

# An offline test of SDL_SERIAL_JoystickDriver, the serial joystick driver of
# hifihedgehog/SDL#33 Part 5, with the I-Force haptic driver of
# hifihedgehog/SDL#35. The real driver runs inside a real static SDL against
# scripted COM ports, so its port threads, the engine, the protocol modules,
# the joysticks and SDL's haptic API all run without a COM port, the
# configuration manager, the registry or a library load.
if(NOT WIN32 OR NOT CMAKE_C_COMPILER_ID STREQUAL "MSVC" OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "The driver test requires native MSVC for 64-bit Windows.")
endif()
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    message(FATAL_ERROR "Use a separate build directory for the offline tests.")
endif()

option(SDL_SERIAL_DRIVER_TEST_ASAN "Instrument the test and the static SDL with AddressSanitizer" OFF)
if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Offline test configuration" FORCE)
endif()
if(SDL_SERIAL_DRIVER_TEST_ASAN)
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

get_filename_component(SERIAL_SDL_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(SERIAL_TESTS "${SERIAL_SDL_ROOT}/test")

# The test always includes the tree's driver,
# src/joystick/windows/SDL_serialjoystick.c, by its path relative to
# testserialdriver.c. No cache entry can put another source in its place.

# This cache belongs only to the standalone test build, as in
# test/icade-driver. The real SDL archive supplies the joystick, haptic,
# event, hint and thread code the driver runs on, the serial engine and its
# modules, and the I-Force haptic driver.
set(SDL_SHARED OFF CACHE BOOL "Offline static SDL dependency" FORCE)
set(SDL_STATIC ON CACHE BOOL "Offline static SDL dependency" FORCE)
set(SDL_TESTS OFF CACHE BOOL "Only register this sidecar's tests" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "No SDL test library needed" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "No example applications" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "No installation from this test build" FORCE)
set(SDL_LIBC ON CACHE BOOL "Use the native C runtime" FORCE)
set(SDL_XINPUT_PADDLES OFF CACHE BOOL "No production paddle acquisition" FORCE)
set(HAVE_GAMEINPUT_LIB OFF CACHE BOOL "Keep GameInput an optional dynamic dependency" FORCE)
# SDL builds its HIDAPI haptic layer, which serves the serial joysticks, only
# with the HIDAPI joystick backend. The backend is built and switched off by
# SDL_JOYSTICK_HIDAPI below, so it lists no HID device. libusb stays out.
set(SDL_HIDAPI ON CACHE BOOL "Required by the HIDAPI haptic layer" FORCE)
set(SDL_HIDAPI_JOYSTICK ON CACHE BOOL "Required by the HIDAPI haptic layer" FORCE)
set(SDL_HIDAPI_LIBUSB OFF CACHE BOOL "No native USB acquisition" FORCE)
set(SDL_HAPTIC ON CACHE BOOL "The haptic API under test" FORCE)
add_subdirectory("${SERIAL_SDL_ROOT}" "${CMAKE_CURRENT_BINARY_DIR}/sdl" EXCLUDE_FROM_ALL)
if(SDL_SERIAL_DRIVER_TEST_ASAN)
    target_compile_options(SDL3-static PRIVATE /fsanitize=address /Zi)
endif()

# The engine and the modules come from the static SDL, whose Windows build
# takes src/joystick/serial, and the I-Force haptic driver from its HIDAPI
# haptic layer.
get_target_property(SERIAL_SDL_SOURCES SDL3-static SOURCES)
set(SERIAL_SDL_ENGINE)
set(SERIAL_SDL_HAPTIC)
foreach(source IN LISTS SERIAL_SDL_SOURCES)
    if(source MATCHES "/src/joystick/serial/SDL_serial_engine\\.c$")
        set(SERIAL_SDL_ENGINE "${source}")
    elseif(source MATCHES "/src/haptic/hidapi/SDL_hidapihaptic_iforce\\.c$")
        set(SERIAL_SDL_HAPTIC "${source}")
    endif()
endforeach()
if(NOT SERIAL_SDL_ENGINE)
    message(FATAL_ERROR "The static SDL does not build src/joystick/serial/SDL_serial_engine.c.")
endif()
if(NOT SERIAL_SDL_HAPTIC)
    message(FATAL_ERROR "The static SDL does not build src/haptic/hidapi/SDL_hidapihaptic_iforce.c.")
endif()

enable_testing()

# How the driver in the test reaches the fake port and not Windows:
# - testserialdriver.c defines every name through which the driver reaches a
#   COM port, the configuration manager, the registry or a library, then
#   includes the driver source.
# - The test's object defines SDL_SERIAL_JoystickDriver. The linker takes
#   every object named on its command line and searches a library only for a
#   symbol still undefined, so SDL_joystick.c's driver table, the HIDAPI
#   haptic layer and the dynamic API bind to the test's copy, and SDL's own
#   SDL_serialjoystick.c object is never pulled. Were it pulled for any other
#   symbol, its definition would collide with the test's and the link would
#   fail with LNK2005.
# - Before the link, CheckSystem.cmake reads the test object's symbol table
#   and fails the build when it still imports a Win32 function that reaches
#   a device, the registry or a library.
add_library(testserialdriver_main OBJECT "${SERIAL_TESTS}/testserialdriver.c")
target_compile_definitions(testserialdriver_main PRIVATE USING_GENERATED_CONFIG_H SDL_STATIC_LIB)
target_include_directories(testserialdriver_main PRIVATE
    "${SERIAL_SDL_ROOT}/src"
    "${CMAKE_CURRENT_BINARY_DIR}/sdl/include-config-$<LOWER_CASE:$<CONFIG>>/build_config"
    "${CMAKE_CURRENT_BINARY_DIR}/sdl/include-revision")
target_link_libraries(testserialdriver_main PRIVATE SDL3::Headers)

add_executable(testserialdriver $<TARGET_OBJECTS:testserialdriver_main>)
target_link_libraries(testserialdriver PRIVATE SDL3-static)

foreach(target testserialdriver_main testserialdriver)
    set_target_properties(${target} PROPERTIES
        C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    target_compile_options(${target} PRIVATE /W4 /WX /utf-8)
    if(SDL_SERIAL_DRIVER_TEST_ASAN)
        target_compile_options(${target} PRIVATE /fsanitize=address /Zi)
    endif()
endforeach()
# The map shows which object supplies SDL_SERIAL_JoystickDriver
target_link_options(testserialdriver PRIVATE /INCREMENTAL:NO "/MAP:${CMAKE_CURRENT_BINARY_DIR}/testserialdriver.map")
if(SDL_SERIAL_DRIVER_TEST_ASAN)
    target_link_options(testserialdriver PRIVATE /DEBUG)
endif()

get_filename_component(SERIAL_LINKER_DIR "${CMAKE_LINKER}" DIRECTORY)
find_program(SERIAL_DUMPBIN dumpbin HINTS "${SERIAL_LINKER_DIR}" NO_CACHE REQUIRED)
add_custom_command(TARGET testserialdriver PRE_LINK
    COMMAND "${CMAKE_COMMAND}" "-DDUMPBIN=${SERIAL_DUMPBIN}"
        "-DOBJECT=$<TARGET_OBJECTS:testserialdriver_main>"
        "-DREPORT=${CMAKE_CURRENT_BINARY_DIR}/testserialdriver-externals.txt"
        -P "${CMAKE_CURRENT_LIST_DIR}/CheckSystem.cmake"
    VERBATIM)

# Every other joystick driver is off, so only the driver under test can add
# a joystick. The test also sets these hints at override priority before
# SDL_Init, with SDL_JOYSTICK_SERIAL naming its ports. The lists the driver
# and SDL's haptic API read are cleared, since the test sets them.
set(SERIAL_DRIVER_OFFLINE_ENVIRONMENT
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
set(SERIAL_DRIVER_CLEARED_ENVIRONMENT
    "SDL_JOYSTICK_SERIAL=unset:"
    "SDL_JOYSTICK_DJI_REMOTE_TCP_HOSTS=unset:"
    "SDL_JOYSTICK_KONAMI_ACIO=unset:"
    "SDL_JOYSTICK_KONAMI_BIO2_MODE=unset:"
    "SDL_JOYSTICK_BLACKLIST_DEVICES=unset:"
    "SDL_GAMECONTROLLER_IGNORE_DEVICES=unset:"
    "SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT=unset:"
    "SDL_GAMECONTROLLERCONFIG=unset:"
    "SDL_HAPTIC_GAIN_MAX=unset:")

add_test(NAME serial-driver.iforce COMMAND testserialdriver)
set_tests_properties(serial-driver.iforce PROPERTIES
    TIMEOUT 120
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    ENVIRONMENT "${SERIAL_DRIVER_OFFLINE_ENVIRONMENT}"
    ENVIRONMENT_MODIFICATION "${SERIAL_DRIVER_CLEARED_ENVIRONMENT}")
set_property(TEST serial-driver.iforce PROPERTY LABELS serial-joystick offline)
