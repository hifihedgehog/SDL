cmake_minimum_required(VERSION 3.22)
project(SDLLibUSBBackendTests LANGUAGES C CXX)

# Offline tests of SDL's libusb HIDAPI backend and of drivers on it: the
# Intel Wireless Series and DJI RC drivers, the I-Force joystick and haptic
# drivers, which build only with libusb, and the Namco USIO and Tacx
# drivers. A static SDL from this tree, configured with libusb loaded at run
# time as the shared library is, runs against a fake libusb-1.0.dll built
# here from test/testlibusbfake.c, or from test/testlibusbspecialtyfake.c
# for the USIO and Tacx. Each test loads its fake by its full path before
# SDL loads libusb by name, so no real USB device is reached, and none
# starts the HIDAPI joystick backend.
if(NOT WIN32 OR NOT CMAKE_C_COMPILER_ID STREQUAL "MSVC" OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "The libusb backend test requires native MSVC for 64-bit Windows.")
endif()
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    message(FATAL_ERROR "Use a separate build directory for the libusb backend test.")
endif()
if(NOT LibUSB_INCLUDE_PATH)
    message(FATAL_ERROR "Set LibUSB_INCLUDE_PATH and LibUSB_LIBRARY, as for the shared library.")
endif()

option(SDL_LIBUSB_BACKEND_TEST_ASAN "Instrument the test, the fake libusb and the static SDL with AddressSanitizer" OFF)
if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Offline test configuration" FORCE)
endif()
if(SDL_LIBUSB_BACKEND_TEST_ASAN)
    # Debug's default /RTC1 conflicts with MSVC AddressSanitizer.
    if(CMAKE_CONFIGURATION_TYPES)
        set(CMAKE_CONFIGURATION_TYPES Release RelWithDebInfo CACHE STRING "ASan configurations" FORCE)
    elseif(NOT CMAKE_BUILD_TYPE MATCHES "^(Release|RelWithDebInfo)$")
        message(FATAL_ERROR "ASan requires Release or RelWithDebInfo.")
    endif()
    # Without optimization every load stays where the source puts it. This
    # also applies to the static SDL below.
    foreach(config RELEASE RELWITHDEBINFO)
        string(REPLACE "/O2" "/Od" CMAKE_C_FLAGS_${config} "${CMAKE_C_FLAGS_${config}}")
    endforeach()
endif()

