cmake_minimum_required(VERSION 3.20)
project(tenant_vm LANGUAGES C CXX)
include(CTest)
find_package(Threads REQUIRED)

option(TENANT_VM_SANITIZE "ASan+UBSan on the host" OFF)
option(TENANT_VM_TSAN "TSan on the host" OFF)
# Release evidence MUST include compiler-generated .atto guests, not a host-only suite.
option(TENANT_VM_REQUIRE_ATTO_GUESTS "Reject configurations without exact-source .atto compiler lowering" OFF)

set(ROOT "${CMAKE_CURRENT_LIST_DIR}/../..")
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra -Wpedantic -Werror -UNDEBUG)
include_directories("${ROOT}/src/main/cpp" "${CMAKE_CURRENT_LIST_DIR}")

# Guests stay unsanitized and unprotected: sanitizers would add the libc calls and host
# memory access that guest code must never make. They are the code under test, not tools.
add_library(tenant_vm_guests STATIC guests.c)
target_compile_options(tenant_vm_guests PRIVATE -O1 -fno-stack-protector -Wno-infinite-recursion
    -Wno-gnu-designator -Wno-c99-designator -fno-sanitize=all)

add_executable(tenant_vm_test tenant_vm_test.cpp "${ROOT}/src/main/cpp/oreslang/runtime/TenantVm.cpp")
target_link_libraries(tenant_vm_test PRIVATE tenant_vm_guests Threads::Threads)
if(NOT TENANT_VM_SANITIZE AND NOT TENANT_VM_TSAN)
    target_compile_definitions(tenant_vm_test PRIVATE TENANT_VM_TIMING_ASSERTS)
