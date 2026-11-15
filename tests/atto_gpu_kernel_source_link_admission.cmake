# Compiler-level GPU declaration + launch source-link negative admission.
# This CTest invokes the REAL oreslang-llvmc binary, not a synthetic parser.
# Every input must fail safely until typed host values/device codegen exists.
if(NOT DEFINED COMPILER OR NOT DEFINED WORKDIR)
  message(FATAL_ERROR "COMPILER and WORKDIR required")
endif()
file(MAKE_DIRECTORY "${WORKDIR}")
set(kernel
  "pub gpu fnc saxpy(f32 alpha, GpuReadBuffer<f32> x, GpuReadBuffer<f32> y, GpuWriteBuffer<f32> out, u64 n): void { const u64 i = gpu.global_id(0); if i < n { out[i] = alpha * x[i] + y[i]; } }\n")
set(legacy
  "pub gpu fnc saxpy(f32 alpha, GpuArray<f32> x, GpuArray<f32> y, GpuArray<f32> mut out, u64 n) => void { return; }\n")
set(launch
  "pub fnc main(): int { return launch saxpy(alpha,x,y,out,n) on gpu(device) { over: [n], group: [256], queue: queue }; }\n")
set(cases "gpu-only" "source-linked" "array-mutable" "unknown-kernel" "wrong-arity"
          "duplicate-kernel" "duplicate-param" "forbidden-await" "invalid-type"
          "nonvoid" "missing-body" "read-mut" "wrong-scalar-name" "signed-int"
          "string-body" "template-body" "defer-body"
          "throw-body"
          "raise-body"
          "panic-body"
          "async-body"
          "nb-body"
          "using-body"
          "console-body"
          "sleep-body"
          "select-body"
          "readch-body"
          "writech-body"
          "spawn-body"
          "nested-braces"
          "comment-braces"
          "opaque-host-call"
          "saxpy-typed-int"
          "saxpy-early-return"
          "saxpy-unguarded"
          "saxpy-readonly-store"
          "saxpy-read-from-output"
          "saxpy-wrong-index"
          "saxpy-invalid-axis"
          "saxpy-signedness"
          "saxpy-wrong-operator"
          "saxpy-early-no-return"
          "saxpy-wrong-bound"
          "saxpy-leak-store")
