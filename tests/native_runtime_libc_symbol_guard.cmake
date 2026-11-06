# Every unprefixed libc/POSIX symbol imported by the host runtimes that
# scripts/compile-aot.sh links must be listed in HostRuntimeLibcSymbols.hpp,
# otherwise a guest function with that name could replace it at link time.
#
# Inputs (-D): ROOT (repo), BUILD (build dir), CLANG, NM (llvm-nm or nm).
cmake_minimum_required(VERSION 3.20)

file(READ "${ROOT}/src/main/cpp/oreslang/backend/HostRuntimeLibcSymbols.hpp" header)
string(REGEX MATCH "kHostRuntimeLibcSymbols\\[\\] = {[^}]*}" table "${header}")
string(REGEX MATCHALL "\"[A-Za-z0-9_]+\"" quoted "${table}")
set(reserved "")
foreach(entry IN LISTS quoted)
  string(REPLACE "\"" "" entry "${entry}")
  list(APPEND reserved "${entry}")
endforeach()
list(LENGTH reserved count)
if(count LESS 10)
  message(FATAL_ERROR "could not parse kHostRuntimeLibcSymbols (${count} entries)")
endif()

set(work "${BUILD}/runtime-libc-symbol-guard")
file(MAKE_DIRECTORY "${work}")
set(missing "")
foreach(runtime NativeStringRegion NativeRecordRegion NativeIntArrayRegion)
  set(object "${work}/${runtime}.o")
  execute_process(
    COMMAND "${CLANG}" -O2 -std=c11 -I "${ROOT}/src/main/cpp" -c
      "${ROOT}/src/main/cpp/oreslang/runtime/${runtime}.c" -o "${object}"
    RESULT_VARIABLE compiled ERROR_VARIABLE compile_err)
  if(NOT compiled EQUAL 0)
    message(FATAL_ERROR "${runtime}: compile failed: ${compile_err}")
  endif()
  execute_process(COMMAND "${NM}" -u "${object}"
    RESULT_VARIABLE listed OUTPUT_VARIABLE symbols ERROR_VARIABLE nm_err)
  if(NOT listed EQUAL 0)
    message(FATAL_ERROR "${runtime}: nm failed: ${nm_err}")
  endif()
  string(REGEX MATCHALL "[A-Za-z0-9_.$@]+\n" lines "${symbols}")
  foreach(line IN LISTS lines)
    string(STRIP "${line}" symbol)
    if(APPLE AND symbol MATCHES "^_")
      # Mach-O adds exactly one `_` to C symbols. (REGEX REPLACE "^_" would
      # re-anchor and strip every leading underscore.)
      string(SUBSTRING "${symbol}" 1 -1 symbol)
    endif()
    # Compiler/libc-internal names (`_x`, `__x`) and the runtime's own ABI are
    # not valid unprefixed guest function symbols.
    if(symbol MATCHES "^_" OR symbol MATCHES "^ores_" OR symbol MATCHES "^oreslang_")
      continue()
    endif()
    list(FIND reserved "${symbol}" at)
    if(at EQUAL -1)
      list(APPEND missing "${runtime}:${symbol}")
    endif()
  endforeach()
endforeach()

if(missing)
  message(FATAL_ERROR
    "runtime imports libc symbols missing from HostRuntimeLibcSymbols.hpp: ${missing}")
endif()
message(STATUS "runtime libc symbol guard: all imports reserved (${count} names)")
