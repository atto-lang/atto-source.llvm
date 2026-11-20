# Experimental scalar-loop frame integrity gate (forward-port of #1570's
# hardening onto #1569's batched step lowering).
#
#  1. Compile the opt-in fixture with the REAL compiler and check the emitted
#     IR keeps #1569's batch endpoint (no per-backedge fuel test) plus the
#     frame-integrity guards (all-zero init, counter range, exact DONE).
#  2. Link the generated module with the C frame oracle at clang -O0/-O2/-O3
#     and require every receipt, including the child-signal trap suite on
#     hosts with fork/waitpid (Linux, macOS).
#  3. Re-run under sanitizers wherever a toolchain can build AND run a
#     sanitized probe: ASan+UBSan, else UBSan alone, with CLANG; ASan+UBSan
#     with SAN_CLANG when given (e.g. a vendor clang whose ASan runtime works).
#  4. Mutate the compiler-emitted IR (fuel-cap bypass, double commit, stale
#     persisted counter, CANCELLED disguised as DONE, and guard removals).
#     Every mutant must still compile and must be killed by the native oracle
#     ("frame check failed at ..." naming the intended check), never by a
#     parser error or an unrelated crash.
#
# Required: -DCOMPILER -DCLANG -DFIXTURE -DHARNESS -DWORK
# Optional: -DSANITIZERS=AUTO|ON|OFF (default AUTO; ON = at least one
#           sanitizer configuration must run), -DSAN_CLANG=<clang>
cmake_minimum_required(VERSION 3.20)

foreach(var COMPILER CLANG FIXTURE HARNESS WORK)
  if(NOT DEFINED ${var} OR "${${var}}" STREQUAL "")
    message(FATAL_ERROR "scalar frame integrity: -D${var}=... is required")
  endif()
endforeach()
foreach(path COMPILER CLANG FIXTURE HARNESS)
  if(NOT EXISTS "${${path}}")
    message(FATAL_ERROR "scalar frame integrity: ${path} does not exist: ${${path}}")
  endif()
endforeach()
if(NOT DEFINED SANITIZERS)
  set(SANITIZERS AUTO)
endif()
get_filename_component(work_parent "${WORK}" DIRECTORY)
if(NOT IS_DIRECTORY "${work_parent}" OR IS_SYMLINK "${WORK}" OR
   "${WORK}" STREQUAL "/" OR "${WORK}" STREQUAL "${work_parent}")
  message(FATAL_ERROR "scalar frame integrity: refusing unsafe work directory ${WORK}")
endif()
file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}")

set(failures 0)
macro(fail_case reason)
  message(SEND_ERROR "${reason}")
  math(EXPR failures "${failures}+1")
endmacro()

# The child-signal trap proof needs POSIX fork/waitpid (see the harness).
set(trap_proof OFF)
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux" OR CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
  set(trap_proof ON)
endif()
set(receipts
  SCALAR_FRAME_AOT_TWO_TASKS_PASS
  SCALAR_FRAME_AOT_THREE_PEERS_BOUNDED_PROGRESS_PASS
  SCALAR_FRAME_AOT_CANCELLED_PEER_ISOLATION_PASS
  SCALAR_FRAME_AOT_BATCH_FUEL_BOUNDARIES_PASS
  SCALAR_FRAME_AOT_TERMINAL_RETIREMENT_PASS)
if(trap_proof)
  list(APPEND receipts SCALAR_FRAME_AOT_INVALID_FRAME_TRAPS_20_PASS)
endif()
set(warn_flags -std=c11 -DNDEBUG -Wall -Wextra -Werror -pedantic
  -Wno-error=override-module -Wno-override-module)

# ---- 1. compiler-emitted IR -------------------------------------------------
execute_process(COMMAND "${COMPILER}" --experimental-rt-scalar-loop "${FIXTURE}"
  RESULT_VARIABLE rc OUTPUT_VARIABLE ir ERROR_VARIABLE err TIMEOUT 60)
if(NOT rc STREQUAL "0")
  message(FATAL_ERROR "scalar frame integrity: compiler rejected fixture (${rc}): ${err}")
