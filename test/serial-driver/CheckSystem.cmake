# Runs before testserialdriver links. It reads the external symbols the
# test's object file calls and fails when any of them could reach a device,
# the registry or a library: a Win32 function that opens, configures, reads,
# writes, flushes or cancels I/O on a file or COM port, lists devices through
# the configuration manager or SetupAPI, reads the registry, or loads a
# library. CloseHandle stays allowed, since the fake hands the driver's real
# event handles to it, and the fake port handles never reach it. The fake
# must be the only system the driver under test can reach.
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
#   01A 00000000 UNDEF  notype ()    External     | __imp_WriteFile
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
    if(name MATCHES "^(__imp_)?(CreateFile[AW]?|CreateFile2|ReadFile|ReadFileEx|WriteFile|WriteFileEx|DeviceIoControl|FlushFileBuffers)$" OR
       name MATCHES "^(__imp_)?(CancelIo|CancelIoEx|CancelSynchronousIo|GetOverlappedResult|GetOverlappedResultEx)$" OR
       name MATCHES "^(__imp_)?(GetCommState|SetCommState|GetCommTimeouts|SetCommTimeouts|PurgeComm|EscapeCommFunction|ClearCommError|SetupComm|SetCommMask|WaitCommEvent|TransmitCommChar|BuildCommDCB[AW]?|BuildCommDCBAndTimeouts[AW]?)$" OR
       name MATCHES "^(__imp_)?(LoadLibrary[AW]?|LoadLibraryEx[AW]?|GetProcAddress|FreeLibrary)$" OR
       name MATCHES "^(__imp_)?(Reg[A-Z]|CM_|SetupDi)")
        list(APPEND forbidden "${name}")
    endif()
endforeach()

list(JOIN names "\n" text)
file(WRITE "${REPORT}" "External symbols the driver test calls:\n${text}\n")
if(forbidden)
    list(JOIN forbidden ", " text)
    message(FATAL_ERROR "The driver test would reach the system through: ${text}. "
                        "Define each name the driver calls it through in testserialdriver.c.")
endif()
list(LENGTH names count)
message(STATUS "CheckSystem: ${count} external symbols, none reaches a device, the registry or a library")
