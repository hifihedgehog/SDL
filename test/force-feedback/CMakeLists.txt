cmake_minimum_required(VERSION 3.22)
project(SDLForceFeedbackTests LANGUAGES C)

# Offline tests of hifihedgehog/SDL#48 inside a real static SDL: the
# DirectInput haptic backend's effect creation and update against a scripted
# DirectInput device, and the wheel typing of the Logitech RS50's three IDs
# through virtual joysticks with every real backend off. No device opens.
if(NOT WIN32 OR NOT CMAKE_C_COMPILER_ID STREQUAL "MSVC" OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "The force feedback tests require native MSVC for 64-bit Windows.")
endif()
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    message(FATAL_ERROR "Use a separate build directory for the offline tests.")
endif()

option(SDL_FORCE_FEEDBACK_TEST_ASAN "Instrument the tests and the static SDL with AddressSanitizer" OFF)
if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Offline test configuration" FORCE)
endif()
if(SDL_FORCE_FEEDBACK_TEST_ASAN)
    # Debug's default /RTC1 conflicts with MSVC AddressSanitizer.
    if(CMAKE_CONFIGURATION_TYPES)
        set(CMAKE_CONFIGURATION_TYPES Release RelWithDebInfo CACHE STRING "ASan configurations" FORCE)
    elseif(NOT CMAKE_BUILD_TYPE MATCHES "^(Release|RelWithDebInfo)$")
        message(FATAL_ERROR "ASan requires Release or RelWithDebInfo.")
    endif()
    foreach(config RELEASE RELWITHDEBINFO)
        string(REPLACE "/O2" "/Od" CMAKE_C_FLAGS_${config} "${CMAKE_C_FLAGS_${config}}")
    endforeach()
endif()

get_filename_component(FFB_SDL_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(FFB_TESTS "${FFB_SDL_ROOT}/test")

# The cache of this standalone build only, as in test/ezfrd64
set(SDL_SHARED OFF CACHE BOOL "Offline static SDL dependency" FORCE)
set(SDL_STATIC ON CACHE BOOL "Offline static SDL dependency" FORCE)
set(SDL_TESTS OFF CACHE BOOL "Only register this sidecar's tests" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "No SDL test library needed" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "No example applications" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "No installation from this test build" FORCE)
set(SDL_LIBC ON CACHE BOOL "Use the native C runtime" FORCE)
set(SDL_XINPUT_PADDLES OFF CACHE BOOL "No production paddle acquisition" FORCE)
set(HAVE_GAMEINPUT_LIB OFF CACHE BOOL "Keep GameInput an optional dynamic dependency" FORCE)
set(SDL_HIDAPI_LIBUSB OFF CACHE BOOL "No native USB acquisition" FORCE)
add_subdirectory("${FFB_SDL_ROOT}" "${CMAKE_CURRENT_BINARY_DIR}/sdl" EXCLUDE_FROM_ALL)
if(SDL_FORCE_FEEDBACK_TEST_ASAN)
    target_compile_options(SDL3-static PRIVATE /fsanitize=address /Zi)
endif()

enable_testing()

add_executable(testdinputeffects "${FFB_TESTS}/testdinputeffects.c")
add_executable(testwheeltype "${FFB_TESTS}/testwheeltype.c")
foreach(target IN ITEMS testdinputeffects testwheeltype)
    target_link_libraries(${target} PRIVATE SDL3-static)
    target_compile_definitions(${target} PRIVATE USING_GENERATED_CONFIG_H SDL_STATIC_LIB)
    target_include_directories(${target} PRIVATE
        "${FFB_SDL_ROOT}/src"
        "${CMAKE_CURRENT_BINARY_DIR}/sdl/include-config-$<LOWER_CASE:$<CONFIG>>/build_config"
        "${CMAKE_CURRENT_BINARY_DIR}/sdl/include-revision")
    target_link_libraries(${target} PRIVATE SDL3::Headers)
    target_compile_options(${target} PRIVATE /W4 /WX /utf-8)
    target_link_options(${target} PRIVATE /INCREMENTAL:NO)
    if(SDL_FORCE_FEEDBACK_TEST_ASAN)
        target_compile_options(${target} PRIVATE /fsanitize=address /Zi)
        target_link_options(${target} PRIVATE /DEBUG)
    endif()
endforeach()
# SDL_dinputhaptic.c, which testdinputeffects.c includes, leaves some of its
# functions' parameters unused (C4100) and holds an unused helper (C4505).
# SDL's own /W3 build does not report them.
target_compile_options(testdinputeffects PRIVATE /wd4100 /wd4505)

# testwheeltype.c turns every real joystick backend off itself. The wheel and
# list hints come from the test alone.
set(FFB_CLEARED_ENVIRONMENT
    "SDL_JOYSTICK_SERIAL=unset:"
    "SDL_JOYSTICK_DJI_REMOTE_TCP_HOSTS=unset:"
    "SDL_JOYSTICK_WHEEL_DEVICES=unset:"
    "SDL_JOYSTICK_WHEEL_DEVICES_EXCLUDED=unset:"
    "SDL_JOYSTICK_IGNORE_DEVICES=unset:"
    "SDL_JOYSTICK_IGNORE_DEVICES_EXCEPT=unset:")
add_test(NAME force-feedback.dinput-effects COMMAND testdinputeffects)
add_test(NAME force-feedback.wheel-type COMMAND testwheeltype)
foreach(test IN ITEMS force-feedback.dinput-effects force-feedback.wheel-type)
    set_tests_properties(${test} PROPERTIES
        TIMEOUT 120
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
        ENVIRONMENT_MODIFICATION "${FFB_CLEARED_ENVIRONMENT}")
    set_property(TEST ${test} PROPERTY LABELS force-feedback offline)
endforeach()