endif()
file(WRITE "${WORK}/guest.ll" "${ir}")
foreach(needle
    "define i32 @oreslang_rt_scalar_frame_step"
    "define void @oreslang_rt_scalar_frame_init"
    "define void @oreslang_rt_scalar_frame_cancel"
    "define i64 @oreslang_rt_scalar_frame_result"
    # #1569: endpoint computed once per dispatch; the backedge compares the
    # SSA induction value against it and nothing else.
    "%dispatch.batch.stop = select"
    "icmp slt i64 %counter.before, %dispatch.batch.stop"
    "loop.batch.exit:"
    # ported #1570 integrity guards
    "%frame.all.zero = and i1"
    "counter.valid:"
    "%frame.done.exact = or i1"
    "frame.state.consistent:"
    "%result.exact = icmp eq i64 %result.counter")
  string(FIND "${ir}" "${needle}" at)
  if(at EQUAL -1)
    fail_case("emitted IR is missing `${needle}`")
  endif()
endforeach()
foreach(banned "loop.fuel" "fuel.remaining" "dispatch.fuel")
  string(FIND "${ir}" "${banned}" at)
  if(NOT at EQUAL -1)
    fail_case("per-backedge fuel accounting regressed: `${banned}` in emitted IR")
  endif()
endforeach()

function(build_guest cc out ir_file)
  execute_process(COMMAND "${cc}" ${warn_flags} ${ARGN} "${HARNESS}"
      -x ir "${ir_file}" -o "${out}"
    RESULT_VARIABLE brc OUTPUT_VARIABLE bout ERROR_VARIABLE berr TIMEOUT 120)
  set(build_rc "${brc}" PARENT_SCOPE)
  set(build_err "${bout}${berr}" PARENT_SCOPE)
endfunction()

function(run_guest exe)
  execute_process(COMMAND "${exe}" RESULT_VARIABLE rrc
    OUTPUT_VARIABLE rout ERROR_VARIABLE rerr TIMEOUT 120)
  set(run_rc "${rrc}" PARENT_SCOPE)
  set(run_out "${rout}" PARENT_SCOPE)
  set(run_err "${rerr}" PARENT_SCOPE)
endfunction()

macro(require_receipts label)
  foreach(receipt IN LISTS receipts)
    string(FIND "${run_out}" "${receipt}" at)
    if(at EQUAL -1)
      fail_case("${label}: missing receipt ${receipt}\n${run_out}${run_err}")
    endif()
  endforeach()
endmacro()

# ---- 2. O0/O2/O3 matrix ------------------------------------------------------
foreach(opt O0 O2 O3)
  build_guest("${CLANG}" "${WORK}/guest-${opt}" "${WORK}/guest.ll" -${opt})
  if(NOT build_rc STREQUAL "0")
    fail_case("-${opt}: generated guest did not link: ${build_err}")
    continue()
  endif()
  run_guest("${WORK}/guest-${opt}")
  if(NOT run_rc STREQUAL "0")
    fail_case("-${opt}: generated guest failed (${run_rc}): ${run_err}")
  else()
    require_receipts("-${opt}")
    message(STATUS "PASS scalar frame guest -${opt}")
  endif()
endforeach()

# ---- 3. sanitizers where supported ------------------------------------------
# A probe must build AND run within 20s: some toolchain/OS pairs link an ASan
# runtime that hangs at startup, which is "unsupported", not a guest failure.
function(sanitizer_probe cc sanitizers tag)
  file(WRITE "${WORK}/san-probe.c" "int main(void) { return 0; }\n")
  execute_process(COMMAND "${cc}" -O1 -fsanitize=${sanitizers}
      "${WORK}/san-probe.c" -o "${WORK}/san-probe-${tag}"
    RESULT_VARIABLE prc OUTPUT_QUIET ERROR_QUIET TIMEOUT 60)
  if(prc STREQUAL "0")
    execute_process(COMMAND "${WORK}/san-probe-${tag}" RESULT_VARIABLE prc
      OUTPUT_QUIET ERROR_QUIET TIMEOUT 20)
  endif()
  if(prc STREQUAL "0")
    set(probe_ok ON PARENT_SCOPE)
  else()
    set(probe_ok OFF PARENT_SCOPE)
  endif()
