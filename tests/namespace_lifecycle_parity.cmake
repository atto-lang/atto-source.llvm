# Source-only conformance gate: no ambient namespace package loading and no
# ordinary invocation/emission of once-only init while native lowering lacks it.
if(NOT DEFINED COMPILER OR NOT DEFINED WORK_DIR)
  message(FATAL_ERROR "COMPILER and WORK_DIR are required")
endif()
file(MAKE_DIRECTORY "${WORK_DIR}")

function(expect_rejection name source_text diagnostic)
  set(input "${WORK_DIR}/${name}.ores")
  file(WRITE "${input}" "${source_text}")
  # Rejected inputs must not leave native output on disk even when stdout is
  # empty. Check both common compiler output naming conventions; remove stale
  # outputs from prior CTest attempts so each case measures this invocation.
  set(artifact_suffixes ".ll" ".bc" ".o" ".obj" ".s" ".wasm" ".out" ".exe")
  foreach(stem "${WORK_DIR}/${name}" "${input}")
    foreach(suffix IN LISTS artifact_suffixes)
      file(REMOVE "${stem}${suffix}")
    endforeach()
  endforeach()
  execute_process(
    COMMAND "${COMPILER}" "${input}"
    RESULT_VARIABLE status OUTPUT_VARIABLE ir ERROR_VARIABLE err
    TIMEOUT 15)
  if(NOT "${status}" MATCHES "^[0-9]+$")
    message(FATAL_ERROR "${name} compiler did not exit normally: ${status}")
  endif()
  if(status EQUAL 0)
    message(FATAL_ERROR "${name} incorrectly admitted")
  endif()
  if(NOT ir STREQUAL "")
    message(FATAL_ERROR "${name} emitted IR on rejection")
  endif()
  foreach(stem "${WORK_DIR}/${name}" "${input}")
    foreach(suffix IN LISTS artifact_suffixes)
      if(EXISTS "${stem}${suffix}")
        message(FATAL_ERROR "${name} wrote ${stem}${suffix} on rejection")
      endif()
    endforeach()
  endforeach()
  string(FIND "${err}" "${diagnostic}" found)
  if(found EQUAL -1)
    message(FATAL_ERROR "${name} expected '${diagnostic}' but saw: ${err}")
  endif()
endfunction()

expect_rejection(namespace-import
  "import namespace billing as payments from \"checkout\";\ndefine module App as pub fnc main(): int { return 0; } end\n"
  "verified manifest package index")
expect_rejection(lifecycle-init
  "define module App as pub fnc init(): int { return 1; } pub fnc main(): int { return 0; } end\n"
  "once-only reachable initialization lowering")


# A private init is still a once-only lifecycle hook, not an ordinary helper.
expect_rejection(lifecycle-private-init
  "define module App as fnc init(): int { return 1; } pub fnc main(): int { return 0; } end\n"
  "once-only reachable initialization lowering")

# Every reachable provider must be checked before the root's no-import/linked
# compiler paths can emit IR. The imported module's init must fail closed.
file(WRITE "${WORK_DIR}/provider-with-init.ores"
  "define module Provider as pub fnc init(): int { return 1; } end\n")
expect_rejection(lifecycle-imported-init
  "import module Provider from \"./provider-with-init.ores\";\ndefine module App as pub fnc main(): int { return 0; } end\n"
  "once-only reachable initialization lowering")

# A private imported hook must not bypass the same once-only lifecycle fence.
file(WRITE "${WORK_DIR}/private-provider.ores"
  "define module PrivateProvider as fnc init():int{return 1;} pub fnc answer():int{return 42;} end\n")
expect_rejection(lifecycle-imported-private-init
  "import module PrivateProvider from \"./private-provider.ores\"; pub fnc main():int{return PrivateProvider.answer();}\n"
  "once-only reachable initialization lowering")

# Reject hooks in transitive reachable dependencies, not only direct imports.
file(WRITE "${WORK_DIR}/deep-with-init.ores"
  "define module Deep as pub fnc init():int{return 1;} pub fnc answer():int{return 42;} end\n")
file(WRITE "${WORK_DIR}/middle.ores"
  "import module Deep from \"./deep-with-init.ores\"; define module Middle as pub fnc answer():int{return Deep.answer();} end\n")
expect_rejection(lifecycle-transitive-init
  "import module Middle from \"./middle.ores\"; pub fnc main():int{return Middle.answer();}\n"
  "once-only reachable initialization lowering")

# A source file in the same directory but not reached by an import must
# not enter the native compile graph or run an init hook.
file(WRITE "${WORK_DIR}/unrelated.ores"
  "define module Unrelated as pub fnc init(): int { return 1; } end\n")
file(WRITE "${WORK_DIR}/main.ores"
  "define module App as pub fnc main(): int { return 42; } end\n")
execute_process(
  COMMAND "${COMPILER}" "${WORK_DIR}/main.ores"
  RESULT_VARIABLE status OUTPUT_VARIABLE ir ERROR_VARIABLE err
  TIMEOUT 15)
if(NOT "${status}" MATCHES "^[0-9]+$" OR NOT status EQUAL 0 OR NOT ir MATCHES "define i32 @main")
  message(FATAL_ERROR "unreferenced namespace source contaminated native compilation: ${status}: ${err}")
endif()

# A clean, explicitly imported provider must remain admitted; rejecting all
# imports would incorrectly satisfy the negative-only lifecycle cases.
file(WRITE "${WORK_DIR}/clean-provider.ores"
  "define module Clean as pub fnc answer():int{return 42;} end\n")
file(WRITE "${WORK_DIR}/clean-main.ores"
  "import module Clean from \"./clean-provider.ores\"; pub fnc main():int{return Clean.answer();}\n")
execute_process(
  COMMAND "${COMPILER}" "${WORK_DIR}/clean-main.ores"
  RESULT_VARIABLE status OUTPUT_VARIABLE ir ERROR_VARIABLE err
  TIMEOUT 15)
if(NOT "${status}" MATCHES "^[0-9]+$" OR NOT status EQUAL 0 OR NOT ir MATCHES "define i32 @main")
  message(FATAL_ERROR "valid explicit module import rejected: ${status}: ${err}")
endif()