set(source_gpu-only "${kernel}")
set(source_source-linked "${kernel}${launch}")
set(source_array-mutable "${legacy}${launch}")
set(source_unknown-kernel "${kernel}pub fnc main(): int { return launch wrong(alpha,x,y,out,n) on gpu(device) { over: [n], group: [256], queue: queue }; }\n")
set(source_wrong-arity "${kernel}pub fnc main(): int { return launch saxpy(alpha,x) on gpu(device) { over: [n], group: [256], queue: queue }; }\n")
set(source_duplicate-kernel "${kernel}${kernel}")
set(source_duplicate-param "pub gpu fnc saxpy(f32 alpha, f32 alpha): void { return; }\n")
set(source_forbidden-await "pub gpu fnc saxpy(f32 alpha): void { await completion; }\n")
set(source_invalid-type "pub gpu fnc saxpy(bool n): void { return; }\n")
set(source_nonvoid "pub gpu fnc saxpy(f32 alpha): int { return 1; }\n")
set(source_missing-body "pub gpu fnc saxpy(f32 alpha): void {\n")
set(source_read-mut "pub gpu fnc saxpy(GpuReadBuffer<f32> mut input): void { return; }\n")
set(source_wrong-scalar-name "pub gpu fnc saxpy(f32 gpu): void { return; }\n")
set(source_signed-int "pub gpu fnc saxpy(int n): void { return; }\n")
set(source_string-body "pub gpu fnc saxpy(): void { const val = \"}\"; return; }\n")
set(source_template-body "pub gpu fnc saxpy(): void { const val = `hello`; return; }\n")
set(source_defer-body "pub gpu fnc saxpy(): void { defer cleanup(); }\n")
set(source_throw-body "pub gpu fnc saxpy(): void { throw error; }\n")
set(source_raise-body "pub gpu fnc saxpy(): void { raise error; }\n")
set(source_panic-body "pub gpu fnc saxpy(): void { panic error; }\n")
set(source_async-body "pub gpu fnc saxpy(): void { async fnc f() {} }\n")
set(source_nb-body "pub gpu fnc saxpy(): void { nb select first; }\n")
set(source_using-body "pub gpu fnc saxpy(): void { using device; }\n")
set(source_console-body "pub gpu fnc saxpy(): void { console.log(); }\n")
set(source_sleep-body "pub gpu fnc saxpy(): void { sleep(10); }\n")
set(source_select-body "pub gpu fnc saxpy(): void { select first; }\n")
set(source_readch-body "pub gpu fnc saxpy(): void { readch c; }\n")
set(source_writech-body "pub gpu fnc saxpy(): void { writech c; }\n")
set(source_spawn-body "pub gpu fnc saxpy(): void { spawn worker; }\n")
set(source_nested-braces "pub gpu fnc saxpy(): void { if true { if true { return; } } }\n")
set(source_comment-braces "pub gpu fnc saxpy(): void { /* } } } */ return; }\n")
set(source_opaque-host-call "pub gpu fnc saxpy(): void { unresolved_host_function(); }\n")
set(typed_saxpy_prefix "pub gpu fnc saxpy(f32 alpha, GpuReadBuffer<f32> x, GpuReadBuffer<f32> y, GpuWriteBuffer<f32> out, int n): void { ")
set(source_saxpy-typed-int "${typed_saxpy_prefix}const int i = gpu.global_id(0); if i < n { out[i] = alpha * x[i] + y[i]; } fi }\n")
set(source_saxpy-early-return "${typed_saxpy_prefix}const int i = gpu.global_id(0); if i >= n { return; } out[i] = alpha * x[i] + y[i]; return; }\n")
set(source_saxpy-unguarded "${typed_saxpy_prefix}const int i = gpu.global_id(0); out[i] = alpha * x[i] + y[i]; }\n")
set(source_saxpy-readonly-store "${typed_saxpy_prefix}const int i = gpu.global_id(0); if i < n { x[i] = alpha * x[i] + y[i]; } }\n")
set(source_saxpy-read-from-output "${typed_saxpy_prefix}const int i = gpu.global_id(0); if i < n { out[i] = alpha * out[i] + y[i]; } }\n")
set(source_saxpy-wrong-index "${typed_saxpy_prefix}const int i = gpu.global_id(0); if i < n { out[n] = alpha * x[i] + y[i]; } }\n")
set(source_saxpy-invalid-axis "${typed_saxpy_prefix}const int i = gpu.global_id(1); if i < n { out[i] = alpha * x[i] + y[i]; } }\n")
set(source_saxpy-signedness "${typed_saxpy_prefix}const u64 i = gpu.global_id(0); if i < n { out[i] = alpha * x[i] + y[i]; } }\n")
set(source_saxpy-wrong-operator "${typed_saxpy_prefix}const int i = gpu.global_id(0); if i < n { out[i] = alpha + x[i] + y[i]; } }\n")
set(source_saxpy-early-no-return "${typed_saxpy_prefix}const int i = gpu.global_id(0); if i >= n {} out[i] = alpha * x[i] + y[i]; }\n")
set(source_saxpy-wrong-bound "${typed_saxpy_prefix}const int i = gpu.global_id(0); if i < other { out[i] = alpha * x[i] + y[i]; } }\n")
set(source_saxpy-leak-store "${typed_saxpy_prefix}const int i = gpu.global_id(0); if i < n { out[i] = alpha * x[i] + y[i]; } out[i] = alpha * x[i] + y[i]; }\n")
set(expected_gpu-only "GPU-LAUNCH-008")
set(expected_source-linked "GPU-LAUNCH-008")
set(expected_array-mutable "GPU-LAUNCH-008")
set(expected_unknown-kernel "GPU-LAUNCH-001")
set(expected_wrong-arity "GPU-LAUNCH-002")
set(expected_duplicate-kernel "GPU-LAUNCH-001")
set(expected_duplicate-param "GPU-LAUNCH-001")
set(expected_forbidden-await "GPU-LAUNCH-006")
set(expected_invalid-type "GPU-LAUNCH-002")
set(expected_nonvoid "GPU-LAUNCH-001")
set(expected_missing-body "GPU-LAUNCH-001")
set(expected_read-mut "GPU-LAUNCH-002")
set(expected_wrong-scalar-name "GPU-LAUNCH-001")
set(expected_signed-int "GPU-LAUNCH-008")
set(expected_string-body "GPU-LAUNCH-006")
set(expected_template-body "GPU-LAUNCH-006")
set(expected_defer-body "GPU-LAUNCH-006")
set(expected_throw-body "GPU-LAUNCH-006")
set(expected_raise-body "GPU-LAUNCH-006")
set(expected_panic-body "GPU-LAUNCH-006")
set(expected_async-body "GPU-LAUNCH-006")
set(expected_nb-body "GPU-LAUNCH-006")
set(expected_using-body "GPU-LAUNCH-006")
set(expected_console-body "GPU-LAUNCH-006")
set(expected_sleep-body "GPU-LAUNCH-006")
set(expected_select-body "GPU-LAUNCH-006")
set(expected_readch-body "GPU-LAUNCH-006")
set(expected_writech-body "GPU-LAUNCH-006")
set(expected_spawn-body "GPU-LAUNCH-006")
set(expected_nested-braces "GPU-LAUNCH-008")
set(expected_comment-braces "GPU-LAUNCH-008")
set(expected_opaque-host-call "GPU-LAUNCH-008")

