cmake_minimum_required(VERSION 3.22)
project(SDLBLEJoystickTests LANGUAGES C)

# Offline replay tests for the Bluetooth LE GATT joystick driver of
# hifihedgehog/SDL#33 Part 12: the session, the device table and one pure
# module per device. They are C99 with no SDL runtime and no I/O, so these
# tests build without SDL and need no Bluetooth radio. The Zwift key
# exchange test calls Windows' CNG.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    message(FATAL_ERROR "Use a separate build directory for the offline tests.")
endif()

option(SDL_BLE_TEST_ASAN "Instrument the offline test targets with AddressSanitizer" OFF)
if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Offline test configuration" FORCE)
endif()
if(SDL_BLE_TEST_ASAN AND MSVC)
    # Debug's default /RTC1 conflicts with MSVC AddressSanitizer.
    if(CMAKE_CONFIGURATION_TYPES)
        set(CMAKE_CONFIGURATION_TYPES "Release;RelWithDebInfo" CACHE STRING "ASan configurations" FORCE)
    elseif(NOT CMAKE_BUILD_TYPE MATCHES "^(Release|RelWithDebInfo)$")
        message(FATAL_ERROR "ASan requires Release or RelWithDebInfo.")
    endif()
    # Without optimization every load stays where the source puts it, so a
    # read before a length check reaches the sanitizer.
    foreach(config RELEASE RELWITHDEBINFO)
        string(REPLACE "/O2" "/Od" CMAKE_C_FLAGS_${config} "${CMAKE_C_FLAGS_${config}}")
    endforeach()
endif()

get_filename_component(BLE_SDL_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(BLE_TESTS "${BLE_SDL_ROOT}/test")
set(BLE_SOURCE "${BLE_SDL_ROOT}/src/joystick/ble")

# The capture tests read captures from the reference clones at run time.
# They are not copied into this tree.
get_filename_component(BLE_DEFAULT_REFERENCES "${BLE_SDL_ROOT}/../.." ABSOLUTE)
set(BLE_REFERENCE_ROOT "${BLE_DEFAULT_REFERENCES}" CACHE PATH "Folder that holds the reference clones")

set(BLE_PURE_SOURCES
    "${BLE_SOURCE}/SDL_ble_proto.c"
    "${BLE_SOURCE}/SDL_ble_session.c"
    "${BLE_SOURCE}/SDL_ble_devices.c"
    "${BLE_SOURCE}/SDL_ble_pokeball_proto.c"
    "${BLE_SOURCE}/SDL_ble_daydream_proto.c"
    "${BLE_SOURCE}/SDL_ble_gearvr_proto.c"
    "${BLE_SOURCE}/SDL_ble_oculusgo_proto.c"
    "${BLE_SOURCE}/SDL_ble_ghlios_proto.c"
    "${BLE_SOURCE}/SDL_ble_zwift_proto.c"
    "${BLE_SOURCE}/SDL_ble_myo_proto.c")

enable_testing()

function(ble_configure target)
    set_target_properties(${target} PROPERTIES
        C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    target_include_directories(${target} PRIVATE "${BLE_SOURCE}" "${BLE_TESTS}")
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX /utf-8)
        target_link_options(${target} PRIVATE /INCREMENTAL:NO)
        if(SDL_BLE_TEST_ASAN)
            target_compile_options(${target} PRIVATE /fsanitize=address /Zi)
            target_link_options(${target} PRIVATE /DEBUG)
        endif()
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
        target_link_libraries(${target} PRIVATE m)
        if(SDL_BLE_TEST_ASAN)
            target_compile_options(${target} PRIVATE -fsanitize=address -fno-omit-frame-pointer -g -O0)
            target_link_options(${target} PRIVATE -fsanitize=address)
        endif()
    endif()
endfunction()

# A test: test/testble<name>.c with every pure source. Arguments after the
# name reach the test.
function(ble_test name)
    set(target "testble${name}")
    add_executable(${target} "${BLE_TESTS}/testble${name}.c" ${BLE_PURE_SOURCES} ${BLE_EXTRA_${name}})
    ble_configure(${target})
    add_test(NAME "ble-joystick.${name}" COMMAND ${target} ${ARGN})
    set_tests_properties("ble-joystick.${name}" PROPERTIES
        TIMEOUT 120 LABELS "ble-joystick;offline"
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
endfunction()

# The session: matcher, host, value queue and the connection state machine.
ble_test(session)

# Nintendo's Poke Ball Plus, the Google Daydream controller, and the Samsung
# Gear VR and Oculus Go controllers.
ble_test(pokeball)
ble_test(daydream)
# The Gear VR test replays the captures of the gearvr-controller clone, which
# must be at BLE_REFERENCE_ROOT/gearvr-controller. BLE_REFERENCE_ROOT defaults
# to the folder two levels above the SDL tree, the folder that holds
# SDL3-build. The test fails when the clone is missing.
ble_test(gearvr "${BLE_REFERENCE_ROOT}/gearvr-controller")
ble_test(oculusgo)

# The iOS Guitar Hero Live guitar, the Thalmic Myo, and the Zwift Play and
# Zwift Click with the key exchange of older Zwift firmware.
ble_test(ghlios)
ble_test(myo)
ble_test(zwift)
set(BLE_EXTRA_zwiftcrypto "${BLE_SOURCE}/SDL_ble_zwift_crypto.c")
if(WIN32)
    ble_test(zwiftcrypto)
endif()
