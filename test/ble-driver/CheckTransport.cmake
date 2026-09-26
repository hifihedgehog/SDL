# Runs before testblegattdriver links. It reads the external symbols the
# test's object file calls and fails when any of them could reach Bluetooth:
# an SDL_BLEGATT_ function the test did not rename to its fake transport, a
# Windows Bluetooth or WinRT entry point, or a function that resolves entry
# points at run time. A function resolved that way never appears in the
# symbol table, so the resolvers themselves are refused. The fake transport
# must be the only transport the driver under test can reach.
#   cmake -DDUMPBIN=<dumpbin.exe> -DOBJECT=<object file> -DREPORT=<file> -P CheckTransport.cmake
foreach(variable DUMPBIN OBJECT REPORT)
    if(NOT DEFINED ${variable})
        message(FATAL_ERROR "CheckTransport.cmake needs -D${variable}=...")
    endif()
endforeach()

execute_process(COMMAND "${DUMPBIN}" /NOLOGO /SYMBOLS "${OBJECT}"
    OUTPUT_VARIABLE symbols ERROR_VARIABLE errors RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "dumpbin failed on ${OBJECT}: ${errors}")
endif()

# A COFF symbol table line for a called function reads
#   01A 00000000 UNDEF  notype ()    External     | SDL_LockJoysticks_REAL
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
    if(name MATCHES "^SDL_BLEGATT_" OR name MATCHES "Bluetooth" OR
       name MATCHES "^(__imp_)?Ro(GetActivationFactory|ActivateInstance|Initialize|Uninitialize)" OR
       name MATCHES "^(__imp_)?WindowsCreateString" OR
       name MATCHES "^WIN_(LoadComBaseFunction|RoInitialize|RoUninitialize)$" OR
       name MATCHES "^(__imp_)?(GetProcAddress|LoadLibrary)")
        list(APPEND forbidden "${name}")
    endif()
endforeach()

list(JOIN names "\n" text)
file(WRITE "${REPORT}" "External symbols the driver test calls:\n${text}\n")
if(forbidden)
    list(JOIN forbidden ", " text)
    message(FATAL_ERROR "The driver test would reach a real transport through: ${text}. "
                        "Rename each function in testblegattdriver.c to the fake transport.")
endif()
list(LENGTH names count)
message(STATUS "CheckTransport: ${count} external symbols, none reaches a real transport")