endfunction()

set(san_runs 0)
macro(sanitized_matrix cc sanitizers tag)
  foreach(opt O1 O2)
    build_guest("${cc}" "${WORK}/guest-${tag}-${opt}" "${WORK}/guest.ll" -${opt}
      -fsanitize=${sanitizers} -fno-sanitize-recover=all -fno-omit-frame-pointer)
    if(NOT build_rc STREQUAL "0")
      fail_case("${tag} -${opt}: guest did not link: ${build_err}")
      continue()
    endif()
    run_guest("${WORK}/guest-${tag}-${opt}")
    if(NOT run_rc STREQUAL "0")
      fail_case("${tag} -${opt}: guest failed (${run_rc}): ${run_err}")
    else()
      require_receipts("${tag} -${opt}")
      math(EXPR san_runs "${san_runs}+1")
      message(STATUS "PASS scalar frame guest ${tag} -${opt} (${cc})")
    endif()
  endforeach()
endmacro()

if(NOT SANITIZERS STREQUAL "OFF")
  sanitizer_probe("${CLANG}" "address,undefined" clang-asan-ubsan)
  if(probe_ok)
    sanitized_matrix("${CLANG}" "address,undefined" asan-ubsan)
  else()
    message(STATUS "SKIP ASan+UBSan with ${CLANG}: sanitized probe did not build/run")
    sanitizer_probe("${CLANG}" "undefined" clang-ubsan)
    if(probe_ok)
      sanitized_matrix("${CLANG}" "undefined" ubsan)
    else()
      message(STATUS "SKIP UBSan with ${CLANG}: sanitized probe did not build/run")
    endif()
  endif()
  if(DEFINED SAN_CLANG AND NOT "${SAN_CLANG}" STREQUAL "" AND
     NOT "${SAN_CLANG}" STREQUAL "${CLANG}")
    sanitizer_probe("${SAN_CLANG}" "address,undefined" san-clang-asan-ubsan)
    if(probe_ok)
      sanitized_matrix("${SAN_CLANG}" "address,undefined" san-clang-asan-ubsan)
    else()
      message(STATUS "SKIP ASan+UBSan with ${SAN_CLANG}: sanitized probe did not build/run")
    endif()
  endif()
  if(SANITIZERS STREQUAL "ON" AND san_runs EQUAL 0)
    fail_case("SANITIZERS=ON but no sanitizer configuration could run")
  endif()
endif()

# ---- 4. adversarial mutations of the compiler-emitted IR ---------------------
# mutate(label expected_sites pattern replacement oracle_needle)
# A mutant is killed only if it compiles, exits non-zero, the C oracle reports
# `frame check failed at`, that first failed check contains oracle_needle, and
# no success receipt was printed.
function(mutate label expected pattern replacement needle)
  set(ok OFF PARENT_SCOPE)
  string(REGEX MATCHALL "${pattern}" hits "${ir}")
  list(LENGTH hits n)
  if(NOT n EQUAL expected)
    message(SEND_ERROR "mutant ${label}: expected ${expected} injection site(s), found ${n}")
    return()
  endif()
  string(REGEX REPLACE "${pattern}" "${replacement}" changed "${ir}")
  if(changed STREQUAL ir)
    message(SEND_ERROR "mutant ${label}: replacement did not change the module")
    return()
  endif()
  file(WRITE "${WORK}/mutant-${label}.ll" "${changed}")
  build_guest("${CLANG}" "${WORK}/mutant-${label}" "${WORK}/mutant-${label}.ll" -O2)
  if(NOT build_rc STREQUAL "0")
    message(SEND_ERROR "mutant ${label}: mutated IR must compile (a parser rejection is not a kill): ${build_err}")
    return()
  endif()
  run_guest("${WORK}/mutant-${label}")
  string(FIND "${run_out}" "SCALAR_FRAME_AOT_TWO_TASKS_PASS" receipt)
  string(REGEX MATCH "frame check failed at [^\n]*" first_failure "${run_err}")
  if(first_failure STREQUAL "")
    set(reason -1)
  else()
    string(FIND "${first_failure}" "${needle}" reason)
  endif()
  if(run_rc STREQUAL "0" OR NOT receipt EQUAL -1 OR reason EQUAL -1)
    message(SEND_ERROR "mutant ${label} SURVIVED or died for the wrong reason "
      "(rc=${run_rc}, want a failed check containing `${needle}`): ${run_out}${run_err}")
    return()
  endif()
  message(STATUS "KILLED mutant ${label}: ${first_failure}")
  set(ok ON PARENT_SCOPE)
