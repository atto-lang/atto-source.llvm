# Real checked .atto -> LLVM IR -> native Clang -> ActorRegionV1 runtime.
# Independent CTest: 2 lexical owners, one guarded early return, 2 drops.
# A green runner is a restricted guest result, not process-wide zero GC.
cmake_minimum_required(VERSION 3.20)
if(NOT DEFINED ROOT OR NOT DEFINED BUILD OR NOT DEFINED CLANG OR
   "${CLANG}" STREQUAL "" OR "${CLANG}" MATCHES "-NOTFOUND$")
  message(FATAL_ERROR "actor-region-v2 requires pinned ROOT/BUILD/Clang")
endif()
if(NOT EXISTS "${BUILD}/oreslang-llvmc" OR
   NOT EXISTS "${BUILD}/oreslang-no-gc-ir-audit")
  message(FATAL_ERROR "actor-region-v2 requires real LLVM compiler and no-GC IR auditor")
endif()
set(work "${BUILD}/actor-region-v2-real-guest")
# The per-test workspace is compiler-owned under the configured build
# directory. Never recursively remove an untrusted symlink (especially one
# left by an interrupted or malicious previous run).
if(IS_SYMLINK "${BUILD}" OR IS_SYMLINK "${BUILD}/actor-region-v2-real-guest")
  message(FATAL_ERROR "actor-region-v2 refuses symlinked build/work roots")
endif()
if(NOT IS_DIRECTORY "${BUILD}")
  message(FATAL_ERROR "actor-region-v2 requires an existing configured build")
endif()
file(REMOVE_RECURSE "${work}")
file(MAKE_DIRECTORY "${work}")

set(class_prefix
"define class Pair as\n  pub final int flag: 1;\n  pub final int answer: 42;\nend\npub fnc main(): int {\n")
set(owner_prefix
"  let Pair first = new Pair();\n  let Pair second = new Pair();\n")
set(source_early
"${class_prefix}${owner_prefix}  if (first.flag eq 1) { return first.flag; } fi\n  return second.answer;\n}\n")
set(source_normal
"${class_prefix}${owner_prefix}  if (first.flag eq 2) { return first.flag; } fi\n  return second.answer;\n}\n")
set(source_three
"${class_prefix}${owner_prefix}  let Pair third = new Pair();\n  if (first.flag eq 2) { return first.flag; } fi\n  return third.answer;\n}\n")

set(native_successes 0)
foreach(case IN ITEMS early normal three)
  if(case STREQUAL "early")
    set(source "${source_early}")
    set(expected 1)
    set(expected_owners 2)
  elseif(case STREQUAL "normal")
    set(source "${source_normal}")
    set(expected 42)
    set(expected_owners 2)
  else()
    set(source "${source_three}")
    set(expected 42)
    set(expected_owners 3)
  endif()
  set(dir "${work}/${case}")
  file(MAKE_DIRECTORY "${dir}")
  file(WRITE "${dir}/main.atto" "${source}")
  execute_process(COMMAND "${BUILD}/oreslang-llvmc"
      --actor-region-v2-guest --target=host "${dir}/main.atto"
    RESULT_VARIABLE status OUTPUT_VARIABLE ir ERROR_VARIABLE err TIMEOUT 40)
  if(NOT status STREQUAL "0" OR NOT ir MATCHES "define i32 @main\\(")
    message(SEND_ERROR "${case}: frontend failed to produce verified native guest IR: ${status}, ${err}")
  else()
    string(REGEX MATCHALL "call void @ores_actor_region_v1_compiler_alloc\\(" allocations "${ir}")
    string(REGEX MATCHALL "call void @ores_actor_region_v1_compiler_drop\\(" drops "${ir}")
    list(LENGTH allocations alloc_count)
    list(LENGTH drops drop_count)
    if(NOT alloc_count EQUAL ${expected_owners} OR
       NOT drop_count EQUAL ${expected_owners} OR
       NOT ir MATCHES "shared.owner.cleanup")
      message(SEND_ERROR "${case}: expected ${expected_owners} allocations and drops in one CFG cleanup, got ${alloc_count}/${drop_count}")
    else()
      file(WRITE "${dir}/program.ll" "${ir}")
      execute_process(COMMAND "${BUILD}/oreslang-no-gc-ir-audit"
          "${dir}/program.ll"
        RESULT_VARIABLE screened ERROR_VARIABLE scanerr TIMEOUT 20)
      if(NOT screened STREQUAL "0")
        message(SEND_ERROR "${case}: tracing-GC IR audit failed: ${scanerr}")
      else()
        foreach(opt IN ITEMS 0 2)
          set(binary "${dir}/native-O${opt}")
          execute_process(COMMAND "${CLANG}" "-O${opt}"
              -x ir "${dir}/program.ll"
              -x c -std=c11 "-I${ROOT}/src/main/cpp"
              "${ROOT}/src/main/cpp/oreslang/runtime/ActorRegionV1.c"
              "${ROOT}/src/main/cpp/oreslang/runtime/ActorArena.c"
              -o "${binary}"
            RESULT_VARIABLE linked ERROR_VARIABLE link_err TIMEOUT 80)
          if(NOT linked STREQUAL "0" OR NOT EXISTS "${binary}")
            message(SEND_ERROR "${case}: native linker failed O${opt}: ${linked} ${link_err}")
          else()
            file(SHA256 "${binary}" sha)
            execute_process(COMMAND "${binary}"
              RESULT_VARIABLE executed OUTPUT_VARIABLE stdout
              ERROR_VARIABLE stderr TIMEOUT 25)
            if(NOT executed STREQUAL "${expected}" OR
               NOT stdout STREQUAL "" OR NOT stderr STREQUAL "")
              message(SEND_ERROR "${case} O${opt}: expected ${expected} no-output, got ${executed}, stdout=[${stdout}], stderr=[${stderr}]")
            else()
              math(EXPR native_successes "${native_successes}+1")
              message(STATUS "PASS native ActorRegionV2 ${case} O${opt} status=${executed}, sha256=${sha}")
            endif()
          endif()
        endforeach()
      endif()
    endif()
  endif()
