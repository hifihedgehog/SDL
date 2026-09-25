cmake_minimum_required(VERSION 3.22)
project(SDLControllerProtocolTests LANGUAGES C)

# Offline replay tests for the pure protocol modules of the controllers that
# Windows leaves unusable. Each module is C99 with no SDL runtime and no I/O,
# so these tests build without SDL and need no device.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    message(FATAL_ERROR "Use a separate build directory for the offline tests.")
endif()

option(SDL_PROTOCOL_TEST_ASAN "Instrument the offline test targets with AddressSanitizer" OFF)
if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Offline test configuration" FORCE)
endif()
if(SDL_PROTOCOL_TEST_ASAN AND MSVC)
    # Debug's default /RTC1 conflicts with MSVC AddressSanitizer.
    if(CMAKE_CONFIGURATION_TYPES)
        set(CMAKE_CONFIGURATION_TYPES "Release;RelWithDebInfo" CACHE STRING "ASan configurations" FORCE)
    elseif(NOT CMAKE_BUILD_TYPE MATCHES "^(Release|RelWithDebInfo)$")
        message(FATAL_ERROR "ASan requires Release or RelWithDebInfo.")
    endif()
endif()

get_filename_component(PROTOCOL_SDL_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(PROTOCOL_TESTS "${PROTOCOL_SDL_ROOT}/test")
set(PROTOCOL_HIDAPI "${PROTOCOL_SDL_ROOT}/src/hidapi")
set(PROTOCOL_JOYSTICK "${PROTOCOL_SDL_ROOT}/src/joystick/hidapi")
set(PROTOCOL_JOYSTICK_SHARED "${PROTOCOL_SDL_ROOT}/src/joystick")

enable_testing()

function(protocol_test part)
    set(target "testprotocol${part}")
    add_executable(${target} ${ARGN})
    set_target_properties(${target} PROPERTIES
        C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX /utf-8)
        target_link_options(${target} PRIVATE /INCREMENTAL:NO)
        if(SDL_PROTOCOL_TEST_ASAN)
            target_compile_options(${target} PRIVATE /fsanitize=address /Zi)
            target_link_options(${target} PRIVATE /DEBUG)
        endif()
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
        if(SDL_PROTOCOL_TEST_ASAN)
            target_compile_options(${target} PRIVATE -fsanitize=address -fno-omit-frame-pointer -g)
            target_link_options(${target} PRIVATE -fsanitize=address)
        endif()
    endif()
    add_test(NAME "controller-protocols.${part}" COMMAND ${target})
    set_tests_properties("controller-protocols.${part}" PROPERTIES
        TIMEOUT 60 LABELS "controller-protocols;offline"
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
endfunction()

# hifihedgehog/SDL#33 Part 1: the shared vendor-USB path, the Intel Wireless
# Series base station, and the Xbox 360 Big Button receiver.
protocol_test(vendorusb "${PROTOCOL_TESTS}/testvendorusb.c"
    "${PROTOCOL_HIDAPI}/SDL_hidapi_vendorusb.c")

protocol_test(intelwireless "${PROTOCOL_TESTS}/testintelwireless.c"
    "${PROTOCOL_JOYSTICK}/SDL_hidapi_intelwireless_proto.c"
    "${PROTOCOL_HIDAPI}/SDL_hidapi_vendorusb.c")

protocol_test(xbox360bb "${PROTOCOL_TESTS}/testxbox360bb.c"
    "${PROTOCOL_JOYSTICK}/SDL_hidapi_xbox360bb_proto.c"
    "${PROTOCOL_HIDAPI}/SDL_hidapi_vendorusb.c")

# hifihedgehog/SDL#33 Part 2: Wii Remote extensions.
protocol_test(wiiext "${PROTOCOL_TESTS}/testwiiext.c"
    "${PROTOCOL_JOYSTICK}/SDL_hidapi_wii_ext_proto.c")

# hifihedgehog/SDL#33 Part 3: Guitar Hero Live, Rock Band 3 Pro, uDraw,
# Top Shot and Tony Hawk.
protocol_test(ghl "${PROTOCOL_TESTS}/testghl.c"
    "${PROTOCOL_JOYSTICK_SHARED}/SDL_ghl_proto.c")

protocol_test(rb3pro "${PROTOCOL_TESTS}/testrb3pro.c"
    "${PROTOCOL_JOYSTICK_SHARED}/SDL_rb3pro_proto.c")

protocol_test(ps3ext "${PROTOCOL_TESTS}/testps3ext.c"
    "${PROTOCOL_JOYSTICK}/SDL_hidapi_ps3ext_proto.c")

# hifihedgehog/SDL#33 Part 13: the Ring-Con on a right Joy-Con.
protocol_test(ringcon "${PROTOCOL_TESTS}/testringcon.c"
    "${PROTOCOL_JOYSTICK}/SDL_hidapi_switch_ringcon_proto.c")