endif()
if(TENANT_VM_SANITIZE)
    target_compile_options(tenant_vm_test PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(tenant_vm_test PRIVATE -fsanitize=address,undefined)
endif()
if(TENANT_VM_TSAN)
    target_compile_definitions(tenant_vm_test PRIVATE TENANT_VM_TSAN_BUILD)
    target_compile_options(tenant_vm_test PRIVATE -fsanitize=thread -fno-omit-frame-pointer)
    target_link_options(tenant_vm_test PRIVATE -fsanitize=thread)
endif()
add_test(NAME tenant_vm_test COMMAND tenant_vm_test)
set_tests_properties(tenant_vm_test PROPERTIES TIMEOUT 300)

# Enforced path: tenant source or Wasm, admitted and lowered to a loadable library. Needs WABT.
find_program(WAT2WASM wat2wasm)
find_program(WASM2C wasm2c)
find_path(WASM_RT_INCLUDE wasm-rt.h)
find_program(LOWER_CLANG NAMES clang clang-18 clang-19 clang-20)
find_package(Python3 COMPONENTS Interpreter)
# Optional, for .atto. The compiler may come from a different build.
set(TENANT_VM_ATTO_COMPILER "" CACHE FILEPATH "oreslang-llvmc, to lower real .atto source")
set(TENANT_VM_ATTO_IR_DIR "" CACHE PATH "IR already emitted by oreslang-llvmc --target=wasm32, one NAME.ll per fixture")
set(TENANT_VM_WASM_CLANG "${LOWER_CLANG}" CACHE FILEPATH "clang that can target wasm32")
if(TENANT_VM_REQUIRE_ATTO_GUESTS)
    if(NOT TENANT_VM_ATTO_COMPILER OR NOT EXISTS "${TENANT_VM_ATTO_COMPILER}" OR IS_DIRECTORY "${TENANT_VM_ATTO_COMPILER}")
        message(FATAL_ERROR "ATTO_E2E_NOT_EXECUTED: explicit executable TENANT_VM_ATTO_COMPILER required")
    endif()
    if(TENANT_VM_ATTO_IR_DIR)
        message(FATAL_ERROR "ATTO_E2E_NOT_EXECUTED: pre-generated .ll is not a source compiler run")
    endif()
    if(NOT WAT2WASM OR NOT WASM2C OR NOT WASM_RT_INCLUDE OR NOT LOWER_CLANG OR NOT Python3_FOUND)
        message(FATAL_ERROR "ATTO_E2E_NOT_EXECUTED: WABT, clang, Python and wasm-rt.h required")
    endif()
endif()
find_program(TENANT_VM_WASM_LD NAMES wasm-ld wasm-ld-18 wasm-ld-19 wasm-ld-20)
if(TENANT_VM_REQUIRE_ATTO_GUESTS AND NOT TENANT_VM_WASM_LD)
    message(FATAL_ERROR "ATTO_E2E_NOT_EXECUTED: wasm-ld required for .atto guest lowering")
endif()

if(WAT2WASM AND WASM2C AND WASM_RT_INCLUDE AND LOWER_CLANG AND Python3_FOUND)
    set(LOWER "${CMAKE_CURRENT_LIST_DIR}/../../tools/tenant-vm/lower_guest.py")
    set(ADAPTER "${CMAKE_CURRENT_LIST_DIR}/../../tools/tenant-vm/guest_adapter.c")
    set(GEN "${CMAKE_CURRENT_BINARY_DIR}/lowered")
    set(LOWER_ARGS --include "${ROOT}/src/main/cpp" --adapter "${ADAPTER}" --wasm-rt-include "${WASM_RT_INCLUDE}"
        --clang "${LOWER_CLANG}" --wat2wasm "${WAT2WASM}" --wasm2c "${WASM2C}")
    set(LOWERED_OUTPUTS "")
    function(lower_guest name input)
        add_custom_command(OUTPUT "${GEN}/${name}/manifest.json"
            COMMAND "${Python3_EXECUTABLE}" "${LOWER}" "${input}" --out "${GEN}/${name}" ${LOWER_ARGS} ${ARGN}
            DEPENDS "${input}" "${LOWER}" "${ADAPTER}" VERBATIM)
        set(LOWERED_OUTPUTS ${LOWERED_OUTPUTS} "${GEN}/${name}/manifest.json" PARENT_SCOPE)
    endfunction()
    set(HOSTILE "${CMAKE_CURRENT_LIST_DIR}/lowered/hostile.wat")
    lower_guest(hostile_bounds "${HOSTILE}" --mode bounds)
    lower_guest(hostile_guard "${HOSTILE}" --mode guard)

    set(ATTO_DEFINITION "")
    if(TENANT_VM_WASM_LD AND (TENANT_VM_ATTO_COMPILER OR TENANT_VM_ATTO_IR_DIR))
        set(ATTO_DEFINITION TENANT_VM_HAVE_ATTO)
        foreach(fixture counter doubler spin recurse work fleet misordered pair)
            if(TENANT_VM_ATTO_COMPILER)
                set(input "${CMAKE_CURRENT_LIST_DIR}/lowered/atto/${fixture}.atto")
            else()
                set(input "${TENANT_VM_ATTO_IR_DIR}/${fixture}.ll")
            endif()
            set(compiler_args "")
            if(TENANT_VM_ATTO_COMPILER)
                set(compiler_args --compiler "${TENANT_VM_ATTO_COMPILER}")
            endif()
            lower_guest(atto_${fixture} "${input}" ${compiler_args}
                --wasm-clang "${TENANT_VM_WASM_CLANG}" --wasm-ld "${TENANT_VM_WASM_LD}")
            if(fixture STREQUAL "pair")
                lower_guest(atto_pair_fault "${input}" ${compiler_args}
                    --wasm-clang "${TENANT_VM_WASM_CLANG}" --wasm-ld "${TENANT_VM_WASM_LD}"
                    --test-define ORES_VM_TEST_COMMIT_FAULT_ON=77)
                lower_guest(atto_pair_unsafe "${input}" ${compiler_args}
                    --wasm-clang "${TENANT_VM_WASM_CLANG}" --wasm-ld "${TENANT_VM_WASM_LD}"
                    --test-define ORES_VM_TEST_COMMIT_FAULT_ON=77 --test-define ORES_VM_TEST_UNSAFE_COMMIT=1)
            endif()
        endforeach()
    endif()
    if(TENANT_VM_REQUIRE_ATTO_GUESTS AND NOT ATTO_DEFINITION)
        message(FATAL_ERROR "ATTO_E2E_NOT_EXECUTED: real source lowering was not configured")
    endif()
    add_custom_target(tenant_vm_lowered_guests ALL DEPENDS ${LOWERED_OUTPUTS})

    add_executable(tenant_vm_lowered_test lowered/lowered_test.cpp
        "${ROOT}/src/main/cpp/oreslang/runtime/TenantVm.cpp"
        "${ROOT}/src/main/cpp/oreslang/runtime/TenantVmGuestLoader.cpp")
    target_include_directories(tenant_vm_lowered_test PRIVATE lowered)
    target_compile_definitions(tenant_vm_lowered_test PRIVATE ${ATTO_DEFINITION})
    target_link_libraries(tenant_vm_lowered_test PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
    add_dependencies(tenant_vm_lowered_test tenant_vm_lowered_guests)
    if(TENANT_VM_SANITIZE)
        target_compile_options(tenant_vm_lowered_test PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(tenant_vm_lowered_test PRIVATE -fsanitize=address,undefined)
    endif()
    if(TENANT_VM_TSAN) # run named cases by hand: TSan defers the signals that kill a pure spin
        target_compile_options(tenant_vm_lowered_test PRIVATE -fsanitize=thread -fno-omit-frame-pointer)
        target_link_options(tenant_vm_lowered_test PRIVATE -fsanitize=thread)
    endif()
    add_test(NAME tenant_vm_lowered_test COMMAND tenant_vm_lowered_test "${GEN}")
    set_tests_properties(tenant_vm_lowered_test PROPERTIES TIMEOUT 300)
    add_test(NAME tenant_vm_admission_test COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/lowered/admission_test.py"
        "${LOWER}" --wat2wasm "${WAT2WASM}" --runner "$<TARGET_FILE:tenant_vm_lowered_test>" --hostile "${HOSTILE}"
        --lowered-root "${GEN}" ${LOWER_ARGS})
    set_tests_properties(tenant_vm_admission_test PROPERTIES TIMEOUT 900)
    if(TENANT_VM_REQUIRE_ATTO_GUESTS)
        # Require the compiler-dependent source cases, not just the hostile .wat suite.
        add_test(NAME tenant_vm_source_main COMMAND tenant_vm_lowered_test "${GEN}" atto_main_spawns_sends_and_stops)
        add_test(NAME tenant_vm_source_fleet COMMAND tenant_vm_lowered_test "${GEN}" atto_thousand_tenants_run_programs)
        add_test(NAME tenant_vm_source_isolation COMMAND tenant_vm_lowered_test "${GEN}" atto_actor_state_is_private_and_durable)
        set_tests_properties(tenant_vm_source_main tenant_vm_source_fleet tenant_vm_source_isolation PROPERTIES TIMEOUT 300)
    endif()
endif()
