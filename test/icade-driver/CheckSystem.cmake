# Runs before testicade links. It reads the external symbols the test's
# object file calls and fails when any of them could reach a device or the
# process's Raw Input registration: a Win32 function that lists, names,
# opens or identifies a device, reads raw input, registers for raw input or
# device notifications, or loads a library, and SDL's shared hid.dll state
# in SDL_hid.c, which the driver must not use. The fake system must be the
# only system the driver under test can reach.
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
#   01A 00000000 UNDEF  notype ()    External     | __imp_GetRawInputData
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
    if(name MATCHES "^(__imp_)?(RegisterRawInputDevices|GetRegisteredRawInputDevices|GetRawInputDeviceList|GetRawInputDeviceInfo[AW]?|GetRawInputData|GetRawInputBuffer)$" OR
       name MATCHES "^(__imp_)?(CreateFile[AW]?|CreateFile2|DeviceIoControl|CloseHandle)$" OR
       name MATCHES "^(__imp_)?(RegisterDeviceNotification[AW]?|UnregisterDeviceNotification)$" OR
       name MATCHES "^(__imp_)?(LoadLibrary[AW]?|LoadLibraryEx[AW]?|GetProcAddress|FreeLibrary)$" OR
       name MATCHES "^(__imp_)?(HidD_|HidP_|CM_)" OR
       name MATCHES "^(WIN_LoadHIDDLL|WIN_UnloadHIDDLL|SDL_HidD_|SDL_HidP_)")
        list(APPEND forbidden "${name}")
    endif()
endforeach()

list(JOIN names "\n" text)
file(WRITE "${REPORT}" "External symbols the driver test calls:\n${text}\n")
if(forbidden)
    list(JOIN forbidden ", " text)
    message(FATAL_ERROR "The driver test would reach the system through: ${text}. "
                        "Define each name the driver calls it through in testicade.c.")
endif()
list(LENGTH names count)
message(STATUS "CheckSystem: ${count} external symbols, none reaches a device, a library or the Raw Input registration")
