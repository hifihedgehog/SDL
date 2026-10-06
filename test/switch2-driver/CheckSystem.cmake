# Runs before each test of this folder links. It reads the external symbols
# the test's object file calls and fails when one of them could reach a
# device the fake stands in for.
# - KIND=driver, for testswitch2driver: libusb, SDL's libusb loader, one of
#   SDL's HID functions, the HIDMaestro lookup, the HIDAPI rumble thread, the
#   HIDAPI layer's joystick list, or a Win32 function that opens or reads a
#   device or loads a library. The scripted bus must be the only device the
#   driver under test can reach.
# - KIND=backend, for testhidmaestroserial: a Win32 function that opens or
#   closes a device path. The HID backend it includes keeps its read and
#   write calls and its library loader, which only an opened device or
#   hid_init() reaches, and the test opens none and marks the backend
#   initialized.
#   cmake -DDUMPBIN=<dumpbin.exe> -DKIND=<driver|backend> -DOBJECT=<object file> -DREPORT=<file> -P CheckSystem.cmake
foreach(variable DUMPBIN KIND OBJECT REPORT)
    if(NOT DEFINED ${variable})
        message(FATAL_ERROR "CheckSystem.cmake needs -D${variable}=...")
    endif()
endforeach()
if(NOT KIND STREQUAL "driver" AND NOT KIND STREQUAL "backend")
    message(FATAL_ERROR "CheckSystem.cmake: KIND is driver or backend, not ${KIND}")
endif()

execute_process(COMMAND "${DUMPBIN}" /NOLOGO /SYMBOLS "${OBJECT}"
    OUTPUT_VARIABLE symbols ERROR_VARIABLE errors RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "dumpbin failed on ${OBJECT}: ${errors}")
endif()

# A COFF symbol table line for a called function reads
#   01A 00000000 UNDEF  notype ()    External     | SDL_hid_write_REAL
string(REGEX MATCHALL "[^\r\n]*UNDEF[^\r\n]*External[^\r\n]*" undefined "${symbols}")
set(names)
foreach(line IN LISTS undefined)
    if(line MATCHES "\\| *([^ \r\n]+)")
        list(APPEND names "${CMAKE_MATCH_1}")
    endif()
endforeach()
list(REMOVE_DUPLICATES names)
list(SORT names)

set(forbidden)
foreach(name IN LISTS names)
    if(KIND STREQUAL "driver")
        if(name MATCHES "^(__imp_)?libusb_" OR
           name MATCHES "^SDL_(Init|Quit)LibUSB$" OR
           name MATCHES "^SDL_hid_" OR
           name MATCHES "^SDL_Hidmaestro" OR
           name MATCHES "^SDL_HIDAPI_(LockRumble|UnlockRumble|SendRumble)" OR
           name MATCHES "^HIDAPI_Joystick(Connected|Disconnected)$" OR
           name MATCHES "^(__imp_)?(CreateFile[AW]?|CreateFile2|ReadFile|ReadFileEx|WriteFile|WriteFileEx|DeviceIoControl)$" OR
           name MATCHES "^(__imp_)?(LoadLibrary[AW]?|LoadLibraryEx[AW]?|GetProcAddress)$" OR
           name MATCHES "^(__imp_)?(HidD_|HidP_|CM_|SetupDi|WinUsb_)")
            list(APPEND forbidden "${name}")
        endif()
    else()
        if(name MATCHES "^(__imp_)?(CreateFile[AW]?|CreateFile2|CloseHandle)$" OR
           name MATCHES "^(__imp_)?(HidD_|HidP_|CM_|SetupDi)")
            list(APPEND forbidden "${name}")
        endif()
    endif()
endforeach()

list(JOIN names "\n" text)
file(WRITE "${REPORT}" "External symbols the test calls:\n${text}\n")
if(forbidden)
    list(JOIN forbidden ", " text)
    message(FATAL_ERROR "The test would reach a device through: ${text}. "
                        "Define each name the code under test calls it through in the test file.")
endif()
list(LENGTH names count)
message(STATUS "CheckSystem (${KIND}): ${count} external symbols, none reaches a device")
