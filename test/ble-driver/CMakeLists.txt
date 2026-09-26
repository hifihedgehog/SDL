cmake_minimum_required(VERSION 3.22)
project(SDLBLEDriverTests LANGUAGES C CXX)

# An offline test of SDL_BLEGATT_JoystickDriver, the Bluetooth LE GATT
# joystick driver of hifihedgehog/SDL#33 Part 12. The real driver runs inside
# a real static SDL against a fake transport, so its Init, Detect, Open,
# Update, Close and Quit, the joystick and gamepad APIs, the GUID, the
# gamepad mapping, touchpads, sensors and battery all run without a radio.
if(NOT WIN32 OR NOT CMAKE_C_COMPILER_ID STREQUAL "MSVC" OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "The driver test requires native MSVC for 64-bit Windows.")
endif()
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    message(FATAL_ERROR "Use a separate build directory for the offline tests.")
endif()

option(SDL_BLE_DRIVER_TEST_ASAN "Instrument the test and the static SDL with AddressSanitizer" OFF)
if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Offline test configuration" FORCE)
endif()
if(SDL_BLE_DRIVER_TEST_ASAN)
    # Debug's default /RTC1 conflicts with MSVC AddressSanitizer.
    if(CMAKE_CONFIGURATION_TYPES)
        set(CMAKE_CONFIGURATION_TYPES "Release;RelWithDebInfo" CACHE STRING "ASan configurations" FORCE)
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

