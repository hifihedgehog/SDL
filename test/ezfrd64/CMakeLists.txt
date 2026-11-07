cmake_minimum_required(VERSION 3.22)
project(SDLEZFRD64Tests LANGUAGES C)

# Offline test of the EZFRD64.DLL loader patch of hifihedgehog/SDL#47: the
# patch inside a real static SDL against stand-in modules this project
# builds, and the DirectInput haptic listing against a scripted DirectInput.
# No device opens, and no joystick or haptic subsystem starts.
if(NOT WIN32 OR NOT CMAKE_C_COMPILER_ID STREQUAL "MSVC" OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "The EZFRD64 test requires native MSVC for 64-bit Windows.")
endif()
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    message(FATAL_ERROR "Use a separate build directory for the offline tests.")
endif()

option(SDL_EZFRD64_TEST_ASAN "Instrument the tests and the static SDL with AddressSanitizer" OFF)
if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Offline test configuration" FORCE)
endif()
if(SDL_EZFRD64_TEST_ASAN)
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

get_filename_component(EZFRD64_SDL_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(EZFRD64_TESTS "${EZFRD64_SDL_ROOT}/test")

# The cache of this standalone build only, as in test/gameinput-driver
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
add_subdirectory("${EZFRD64_SDL_ROOT}" "${CMAKE_CURRENT_BINARY_DIR}/sdl" EXCLUDE_FROM_ALL)
if(SDL_EZFRD64_TEST_ASAN)
    target_compile_options(SDL3-static PRIVATE /fsanitize=address /Zi)
endif()

enable_testing()

# The stand-in modules, without SDL and without AddressSanitizer: each makes
# its heap through its kernel32 HeapCreate import, as the real module does.
# NOTEZFRD64.dll is the same module under a name the patch leaves alone.
foreach(name IN ITEMS EZFRD64 811EZFRD64 NOTEZFRD64)
    add_library(module_${name} SHARED "${EZFRD64_TESTS}/testezfrd64_module.c")
    set_target_properties(module_${name} PROPERTIES OUTPUT_NAME "${name}" PREFIX "")
    target_compile_options(module_${name} PRIVATE /W4 /WX /utf-8)
endforeach()
set_target_properties(module_811EZFRD64 PROPERTIES SUFFIX ".DLL")

add_executable(testezfrd64 "${EZFRD64_TESTS}/testezfrd64.c")
add_dependencies(testezfrd64 module_EZFRD64 module_811EZFRD64 module_NOTEZFRD64)
add_executable(testezfrd64haptic "${EZFRD64_TESTS}/testezfrd64haptic.c")
foreach(target IN ITEMS testezfrd64 testezfrd64haptic)
    target_link_libraries(${target} PRIVATE SDL3-static)
    target_compile_definitions(${target} PRIVATE USING_GENERATED_CONFIG_H SDL_STATIC_LIB)
    target_include_directories(${target} PRIVATE
        "${EZFRD64_SDL_ROOT}/src"
        "${CMAKE_CURRENT_BINARY_DIR}/sdl/include-config-$<LOWER_CASE:$<CONFIG>>/build_config"
        "${CMAKE_CURRENT_BINARY_DIR}/sdl/include-revision")
    target_link_libraries(${target} PRIVATE SDL3::Headers)
    target_compile_options(${target} PRIVATE /W4 /WX /utf-8)
    target_link_options(${target} PRIVATE /INCREMENTAL:NO)
    if(SDL_EZFRD64_TEST_ASAN)
        target_compile_options(${target} PRIVATE /fsanitize=address /Zi)
        target_link_options(${target} PRIVATE /DEBUG)
    endif()
endforeach()
# SDL_dinputhaptic.c, which the haptic test includes, leaves some of its
# functions' parameters unused (C4100) and holds an unused helper (C4505).
# SDL's own /W3 build does not report them.
target_compile_options(testezfrd64haptic PRIVATE /wd4100 /wd4505)

# No joystick subsystem starts, and the patch's hints come from the tests
set(EZFRD64_CLEARED_ENVIRONMENT
    "SDL_PATCH_BROKEN_EZFRD64=unset:"
    "SDL_CHECK_BROKEN_EZFRD64=unset:")
add_test(NAME ezfrd64.patch COMMAND testezfrd64)
add_test(NAME ezfrd64.haptic COMMAND testezfrd64haptic)
foreach(test IN ITEMS ezfrd64.patch ezfrd64.haptic)
    set_tests_properties(${test} PROPERTIES
        TIMEOUT 120
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
        ENVIRONMENT_MODIFICATION "${EZFRD64_CLEARED_ENVIRONMENT}")
    set_property(TEST ${test} PROPERTY LABELS ezfrd64 offline)
endforeach()