endforeach()

# Explicit rejection of unsupported flow and type surfaces. Invalid source
# is never allowed to produce LLVM IR or a native output artifact.
set(source_ninth_owner
"${class_prefix}${owner_prefix}  let Pair third = new Pair();\n  let Pair fourth = new Pair();\n  let Pair fifth = new Pair();\n  let Pair sixth = new Pair();\n  let Pair seventh = new Pair();\n  let Pair eighth = new Pair();\n  let Pair ninth = new Pair();\n  if (first.flag eq 1) { return first.flag; } fi\n  return second.answer;\n}\n")
set(source_mutable
"define class Pair as\n  pub final mut int flag: 1;\n  pub final int answer: 42;\nend\npub fnc main(): int {\n${owner_prefix}  if (first.flag eq 1) { return first.flag; } fi\n  return second.answer;\n}\n")
set(source_borrow
"${class_prefix}${owner_prefix}  rt borrow first;\n  if (first.flag eq 1) { return first.flag; } fi\n  return second.answer;\n}\n")
set(source_unknown
"${class_prefix}${owner_prefix}  if (first.flag eq 1) { return first.flag; } fi\n  return second.missing;\n}\n")
set(source_extra_root
"define class Pair as\n  pub final int flag: 1;\n  pub final int answer: 42;\nend\nfnc extra(): int { return 7; }\npub fnc main(): int {\n${owner_prefix}  if (first.flag eq 1) { return first.flag; } fi\n  return second.answer;\n}\n")
set(source_dynamic_call
"${class_prefix}${owner_prefix}  if (first.flag eq 1) { return extra(); } fi\n  return second.answer;\n}\n")
set(source_two_guards
"${class_prefix}${owner_prefix}  if (first.flag eq 1) { return first.flag; } fi\n  if (second.flag eq 1) { return second.flag; } fi\n  return second.answer;\n}\n")
set(source_no_guard
"${class_prefix}${owner_prefix}  return second.answer;\n}\n")
set(source_nonliteral
"define class Pair as\n  pub final int flag: 1 + 1;\n  pub final int answer: 42;\nend\npub fnc main(): int {\n${owner_prefix}  if (first.flag eq 1) { return first.flag; } fi\n  return second.answer;\n}\n")
set(source_exit_truncation
"define class Pair as\n  pub final int flag: 1;\n  pub final int answer: 258;\nend\npub fnc main(): int {\n${owner_prefix}  if (first.flag eq 1) { return first.answer; } fi\n  return second.answer;\n}\n")
set(source_wrong_profile "${source_early}")
set(negatives 0)
foreach(case IN ITEMS ninth_owner mutable borrow unknown extra_root
                 dynamic_call two_guards no_guard nonliteral
                 exit_truncation)
  set(dir "${work}/rejected-${case}")
  file(MAKE_DIRECTORY "${dir}")
  file(WRITE "${dir}/main.atto" "${source_${case}}")
  execute_process(COMMAND "${BUILD}/oreslang-llvmc"
      --actor-region-v2-guest "${dir}/main.atto"
    RESULT_VARIABLE denied OUTPUT_VARIABLE ir ERROR_VARIABLE message TIMEOUT 25)
  file(GLOB artifacts "${dir}/*.ll" "${dir}/*.o" "${dir}/*.exe")
  if(denied STREQUAL "0" OR NOT ir STREQUAL "" OR artifacts)
    message(SEND_ERROR "${case}: unsupported source emitted guest IR or artifact")
  else()
    math(EXPR negatives "${negatives}+1")
    message(STATUS "PASS fail-closed v2 guest negative ${case}")
  endif()
endforeach()
file(MAKE_DIRECTORY "${work}/rejected-profile")
file(WRITE "${work}/rejected-profile/main.atto" "${source_wrong_profile}")
execute_process(COMMAND "${BUILD}/oreslang-llvmc"
    --actor-region-v1-guest --actor-region-v2-guest
    "${work}/rejected-profile/main.atto"
  RESULT_VARIABLE denied OUTPUT_VARIABLE ir ERROR_VARIABLE message TIMEOUT 25)
if(denied STREQUAL "0" OR NOT ir STREQUAL "")
  message(SEND_ERROR "combining v1 and v2 AOT modes silently succeeded")
else()
  math(EXPR negatives "${negatives}+1")
endif()
if(NOT native_successes EQUAL 6 OR NOT negatives EQUAL 11)
  message(FATAL_ERROR "ActorRegionV2 native guest incomplete: ${native_successes}/6 native executions (2/3 owners at O0/O2), ${negatives}/11 rejections")
endif()
message(STATUS "PASS ActorRegionV2: 6 compiled native guest executions (2/3 owners at O0/O2) + 11 rejected source/profile cases")
