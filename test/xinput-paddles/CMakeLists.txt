cmake_minimum_required(VERSION 3.22)
project(SDLXInputPaddleTests LANGUAGES C CXX)

if(NOT WIN32 OR NOT CMAKE_C_COMPILER_ID STREQUAL "MSVC" OR
   NOT CMAKE_CXX_COMPILER_ID STREQUAL "MSVC" OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "These offline tests require native MSVC for Windows x64.")
endif()
include(CheckCXXSourceCompiles)
check_cxx_source_compiles("
#if !defined(_M_X64) || defined(_M_ARM64EC)
#error Native x64 compiler required
#endif
int main() { return 0; }
" PADDLE_TEST_NATIVE_X64)
if(NOT PADDLE_TEST_NATIVE_X64)
    message(FATAL_ERROR "These offline tests require native x64, excluding ARM64 and ARM64EC.")
endif()
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    message(FATAL_ERROR "Use a separate build directory for the offline tests.")
endif()

option(SDL_XINPUT_PADDLE_TEST_ASAN "Instrument the offline test targets with AddressSanitizer" OFF)
if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Offline test configuration" FORCE)
endif()
if(SDL_XINPUT_PADDLE_TEST_ASAN)
    # Debug's default /RTC1 conflicts with MSVC AddressSanitizer.
    if(CMAKE_CONFIGURATION_TYPES)
        set(CMAKE_CONFIGURATION_TYPES "Release;RelWithDebInfo" CACHE STRING "ASan configurations" FORCE)
    elseif(NOT CMAKE_BUILD_TYPE MATCHES "^(Release|RelWithDebInfo)$")
        message(FATAL_ERROR "ASan requires Release or RelWithDebInfo.")
    endif()
endif()

get_filename_component(PADDLE_SDL_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(PADDLE_SOURCE "${PADDLE_SDL_ROOT}/src/joystick/windows")
set(PADDLE_TESTS "${PADDLE_SDL_ROOT}/test")

# This cache belongs only to the standalone test build. The real SDL archive
# supplies the mapping test's event, joystick, virtual-device, and memory code.
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
set(SDL_HIDAPI_JOYSTICK OFF CACHE BOOL "Virtual input only" FORCE)
set(SDL_HIDAPI_LIBUSB OFF CACHE BOOL "No native USB acquisition" FORCE)
set(SDL_VIRTUAL_JOYSTICK ON CACHE BOOL "Virtual input tests" FORCE)
add_subdirectory("${PADDLE_SDL_ROOT}" "${CMAKE_CURRENT_BINARY_DIR}/sdl" EXCLUDE_FROM_ALL)

enable_testing()
# Mapping also sets these hints at override priority before SDL_Init and checks
# that the initial device list is empty. Other tests inject their native inputs.
set(PADDLE_OFFLINE_ENVIRONMENT
    "SDL_JOYSTICK_HIDAPI=0"
    "SDL_JOYSTICK_RAWINPUT=0"
    "SDL_JOYSTICK_DIRECTINPUT=0"
    "SDL_XINPUT_ENABLED=0"
    "SDL_JOYSTICK_WGI=0"
    "SDL_JOYSTICK_GAMEINPUT=0"
    "SDL_JOYSTICK_GAMEINPUT_RAW=0"
    "SDL_JOYSTICK_BLE_SWITCH2=0"
    "SDL_JOYSTICK_THREAD=0"
    "SDL_JOYSTICK_WINMM=0"
    "SDL_JOYSTICK_ROG_CHAKRAM=0")

function(paddle_test part)
    set(target "testxinputpaddle${part}")
    add_executable(${target} ${ARGN})
    set_target_properties(${target} PROPERTIES
        C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO
        CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
    target_compile_options(${target} PRIVATE /W4 /WX /utf-8
        "$<$<COMPILE_LANGUAGE:CXX>:/EHsc>"
        "$<$<COMPILE_LANGUAGE:CXX>:/permissive->")
    target_link_options(${target} PRIVATE /INCREMENTAL:NO)
    if(SDL_XINPUT_PADDLE_TEST_ASAN)
        target_compile_options(${target} PRIVATE /fsanitize=address /Zi)
        target_link_options(${target} PRIVATE /DEBUG)
    endif()
    # No live or probe arguments are accepted from configuration variables.
    add_test(NAME "xinput-paddles.${part}" COMMAND ${target})
    set_tests_properties("xinput-paddles.${part}" PROPERTIES
        TIMEOUT 60 LABELS "xinput-paddles;offline"
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
        ENVIRONMENT "${PADDLE_OFFLINE_ENVIRONMENT}")
endfunction()

paddle_test(decode "${PADDLE_TESTS}/testxinputpaddledecode.c"
    "${PADDLE_SOURCE}/SDL_xinput_paddle_decode.c")

paddle_test(reader "${PADDLE_TESTS}/testxinputpaddlereader.cpp"
    "${PADDLE_SOURCE}/SDL_xinput_paddle_reader.cpp")
target_compile_definitions(testxinputpaddlereader PRIVATE SDL_XINPUT_PADDLE_READER_TESTING)
target_compile_options(testxinputpaddlereader PRIVATE /volatile:ms)

# These canonical tests include their implementation to exercise private seams.
# Do not also compile the included .cpp as a separate translation unit.
paddle_test(identity "${PADDLE_TESTS}/testxinputpaddleidentity.cpp")
target_link_libraries(testxinputpaddleidentity PRIVATE cfgmgr32 advapi32)

paddle_test(gatt "${PADDLE_TESTS}/testxinputpaddlegatt.cpp")
target_link_libraries(testxinputpaddlegatt PRIVATE windowsapp cfgmgr32 ole32)

paddle_test(service "${PADDLE_TESTS}/testxinputpaddleservice.cpp"
    "${PADDLE_SOURCE}/SDL_xinput_paddle_reader.cpp"
    "${PADDLE_SOURCE}/SDL_xinput_paddle_decode.c")
target_compile_definitions(testxinputpaddleservice PRIVATE SDL_XINPUT_PADDLE_READER_TESTING)
target_compile_options(testxinputpaddleservice PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:/volatile:ms>")
target_link_libraries(testxinputpaddleservice PRIVATE advapi32 bcrypt)

paddle_test(broker "${PADDLE_TESTS}/testxinputpaddlebroker.cpp"
    "${PADDLE_SOURCE}/SDL_xinput_paddle_decode.c"
    "${PADDLE_SOURCE}/SDL_xinput_paddle_identity.cpp"
    "${PADDLE_SOURCE}/SDL_xinput_paddle_trace.cpp")
target_link_libraries(testxinputpaddlebroker PRIVATE cfgmgr32 advapi32)

paddle_test(wgi "${PADDLE_TESTS}/testxinputpaddlewgi.cpp"
    "${PADDLE_SOURCE}/SDL_xinput_paddle_trace.cpp")
target_link_libraries(testxinputpaddlewgi PRIVATE windowsapp ole32)

paddle_test(trace "${PADDLE_TESTS}/testxinputpaddletrace.cpp"
    "${PADDLE_SOURCE}/SDL_xinput_paddle_trace.cpp")

# The runtime test's no-argument entry runs FakeSource cases. Its manual --live
# branch is never registered with CTest.
paddle_test(runtime "${PADDLE_TESTS}/testxinputpaddleruntime.cpp")
target_link_libraries(testxinputpaddleruntime PRIVATE advapi32 bcrypt)

paddle_test(mapping "${PADDLE_TESTS}/testxinputpaddlemapping.c")
target_compile_definitions(testxinputpaddlemapping PRIVATE USING_GENERATED_CONFIG_H SDL_STATIC_LIB)
target_include_directories(testxinputpaddlemapping PRIVATE
    "${PADDLE_SDL_ROOT}/src"
    "${CMAKE_CURRENT_BINARY_DIR}/sdl/include-config-$<LOWER_CASE:$<CONFIG>>/build_config"
    "${CMAKE_CURRENT_BINARY_DIR}/sdl/include-revision")
target_link_libraries(testxinputpaddlemapping PRIVATE SDL3-static)
# The included upstream SDL_gamepad.c has unused callback parameters. Suppress
# only C4100 on this target. All other warnings remain enabled and are errors.
target_compile_options(testxinputpaddlemapping PRIVATE /wd4100)

# The pipeline's C bridge includes the same real mapping implementation. Its
# C++ half includes the broker implementation under the offline compile guard.
paddle_test(pipeline "${PADDLE_TESTS}/testxinputpaddlepipeline.c"
    "${PADDLE_TESTS}/testxinputpaddlepipeline.cpp"
    "${PADDLE_SOURCE}/SDL_xinput_paddle_decode.c"
    "${PADDLE_SOURCE}/SDL_xinput_paddle_identity.cpp"
    "${PADDLE_SOURCE}/SDL_xinput_paddle_trace.cpp")
target_compile_definitions(testxinputpaddlepipeline PRIVATE USING_GENERATED_CONFIG_H SDL_STATIC_LIB)
target_include_directories(testxinputpaddlepipeline PRIVATE
    "${PADDLE_SDL_ROOT}/src"
    "${CMAKE_CURRENT_BINARY_DIR}/sdl/include-config-$<LOWER_CASE:$<CONFIG>>/build_config"
    "${CMAKE_CURRENT_BINARY_DIR}/sdl/include-revision")
target_link_libraries(testxinputpaddlepipeline PRIVATE SDL3-static cfgmgr32 advapi32)
# Preserve /W4 /WX for the C++ and decoder sources. Only the C bridge includes
# upstream callback parameters that need the existing C4100 exception.
set_source_files_properties("${PADDLE_TESTS}/testxinputpaddlepipeline.c"
    PROPERTIES COMPILE_OPTIONS /wd4100)

add_test(NAME xinput-paddles.mapping-empty COMMAND testxinputpaddlemapping --empty-field)
set_tests_properties(xinput-paddles.mapping-empty PROPERTIES
    TIMEOUT 60 LABELS "xinput-paddles;offline"
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    ENVIRONMENT "${PADDLE_OFFLINE_ENVIRONMENT}")
