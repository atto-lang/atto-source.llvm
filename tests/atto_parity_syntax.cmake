# Compile and execute the .atto parity-syntax fixtures with the real native
# pipeline (oreslang-llvmc -> clang -> executable) and compare stdout and exit
# status. Negative cases must fail in the compiler with a specific diagnostic.
#
# Inputs (-D): ROOT (repo), BUILD (build dir), CLANG (clang path).
cmake_minimum_required(VERSION 3.20)

set(fixtures "${ROOT}/tests/fixtures/atto_parity_syntax")
set(work "${BUILD}/atto-parity-syntax")
file(REMOVE_RECURSE "${work}")
file(MAKE_DIRECTORY "${work}")
set(ENV{ORESLANG_LLVMC} "${BUILD}/oreslang-llvmc")
set(ENV{ORES_NO_GC_IR_AUDIT} "${BUILD}/oreslang-no-gc-ir-audit")
set(ENV{CLANG} "${CLANG}")
set(failures 0)

function(fail_case message)
  message(SEND_ERROR "${message}")
  math(EXPR n "${failures} + 1")
  set(failures ${n} PARENT_SCOPE)
endfunction()

# name: case id; source: .atto entry; expected_exit: exact status or "nonzero".
function(run_case name source expected_exit)
  set(case_start ${failures})
  get_filename_component(base "${source}" NAME_WE)
  get_filename_component(dir "${source}" DIRECTORY)
  set(binary "${work}/${name}")
  execute_process(
    COMMAND bash "${ROOT}/scripts/compile-aot.sh" "${source}" "${binary}"
    RESULT_VARIABLE compiled OUTPUT_VARIABLE ignored ERROR_VARIABLE compile_err)
  if(NOT compiled EQUAL 0)
    fail_case("${name}: compile failed: ${compile_err}")
    set(failures ${failures} PARENT_SCOPE)
    return()
  endif()
  # Output is captured through a pipe: the native console runtime only admits
  # non-blocking pipes, sockets or terminals as stdout.
  execute_process(COMMAND "${binary}" RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr TIMEOUT 30)
  file(READ "${dir}/${base}.expected" expected)
  if(NOT stdout STREQUAL expected)
    fail_case("${name}: stdout mismatch\n--- expected\n${expected}--- actual\n${stdout}--- stderr\n${stderr}")
  endif()
  # Optional <base>.stderr: text the run's stderr must contain (panic messages).
  if(EXISTS "${dir}/${base}.stderr")
    file(READ "${dir}/${base}.stderr" expected_stderr)
    string(FIND "${stderr}" "${expected_stderr}" at)
    if(at EQUAL -1)
      fail_case("${name}: stderr lacks '${expected_stderr}', got: ${stderr}")
    endif()
  endif()
  set(before )
  if(expected_exit STREQUAL "nonzero")
    if(status EQUAL 0)
      fail_case("${name}: expected a nonzero (panic) exit, got 0")
    endif()
  elseif(NOT status STREQUAL expected_exit)
    fail_case("${name}: exit ${status}, expected ${expected_exit}; stderr: ${stderr}")
  endif()
  set(failures ${failures} PARENT_SCOPE)
  if(failures EQUAL case_start)
    message(STATUS "PASS ${name}")
  endif()
endfunction()

