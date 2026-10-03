# Runs before testwiidriver links. It reads the external symbols the test's
# object file calls and fails when any of them could reach a HID device: one
# of SDL's HID functions, the HIDAPI rumble thread, the HIDAPI layer's
# joystick list, SDL's joystick lookup the fake replaces, or a Win32 function
# that opens or reads a device or loads a library. The scripted remote must be
# the only device the driver under test can reach.
#   cmake -DDUMPBIN=<dumpbin.exe> -DOBJECT=<object file> -DREPORT=<file> -P CheckSystem.cmake
foreach(variable DUMPBIN OBJECT REPORT)
    if(NOT DEFINED ${variable})
        message(FATAL_ERROR "CheckSystem.cmake needs -D${variable}=...")
    endif()
endforeach()

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
    if(name MATCHES "^SDL_hid_" OR
       name MATCHES "^SDL_HIDAPI_(LockRumble|UnlockRumble|SendRumble)" OR
       name MATCHES "^HIDAPI_Joystick(Connected|Disconnected)$" OR
       name MATCHES "^SDL_GetJoystickFromID" OR
       name MATCHES "^(__imp_)?(CreateFile[AW]?|CreateFile2|ReadFile|ReadFileEx|WriteFile|WriteFileEx|DeviceIoControl)$" OR
       name MATCHES "^(__imp_)?(LoadLibrary[AW]?|LoadLibraryEx[AW]?|GetProcAddress)$" OR
       name MATCHES "^(__imp_)?(HidD_|HidP_|CM_|SetupDi)")
        list(APPEND forbidden "${name}")
    endif()
endforeach()

list(JOIN names "\n" text)
file(WRITE "${REPORT}" "External symbols the driver test calls:\n${text}\n")
if(forbidden)
    list(JOIN forbidden ", " text)
    message(FATAL_ERROR "The driver test would reach a device through: ${text}. "
                        "Define each name the driver calls it through in testwiidriver.c.")
endif()
list(LENGTH names count)
message(STATUS "CheckSystem: ${count} external symbols, none reaches a HID device")
