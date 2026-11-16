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
  if(expected_exit STREQUAL "nonzero")
    if(status EQUAL 0)
      fail_case("${name}: expected a nonzero (panic) exit, got 0")
    endif()
  elseif(NOT status STREQUAL expected_exit)
    fail_case("${name}: exit ${status}, expected ${expected_exit}; stderr: ${stderr}")
  endif()
  set(failures ${failures} PARENT_SCOPE)
  message(STATUS "PASS ${name}")
endfunction()

# Negative case: compiler must reject `text` with a diagnostic containing `needle`.
function(reject_case name text needle)
  set(dir "${work}/${name}")
  file(MAKE_DIRECTORY "${dir}")
  file(WRITE "${dir}/main.atto" "${text}")
  file(WRITE "${dir}/lib.atto"
    "pub fnc shown(int x): int { return x; }\nfnc hidden(int x): int { return x; }\n")
  execute_process(COMMAND "${BUILD}/oreslang-llvmc" "${dir}/main.atto"
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

run_case(end_scalar_newline "${fixtures}/end_scalar_newline.atto" 42)
run_case(end_scalar_marker "${fixtures}/end_scalar_marker.atto" 42)
run_case(if_then "${fixtures}/if_then.atto" 0)
run_case(stdout_write "${fixtures}/stdout_write.atto" 0)
run_case(expect_ok "${fixtures}/expect_ok.atto" 42)
run_case(expect_err "${fixtures}/expect_err.atto" nonzero)
run_case(import_fnc "${fixtures}/import_fnc/main.atto" 0)
run_case(attolang_equality "${fixtures}/attolang_equality.atto" 0)
# Real compiler + LLVM IR + clang + process exit status, not parser-only fixtures.
run_case(comma_body_fnc_routine "${fixtures}/comma_body_fncs.atto" 42)
run_case(comma_body_mtd "${fixtures}/comma_body_methods.atto" 42)
run_case(comma_body_named_pipe "${fixtures}/comma_body_named_pipe.atto" 42)


# Strict native scalar equality enforces identical operand types. The
# accepted aliases are all lowered to static comparison instructions.
reject_case(comma_body_trailing
  "fnc main() -> int :: int x = 5, end\n"
  "requires item after ','")
reject_case(comma_body_empty
  "fnc main() -> int :: int x = 5,, x end\n"
  "empty item")

# The native scalar compiler has no checked lambda closure/tuple stage ABI.
# Lexical rejection is intentional until this gets real LLVM guest lowering.
reject_case(comma_lambda_not_lowered
  "fnc main() -> int :: val int x = 5, x |> |n| => int -> n + 1 end\n"
  "unsupported Oreslang token")
reject_case(typed_inline_lambda_not_lowered
  "fnc main(): int { val int f = || => int -> 42; return f(); }\n"
  "NATIVE_LAMBDA_CLOSURE_UNSUPPORTED")

reject_case(equality_alias_mixed_bool_int
  "fnc main(): int { if 7 !eq true then return 1; else return 0; fi }\n"
  "equality operands require the same scalar type")
reject_case(equality_keyword_mixed_bool_int
  "fnc main(): int { if 7 neq true then return 1; else return 0; fi }\n"
  "equality operands require the same scalar type")
reject_case(equality_alias_split
  "fnc main(): int { if 7 ! eq 8 then return 1; else return 0; fi }\n"
  "expected")

reject_case(end_scalar_compact_missing_delimiter
  "fnc add_one(int x) -> int return x + 1; end\nfnc main(): int { return add_one(41); }\n"
  "native compact end-delimited callable requires '::'")
reject_case(end_scalar_reject_unlowered_if
  "fnc add_one(int x) -> int :: if x > 0 then return x + 1; fi end\nfnc main(): int { return add_one(41); }\n"
  "expected int expression")
reject_case(end_scalar_reject_missing_end
  "fnc add_one(int x) -> int :: return x + 1;\n"
  "native comma expression body requires a single terminal result")
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


# Strict closed-world AOT regression lane. These are real oreslang-llvmc
# compiler rejections, not a source-text grep or host-only reference model.
# The JS/GraalVM/NativeAOT runtime profiles are validated separately.
# Use broad diagnostic-presence here: exact message strings may legitimately
# differ as the typed frontend becomes more precise, but the compiler must
# NEVER emit executable LLVM IR or silently accept runtime code generation.
function(reject_closed_world_case name text)
  set(dir "${work}/closed-world-${name}")
  file(MAKE_DIRECTORY "${dir}")
  file(WRITE "${dir}/main.atto" "${text}")
  execute_process(
    COMMAND "${BUILD}/oreslang-llvmc" "${dir}/main.atto"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
    WORKING_DIRECTORY "${dir}"
    TIMEOUT 20)
  if(NOT status MATCHES "^[0-9]+$" OR status EQUAL 0)
    fail_case("${name}: closed-world rejection missing (compiler status ${status})")
  elseif(NOT stdout STREQUAL "")
    fail_case("${name}: invalid dynamic source emitted LLVM IR on stdout")
  elseif(stderr STREQUAL "")
    fail_case("${name}: rejection had no diagnostic on stderr")
  else()
    # Any accidental emitted LLVM/native sidecar must be inside the isolated
    # working directory and must cause a negative compilation test failure.
    file(GLOB emitted "${dir}/*.ll" "${dir}/*.bc" "${dir}/*.o"
      "${dir}/*.obj" "${dir}/*.s" "${dir}/*.exe")
    if(emitted)
      fail_case("${name}: invalid source emitted a code artifact: ${emitted}")
    else()
      message(STATUS "PASS closed-world ${name}")
    endif()
  endif()
  set(failures ${failures} PARENT_SCOPE)
endfunction()

reject_closed_world_case(runtime_eval
  "fnc main(): int { return eval(42); }\n")
reject_closed_world_case(runtime_function_factory
  "fnc main(): int { return Function(42); }\n")
reject_closed_world_case(unknown_runtime_class
  "fnc main(): int { return new RuntimeProxy(); }\n")
reject_closed_world_case(unregistered_reflection
  "fnc main(): int { return Reflect.get(1); }\n")
reject_closed_world_case(runtime_nested_class
  "fnc main(): int { define class Mutable as end; return 0; }\n")

if(failures GREATER 0)
  message(FATAL_ERROR "atto parity syntax: ${failures} failure(s)")
endif()
message(STATUS "atto parity syntax: all cases passed")