# Negative case: compiler must reject `text` with a diagnostic containing `needle`.
function(reject_case name text needle)
  set(dir "${work}/${name}")
  file(MAKE_DIRECTORY "${dir}")
  file(WRITE "${dir}/main.atto" "${text}")
  file(WRITE "${dir}/lib.atto"
    "pub fnc shown(int x): int { return x; }\nfnc hidden(int x): int { return x; }\n")
  # Extra arguments after `needle` are compiler flags (e.g. a freestanding target).
  execute_process(COMMAND "${BUILD}/oreslang-llvmc" ${ARGN} "${dir}/main.atto"
    RESULT_VARIABLE status OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
  if(status EQUAL 0 OR NOT stdout STREQUAL "")
    fail_case("${name}: compiler admitted invalid source")
  else()
    string(FIND "${stderr}" "${needle}" at)
    if(at EQUAL -1)
      fail_case("${name}: expected diagnostic containing '${needle}', got: ${stderr}")
    else()
      message(STATUS "PASS ${name}")
    endif()
  endif()
  set(failures ${failures} PARENT_SCOPE)
endfunction()

run_case(if_then "${fixtures}/if_then.atto" 0)
run_case(stdout_write "${fixtures}/stdout_write.atto" 0)
run_case(expect_ok "${fixtures}/expect_ok.atto" 42)
run_case(expect_err "${fixtures}/expect_err.atto" nonzero)
run_case(expect_guest_write "${fixtures}/expect_guest_write.atto" nonzero)
run_case(import_fnc "${fixtures}/import_fnc/main.atto" 0)
run_case(import_core_fnc "${fixtures}/import_core_fnc.atto" 42)

reject_case(import_private_fnc
  "import fnc hidden from \"./lib\";\nfnc main(): int { return hidden(1); }\n"
  "unknown or private imported function 'hidden'")
reject_case(import_unknown_fnc
  "import fnc missing from \"./lib\";\nfnc main(): int { return missing(1); }\n"
  "unknown or private imported function 'missing'")
reject_case(import_fnc_local_conflict
  "import fnc shown from \"./lib\";\nfnc shown(int x): int { return x; }\nfnc main(): int { return shown(1); }\n"
  "imported function conflicts with local function 'shown'")
reject_case(import_fnc_duplicate
  "import fnc shown from \"./lib\";\nimport fnc shown from \"./lib\";\nfnc main(): int { return shown(1); }\n"
  "duplicate imported function binding 'shown'")
reject_case(import_fnc_list_alias
  "import fnc shown, hidden as both from \"./lib\";\nfnc main(): int { return 0; }\n"
  "named import aliases require exactly one selected name")
reject_case(import_module_after_fnc
  "import fnc shown from \"./lib\";\nimport module shown from \"./lib\";\nfnc main(): int { return 0; }\n"
  "duplicate imported module binding 'shown'")
reject_case(import_fnc_after_module
  "import module lib from \"./lib\";\nimport fnc lib from \"./lib\";\nfnc main(): int { return 0; }\n"
  "duplicate imported function binding 'lib'")
reject_case(import_fnc_empty_braces
  "import fnc {} from \"./lib\";\nfnc main(): int { return 0; }\n"
  "import selection cannot be empty")
reject_case(import_fnc_unclosed_braces
  "import fnc {shown from \"./lib\";\nfnc main(): int { return 0; }\n"
  "expected '}'")
reject_case(import_fnc_parameter_shadow
  "import fnc shown from \"./lib\";\nfnc g(int shown): int { return shown; }\nfnc main(): int { return g(1); }\n"
  "native imported function binding cannot be shadowed: 'shown'")
reject_case(guest_runtime_symbol
  "fnc ores_string_panic(int x): int { return x; }\nfnc main(): void {\n  stdio.println(\"x\");\n  return;\n}\n"
  "reserved native string runtime symbol collision")
reject_case(expect_non_literal
  "fnc main(): int {\n  val Result<int, String> r = Ok(1);\n  val String m = \"x\";\n  return r.expect(m);\n}\n"
  "expect requires a string literal message")
reject_case(stdout_write_two_args
  "fnc main(): void {\n  stdio.stdout.write(\"a\", \"b\");\n  return;\n}\n"
  "stdio.stdout.write takes exactly one argument")
reject_case(stdout_unknown_member
  "fnc main(): void {\n  stdio.stdout.flush(\"a\");\n  return;\n}\n"
  "unsupported native stdio.stdout operation")
reject_case(if_semicolon_requires_then
  "fnc main(): int {\n  if 1 == 1; return 1; fi\n  return 0;\n}\n"
  "expected 'then'")

reject_case(freestanding_relative_fnc_import
  "import fnc shown from \"./lib\";\npub fnc f(int x): int { return shown(x); }\n"
  "freestanding native imports require compiler-owned ores.core/* modules"
  --target=thumbv7m-none-eabi --library-profile=core)
reject_case(freestanding_alloc_fnc_without_provider
  "import fnc sum_pair from \"alloc/int_buffer\";\npub fnc f(int x): int { return sum_pair(x, 1); }\n"
  "requires an explicit allocator provider"
  --target=thumbv7m-none-eabi --library-profile=core)
reject_case(std_fnc_import
  "import fnc check from \"ores.std/testing\";\nfnc main(): int { return 0; }\n"
  "native ores.std package resolution remains unsupported")

if(failures GREATER 0)
  message(FATAL_ERROR "atto parity syntax: ${failures} failure(s)")
endif()
message(STATUS "atto parity syntax: all cases passed")