get_filename_component(BLE_SDL_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(BLE_TESTS "${BLE_SDL_ROOT}/test")

# The test always includes the tree's driver,
# src/joystick/windows/SDL_blegattjoystick.c, by its path relative to
# testblegattdriver.c. No cache entry can put another source in its place.

# The Gear VR scenario reads its fixture from the reference clone at run
# time. It is not copied into this tree.
get_filename_component(BLE_DEFAULT_REFERENCES "${BLE_SDL_ROOT}/../.." ABSOLUTE)
set(BLE_REFERENCE_ROOT "${BLE_DEFAULT_REFERENCES}" CACHE PATH "Folder that holds the reference clones")

# This cache belongs only to the standalone test build, as in
# test/xinput-paddles. The real SDL archive supplies the joystick, gamepad,
# event, hint, thread and sensor code the driver runs on.
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
add_subdirectory("${BLE_SDL_ROOT}" "${CMAKE_CURRENT_BINARY_DIR}/sdl" EXCLUDE_FROM_ALL)
if(SDL_BLE_DRIVER_TEST_ASAN)
    target_compile_options(SDL3-static PRIVATE /fsanitize=address /Zi)
endif()

# The pure session and device modules come from the static SDL, whose
# Windows build takes src/joystick/ble.
get_target_property(BLE_SDL_SOURCES SDL3-static SOURCES)
set(BLE_SDL_MODULES)
foreach(source IN LISTS BLE_SDL_SOURCES)
    if(source MATCHES "/src/joystick/ble/[^/]+\\.c$")
        list(APPEND BLE_SDL_MODULES "${source}")
    endif()
endforeach()
if(NOT BLE_SDL_MODULES)
    message(FATAL_ERROR "The static SDL does not build src/joystick/ble.")
endif()

enable_testing()

# How the driver in the test reaches the fake transport and not SDL's:
# - testblegattdriver.c renames every transport function with a macro, then
#   includes the driver source. The driver in the test calls Fake_BLEGATT_*,
#   while SDL_ble_gatt.c in the static SDL keeps SDL_BLEGATT_* for the
#   Switch 2 driver. The names differ, so nothing clashes.
# - The test's object defines SDL_BLEGATT_JoystickDriver. The linker takes
#   every object named on its command line and searches a library only for a
#   symbol still undefined, so SDL_joystick.c's driver table binds to the
#   test's copy and SDL's own SDL_blegattjoystick.c object is never pulled.
#   Were it pulled for any other symbol, its SDL_BLEGATT_JoystickDriver would
#   collide with the test's and the link would fail with LNK2005. A linked
#   test therefore runs only the test's copy of the driver.
# - Before the link, CheckTransport.cmake reads the test object's symbol
#   table and fails the build when it still calls an SDL_BLEGATT_ function, a
#   Windows Bluetooth or WinRT entry point, or a way to resolve one at run
#   time, such as a transport function added to SDL_ble_gatt.h after the
#   renames were written.
add_library(testblegattdriver_main OBJECT "${BLE_TESTS}/testblegattdriver.c")
target_compile_definitions(testblegattdriver_main PRIVATE USING_GENERATED_CONFIG_H SDL_STATIC_LIB)
target_include_directories(testblegattdriver_main PRIVATE
    "${BLE_SDL_ROOT}/src"
    "${CMAKE_CURRENT_BINARY_DIR}/sdl/include-config-$<LOWER_CASE:$<CONFIG>>/build_config"
    "${CMAKE_CURRENT_BINARY_DIR}/sdl/include-revision")
target_link_libraries(testblegattdriver_main PRIVATE SDL3::Headers)

add_executable(testblegattdriver $<TARGET_OBJECTS:testblegattdriver_main>)
target_link_libraries(testblegattdriver PRIVATE SDL3-static)

foreach(target testblegattdriver_main testblegattdriver)
    set_target_properties(${target} PROPERTIES
        C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    target_compile_options(${target} PRIVATE /W4 /WX /utf-8)
    if(SDL_BLE_DRIVER_TEST_ASAN)
        target_compile_options(${target} PRIVATE /fsanitize=address /Zi)
    endif()
endforeach()
# The map shows which object supplies SDL_BLEGATT_JoystickDriver
target_link_options(testblegattdriver PRIVATE /INCREMENTAL:NO "/MAP:${CMAKE_CURRENT_BINARY_DIR}/testblegattdriver.map")
if(SDL_BLE_DRIVER_TEST_ASAN)
    target_link_options(testblegattdriver PRIVATE /DEBUG)
endif()

get_filename_component(BLE_LINKER_DIR "${CMAKE_LINKER}" DIRECTORY)
find_program(BLE_DUMPBIN dumpbin HINTS "${BLE_LINKER_DIR}" NO_CACHE REQUIRED)
add_custom_command(TARGET testblegattdriver PRE_LINK
    COMMAND "${CMAKE_COMMAND}" "-DDUMPBIN=${BLE_DUMPBIN}"
        "-DOBJECT=$<TARGET_OBJECTS:testblegattdriver_main>"
        "-DREPORT=${CMAKE_CURRENT_BINARY_DIR}/testblegattdriver-externals.txt"
        -P "${CMAKE_CURRENT_LIST_DIR}/CheckTransport.cmake"
    VERBATIM)

# Every other joystick driver is off, so only the driver under test can add
# a joystick. The test also sets these hints at override priority before
# each SDL_Init. The driver's own hints are cleared, since the test sets them.
set(BLE_DRIVER_OFFLINE_ENVIRONMENT
    "SDL_JOYSTICK_HIDAPI=0"
    "SDL_JOYSTICK_RAWINPUT=0"
    "SDL_JOYSTICK_DIRECTINPUT=0"
    "SDL_XINPUT_ENABLED=0"
    "SDL_JOYSTICK_WGI=0"
    "SDL_JOYSTICK_GAMEINPUT=0"
    "SDL_JOYSTICK_GAMEINPUT_RAW=0"
    "SDL_JOYSTICK_BLE_SWITCH2=0"
    "SDL_JOYSTICK_SERIAL_AUTO=0"
    "SDL_JOYSTICK_RFCOMM=0"
    "SDL_JOYSTICK_ICADE=0"
    "SDL_JOYSTICK_WINMM=0"
    "SDL_JOYSTICK_THREAD=0"
    "SDL_JOYSTICK_ROG_CHAKRAM=0")
set(BLE_DRIVER_CLEARED_ENVIRONMENT
    "SDL_JOYSTICK_SERIAL=unset:"
    "SDL_JOYSTICK_DJI_REMOTE_TCP_HOSTS=unset:"
    "SDL_JOYSTICK_BLE=unset:"
    "SDL_JOYSTICK_BLE_POKEBALL=unset:"
    "SDL_JOYSTICK_BLE_DAYDREAM=unset:"
    "SDL_JOYSTICK_BLE_GEARVR=unset:"
    "SDL_JOYSTICK_BLE_OCULUSGO=unset:"
    "SDL_JOYSTICK_BLE_GHLIVE=unset:"
    "SDL_JOYSTICK_BLE_ZWIFT=unset:"
    "SDL_JOYSTICK_BLE_MYO=unset:"
    "SDL_JOYSTICK_BLE_PAIRING=unset:")

# Named as test/ble names the pure tests, so ctest -R ble-joystick and
# ctest -L ble-joystick both select it
add_test(NAME ble-joystick.driver COMMAND testblegattdriver "${BLE_REFERENCE_ROOT}/gearvr-controller")
set_tests_properties(ble-joystick.driver PROPERTIES
    TIMEOUT 300 LABELS "ble-joystick;offline"
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    ENVIRONMENT "${BLE_DRIVER_OFFLINE_ENVIRONMENT}"
    ENVIRONMENT_MODIFICATION "${BLE_DRIVER_CLEARED_ENVIRONMENT}")
