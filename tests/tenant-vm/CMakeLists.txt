cmake_minimum_required(VERSION 3.20)
project(tenant_vm LANGUAGES C CXX)
include(CTest)
find_package(Threads REQUIRED)

option(TENANT_VM_SANITIZE "ASan+UBSan on the host" OFF)
option(TENANT_VM_TSAN "TSan on the host" OFF)

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

# Enforced path: Wasm validated and lowered to native. Needs WABT.
find_program(WAT2WASM wat2wasm)
find_program(WASM2C wasm2c)
find_path(WASM_RT_INCLUDE wasm-rt.h)
if(WAT2WASM AND WASM2C AND WASM_RT_INCLUDE)
    set(GEN "${CMAKE_CURRENT_BINARY_DIR}/lowered")
    file(MAKE_DIRECTORY "${GEN}")
    add_custom_command(OUTPUT "${GEN}/guest.c" "${GEN}/guest.h"
        COMMAND "${WAT2WASM}" "${CMAKE_CURRENT_LIST_DIR}/wasm/guest.wat" -o "${GEN}/guest.wasm"
        COMMAND "${WASM2C}" "${GEN}/guest.wasm" -o "${GEN}/guest.c" --module-name=guest
        DEPENDS wasm/guest.wat VERBATIM)
    add_library(tenant_vm_lowered STATIC wasm/adapter.c "${GEN}/guest.c")
    target_include_directories(tenant_vm_lowered PRIVATE "${GEN}" "${WASM_RT_INCLUDE}" wasm)
    target_compile_definitions(tenant_vm_lowered PRIVATE WASM_RT_MEMCHECK_GUARD_PAGES=1 WASM_RT_USE_MMAP=1)
    target_compile_options(tenant_vm_lowered PRIVATE -O2 -w -Wno-error -Wno-pedantic)
    add_executable(tenant_vm_lowered_test wasm/lowered_test.cpp "${ROOT}/src/main/cpp/oreslang/runtime/TenantVm.cpp")
    target_include_directories(tenant_vm_lowered_test PRIVATE wasm)
    target_link_libraries(tenant_vm_lowered_test PRIVATE tenant_vm_lowered Threads::Threads)
    add_test(NAME tenant_vm_lowered_test COMMAND tenant_vm_lowered_test)
    set_tests_properties(tenant_vm_lowered_test PROPERTIES TIMEOUT 120)
endif()
