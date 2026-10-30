# Source-only conformance gate: no ambient namespace package loading and no
# ordinary invocation/emission of once-only init while native lowering lacks it.
if(NOT DEFINED COMPILER OR NOT DEFINED WORK_DIR)
  message(FATAL_ERROR "COMPILER and WORK_DIR are required")
endif()
file(MAKE_DIRECTORY "${WORK_DIR}")

function(expect_rejection name source_text diagnostic)
  set(input "${WORK_DIR}/${name}.ores")
  file(WRITE "${input}" "${source_text}")
  execute_process(
    COMMAND "${COMPILER}" "${input}"
    RESULT_VARIABLE status OUTPUT_VARIABLE ir ERROR_VARIABLE err
    TIMEOUT 15)
  if(status EQUAL 0)
    message(FATAL_ERROR "${name} incorrectly admitted")
  endif()
  if(NOT ir STREQUAL "")
    message(FATAL_ERROR "${name} emitted IR on rejection")
  endif()
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
  "import module Provider from \"provider-with-init.ores\";\ndefine module App as pub fnc main(): int { return 0; } end\n"
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
if(NOT status EQUAL 0 OR NOT ir MATCHES "define i32 @main")
  message(FATAL_ERROR "unreferenced namespace source contaminated native compilation: ${err}")
endif()
