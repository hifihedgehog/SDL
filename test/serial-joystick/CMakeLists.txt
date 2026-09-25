cmake_minimum_required(VERSION 3.22)
project(SDLSerialJoystickTests LANGUAGES C)

# Offline replay tests for the serial joystick driver: the pure engine and
# one pure module per protocol. They are C99 with no SDL runtime and no I/O,
# so these tests build without SDL and need no device or COM port.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    message(FATAL_ERROR "Use a separate build directory for the offline tests.")
endif()

option(SDL_SERIAL_TEST_ASAN "Instrument the offline test targets with AddressSanitizer" OFF)
if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Offline test configuration" FORCE)
endif()
if(SDL_SERIAL_TEST_ASAN AND MSVC)
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

get_filename_component(SERIAL_SDL_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(SERIAL_TESTS "${SERIAL_SDL_ROOT}/test")
set(SERIAL_SOURCE "${SERIAL_SDL_ROOT}/src/joystick/serial")
set(SERIAL_DJI "${SERIAL_SDL_ROOT}/src/joystick/dji")

# The DJI capture test reads captures from the reference clones at run time.
# They are not copied into this tree.
get_filename_component(SERIAL_DJI_DEFAULT_REFERENCES "${SERIAL_SDL_ROOT}/../.." ABSOLUTE)
set(DJI_REFERENCE_ROOT "${SERIAL_DJI_DEFAULT_REFERENCES}" CACHE PATH "Folder that holds the DJI reference clones")

enable_testing()

function(serial_configure target)
    set_target_properties(${target} PROPERTIES
        C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX /utf-8)
        target_link_options(${target} PRIVATE /INCREMENTAL:NO)
        if(SDL_SERIAL_TEST_ASAN)
            target_compile_options(${target} PRIVATE /fsanitize=address /Zi)
            target_link_options(${target} PRIVATE /DEBUG)
        endif()
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
        if(SDL_SERIAL_TEST_ASAN)
            target_compile_options(${target} PRIVATE -fsanitize=address -fno-omit-frame-pointer -g -O0)
            target_link_options(${target} PRIVATE -fsanitize=address)
        endif()
    endif()
endfunction()

function(serial_test name)
    set(target "testserial${name}")
    add_executable(${target} "${SERIAL_TESTS}/testserial${name}.c"
        "${SERIAL_SOURCE}/SDL_serial_engine.c"
        "${SERIAL_SOURCE}/SDL_serial_proto.c"
        ${ARGN})
    serial_configure(${target})
    add_test(NAME "serial-joystick.${name}" COMMAND ${target})
    set_tests_properties("serial-joystick.${name}" PROPERTIES
        TIMEOUT 120 LABELS "serial-joystick;offline"
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
endfunction()

# A DJI test: test/testdji<name>.c with the engine, the contract and the DJI
# modules. Arguments after the name reach the test. A test that returns 77
# is reported as skipped.
function(dji_test name)
    set(target "testdji${name}")
    add_executable(${target} "${SERIAL_TESTS}/testdji${name}.c"
        "${SERIAL_SOURCE}/SDL_serial_engine.c"
        "${SERIAL_SOURCE}/SDL_serial_proto.c"
        "${SERIAL_DJI}/SDL_dji_duml.c"
        "${SERIAL_DJI}/SDL_dji_remote_proto.c"
        "${SERIAL_DJI}/SDL_dji_tcp_proto.c")
    serial_configure(${target})
    add_test(NAME "serial-joystick.dji${name}" COMMAND ${target} ${ARGN})
    set_tests_properties("serial-joystick.dji${name}" PROPERTIES
        TIMEOUT 120 LABELS "serial-joystick;offline" SKIP_RETURN_CODE 77
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
endfunction()

# An RFCOMM test: test/testrfcomm<name>.c with the serial contract, the
# RFCOMM link and the family modules of hifihedgehog/SDL#33 Part 11.
set(SERIAL_RFCOMM "${SERIAL_SDL_ROOT}/src/joystick/windows")
function(rfcomm_test name)
    set(target "testrfcomm${name}")
    add_executable(${target} "${SERIAL_TESTS}/testrfcomm${name}.c"
        "${SERIAL_SOURCE}/SDL_serial_engine.c"
        "${SERIAL_SOURCE}/SDL_serial_proto.c"
        "${SERIAL_RFCOMM}/SDL_rfcomm_proto.c"
        "${SERIAL_RFCOMM}/SDL_rfcomm_moga_proto.c"
        "${SERIAL_RFCOMM}/SDL_rfcomm_zeemote_proto.c"
        "${SERIAL_RFCOMM}/SDL_rfcomm_bgp100_proto.c")
    serial_configure(${target})
    add_test(NAME "serial-joystick.rfcomm${name}" COMMAND ${target})
    set_tests_properties("serial-joystick.rfcomm${name}" PROPERTIES
        TIMEOUT 120 LABELS "serial-joystick;offline"
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
endfunction()

# hifihedgehog/SDL#33 Part 5: the serial transport and its devices.
serial_test(engine)

serial_test(spaceball "${SERIAL_SOURCE}/SDL_serial_spaceball_proto.c")

serial_test(spaceorb "${SERIAL_SOURCE}/SDL_serial_spaceorb_proto.c")

serial_test(magellan "${SERIAL_SOURCE}/SDL_serial_magellan_proto.c")

serial_test(stinger "${SERIAL_SOURCE}/SDL_serial_stinger_proto.c")

serial_test(warrior "${SERIAL_SOURCE}/SDL_serial_warrior_proto.c")

serial_test(cyberman "${SERIAL_SOURCE}/SDL_serial_cyberman_proto.c")

serial_test(zhenhua "${SERIAL_SOURCE}/SDL_serial_zhenhua_proto.c")

serial_test(ibus "${SERIAL_SOURCE}/SDL_serial_ibus_proto.c")

serial_test(jvs "${SERIAL_SOURCE}/SDL_serial_jvs_proto.c")

serial_test(vrinsight "${SERIAL_SOURCE}/SDL_serial_vrinsight_proto.c")

serial_test(kettler "${SERIAL_SOURCE}/SDL_serial_kettler_proto.c")

# hifihedgehog/SDL#33 Part 8: I-Force wheels and joysticks on RS-232.
serial_test(iforce "${SERIAL_SOURCE}/SDL_serial_iforce_proto.c"
    "${SERIAL_SDL_ROOT}/src/joystick/SDL_iforce_proto.c")

# hifihedgehog/SDL#33 Part 10: Pony Canyon's Master Controllers.
serial_test(mastercontroller "${SERIAL_SOURCE}/SDL_serial_mastercontroller_proto.c"
    "${SERIAL_SDL_ROOT}/src/joystick/SDL_train_proto.c")

# hifihedgehog/SDL#33 Part 11: Bluetooth RFCOMM controllers, the MOGA, the
# Zeemote JS1, the Chainpus BGP100 and the Phonejoy.
rfcomm_test(link)

rfcomm_test(moga)

rfcomm_test(zeemote)

rfcomm_test(bgp100)

# hifihedgehog/SDL#33 Part 6: DJI remotes on the serial port, the USB bulk
# interface and TCP.
dji_test(duml)

dji_test(remote)

dji_test(tcp)

dji_test(capture
    "${DJI_REFERENCE_ROOT}/DJI_RC_Motion_Bridge-v1kr4m/brute_log.txt"
    "${DJI_REFERENCE_ROOT}/dji-rc-joystick-gregszero/capture.bin"
    "${DJI_REFERENCE_ROOT}/dji-rc-joystick-gregszero/capture_f5.bin")