get_filename_component(LIBUSB_TEST_SDL_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(LIBUSB_TESTS "${LIBUSB_TEST_SDL_ROOT}/test")

# This cache belongs only to the standalone test build. SDL_HIDAPI_LIBUSB,
# SDL_HIDAPI_LIBUSB_SHARED and the libusb paths come from the command line,
# as for the shared library.
set(SDL_SHARED OFF CACHE BOOL "Offline static SDL dependency" FORCE)
set(SDL_STATIC ON CACHE BOOL "Offline static SDL dependency" FORCE)
set(SDL_TESTS OFF CACHE BOOL "Only register this sidecar's tests" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "No SDL test library needed" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "No example applications" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "No installation from this test build" FORCE)
add_subdirectory("${LIBUSB_TEST_SDL_ROOT}" "${CMAKE_CURRENT_BINARY_DIR}/sdl" EXCLUDE_FROM_ALL)
if(SDL_LIBUSB_BACKEND_TEST_ASAN)
    target_compile_options(SDL3-static PRIVATE /fsanitize=address /Zi)
endif()

# The fake and the test share one folder
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/bin")

add_library(testlibusbfake SHARED "${LIBUSB_TESTS}/testlibusbfake.c" "${LIBUSB_TESTS}/testlibusbfake.def")
set_target_properties(testlibusbfake PROPERTIES OUTPUT_NAME "libusb-1.0")
target_include_directories(testlibusbfake PRIVATE "${LibUSB_INCLUDE_PATH}")

foreach(target testlibusbbackend testiforcedriver)
    add_executable(${target} "${LIBUSB_TESTS}/${target}.c")
    target_compile_definitions(${target} PRIVATE USING_GENERATED_CONFIG_H SDL_STATIC_LIB)
    target_include_directories(${target} PRIVATE
        "${LIBUSB_TEST_SDL_ROOT}/src"
        "${CMAKE_CURRENT_BINARY_DIR}/sdl/include-config-$<LOWER_CASE:$<CONFIG>>/build_config"
        "${CMAKE_CURRENT_BINARY_DIR}/sdl/include-revision")
    target_link_libraries(${target} PRIVATE SDL3-static)
    add_dependencies(${target} testlibusbfake)
endforeach()

foreach(target testlibusbfake testlibusbbackend testiforcedriver)
    set_target_properties(${target} PROPERTIES
        C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    target_compile_options(${target} PRIVATE /W4 /WX /utf-8)
    if(SDL_LIBUSB_BACKEND_TEST_ASAN)
        target_compile_options(${target} PRIVATE /fsanitize=address /Zi)
        target_link_options(${target} PRIVATE /DEBUG)
    endif()
endforeach()

enable_testing()
add_test(NAME libusb-backend.drivers COMMAND testlibusbbackend "$<TARGET_FILE:testlibusbfake>")
add_test(NAME libusb-backend.iforce COMMAND testiforcedriver "$<TARGET_FILE:testlibusbfake>")
set_tests_properties(libusb-backend.drivers libusb-backend.iforce PROPERTIES
    TIMEOUT 120
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    LABELS "libusb-backend;offline")

# The Namco USIO and Tacx drivers and the backend's IN transfer timeout, run
# by test/testlibusbspecialty.c against a second fake libusb-1.0.dll, one
# that serves bulk devices, built from test/testlibusbspecialtyfake.c into a
# folder of its own. Each test loads its fake by its full path, so SDL takes
# that module when it loads libusb by name.
add_library(testlibusbspecialtyfake SHARED "${LIBUSB_TESTS}/testlibusbspecialtyfake.c" "${LIBUSB_TESTS}/testlibusbspecialtyfake.def")
set_target_properties(testlibusbspecialtyfake PROPERTIES
    OUTPUT_NAME "libusb-1.0"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/bin-specialty"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/bin-specialty"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/bin-specialty")
target_include_directories(testlibusbspecialtyfake PRIVATE "${LibUSB_INCLUDE_PATH}")

add_executable(testlibusbspecialty "${LIBUSB_TESTS}/testlibusbspecialty.c")
target_compile_definitions(testlibusbspecialty PRIVATE USING_GENERATED_CONFIG_H SDL_STATIC_LIB)
target_include_directories(testlibusbspecialty PRIVATE
    "${LIBUSB_TEST_SDL_ROOT}/src"
    "${CMAKE_CURRENT_BINARY_DIR}/sdl/include-config-$<LOWER_CASE:$<CONFIG>>/build_config"
    "${CMAKE_CURRENT_BINARY_DIR}/sdl/include-revision")
target_link_libraries(testlibusbspecialty PRIVATE SDL3-static)
add_dependencies(testlibusbspecialty testlibusbspecialtyfake)

foreach(target testlibusbspecialtyfake testlibusbspecialty)
    set_target_properties(${target} PROPERTIES
        C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    target_compile_options(${target} PRIVATE /W4 /WX /utf-8)
    if(SDL_LIBUSB_BACKEND_TEST_ASAN)
        target_compile_options(${target} PRIVATE /fsanitize=address /Zi)
        target_link_options(${target} PRIVATE /DEBUG)
    endif()
endforeach()

add_test(NAME libusb-backend.specialty COMMAND testlibusbspecialty "$<TARGET_FILE:testlibusbspecialtyfake>")
set_tests_properties(libusb-backend.specialty PROPERTIES
    TIMEOUT 120
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    LABELS "libusb-backend;offline")