set(expected_saxpy-typed-int "GPU-LAUNCH-008")
set(expected_saxpy-early-return "GPU-LAUNCH-008")
set(expected_saxpy-unguarded "GPU-LAUNCH-005")
set(expected_saxpy-readonly-store "GPU-LAUNCH-005")
set(expected_saxpy-read-from-output "GPU-LAUNCH-005")
set(expected_saxpy-wrong-index "GPU-LAUNCH-005")
set(expected_saxpy-invalid-axis "GPU-LAUNCH-003")
set(expected_saxpy-signedness "GPU-LAUNCH-002")
set(expected_saxpy-wrong-operator "GPU-LAUNCH-002")
set(expected_saxpy-early-no-return "GPU-LAUNCH-005")
set(expected_saxpy-wrong-bound "GPU-LAUNCH-005")
set(expected_saxpy-leak-store "GPU-LAUNCH-008")
foreach(case_name IN LISTS cases)
  string(RANDOM LENGTH 24 ALPHABET "0123456789abcdef" nonce)
  set(case_dir "${WORKDIR}/kernel-link-${case_name}-${nonce}")
  if(EXISTS "${case_dir}")
    message(FATAL_ERROR "GPU kernel source-link workspace collision")
  endif()
  file(MAKE_DIRECTORY "${case_dir}")
  set(input "${case_dir}/${case_name}.atto")
  file(WRITE "${input}" "${source_${case_name}}")
  file(SHA256 "${input}" before_hash)
  file(GLOB_RECURSE before LIST_DIRECTORIES true RELATIVE "${case_dir}"
       "${case_dir}/*" "${case_dir}/.*")
  execute_process(COMMAND "${COMPILER}" "${input}"
    WORKING_DIRECTORY "${case_dir}"
    RESULT_VARIABLE status OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr
    TIMEOUT 15)
  if(NOT "${status}" MATCHES "^[1-9][0-9]*$")
    message(FATAL_ERROR "${case_name}: numeric nonzero rejection exit required, got '${status}'")
  endif()
  if(NOT "${stderr}" MATCHES "(^|[\\r\\n])oreslang-llvmc: ${expected_${case_name}}:")
    message(FATAL_ERROR "${case_name}: missing exact native diagnostic '${expected_${case_name}}': ${stderr}")
  endif()
  if(NOT "${stderr}" MATCHES "no implicit CPU fallback")
    message(FATAL_ERROR "${case_name}: no-fallback guarantee missing")
  endif()
  if(NOT "${stdout}" STREQUAL "")
    message(FATAL_ERROR "${case_name}: rejected source emitted stdout/IR")
  endif()
  file(SHA256 "${input}" after_hash)
  if(NOT "${before_hash}" STREQUAL "${after_hash}")
    message(FATAL_ERROR "${case_name}: input mutated by compiler")
  endif()
  file(GLOB_RECURSE after LIST_DIRECTORIES true RELATIVE "${case_dir}"
       "${case_dir}/*" "${case_dir}/.*")
  if(NOT "${before}" STREQUAL "${after}")
    message(FATAL_ERROR "${case_name}: rejected source created hidden or nested artifacts")
  endif()
endforeach()
message(STATUS "PASS 44 native source-link compiler negative admissions; physical_gpu_executed=false")