endfunction()

set(mutants 0)
set(killed 0)
# A function (not a macro) so regex backslashes reach mutate() verbatim.
function(expect_killed label expected pattern replacement needle)
  math(EXPR m "${mutants}+1")
  set(mutants "${m}" PARENT_SCOPE)
  mutate("${label}" "${expected}" "${pattern}" "${replacement}" "${needle}")
  if(ok)
    math(EXPR k "${killed}+1")
    set(killed "${k}" PARENT_SCOPE)
  else()
    math(EXPR f "${failures}+1")
    set(failures "${f}" PARENT_SCOPE)
  endif()
endfunction()

# #1570's four semantic mutants, re-targeted at #1569's IR shape.
# (a) fuel cap bypass: batch endpoint := source upper bound.
expect_killed(bypass-batch-fuel-cap 1
  "%dispatch\\.batch\\.stop = select i1 %[0-9A-Za-z._]+, i64 %dispatch\\.end\\.by\\.fuel, i64 (-?[0-9]+)"
  "%dispatch.batch.stop = add i64 0, \\1"
  "oreslang_rt_scalar_frame_step(a, 0) == 1")
# (b) non-exactly-once commit: each backedge commits +2.
expect_killed(double-commit 1
  "%counter\\.committed = add nsw i64 %counter\\.before, 1\n"
  "%counter.committed = add nsw i64 %counter.before, 2\n"
  "a->counter == 1")
# (c) lost continuation: persist the previous counter, not the committed one.
expect_killed(store-previous-counter 1
  "store i64 %counter\\.committed, "
  "store i64 %counter.before, "
  "a->counter == 1")
# (d) terminal forgery: CANCELLED status disguised as DONE.
expect_killed(cancelled-disguised-as-done 1
  "ret i32 2\n"
  "ret i32 0\n"
  "oreslang_rt_scalar_frame_step(cancelled, 512) == 2")
# Guard removals: prove the trap suite is a live oracle for each ported guard.
if(trap_proof)
  expect_killed(accept-dirty-init 1
    "%frame\\.all\\.zero = and i1 %([0-9A-Za-z._]+), %[0-9A-Za-z._]+\n"
    "%frame.all.zero = and i1 %\\1, true\n"
    "WIFSIGNALED(status)")
  expect_killed(accept-forged-done 2
    "%frame\\.done\\.exact = or i1 %([0-9A-Za-z._]+), %[0-9A-Za-z._]+\n"
    "%frame.done.exact = or i1 %\\1, true\n"
    "WIFSIGNALED(status)")
  expect_killed(publish-inexact-result 1
    "%result\\.exact = icmp eq i64 %result\\.counter, -?[0-9]+\n"
    "%result.exact = icmp eq i64 %result.counter, %result.counter\n"
    "WIFSIGNALED(status)")
endif()

if(failures GREATER 0)
  message(FATAL_ERROR "scalar frame integrity: ${failures} failure(s); mutants killed ${killed}/${mutants}")
endif()
message(STATUS "SCALAR_FRAME_INTEGRITY_AOT_PASS: O0/O2/O3 receipts, trap_proof=${trap_proof}, "
  "sanitizer runs ${san_runs}, mutants killed ${killed}/${mutants}")
