cmake_minimum_required(VERSION 3.20)
project(oresvm_host_isolate_smoke LANGUAGES CXX)
include(CTest)
find_package(Threads REQUIRED)

# Standalone OresVM host tests: does not require LLVM development libraries,
# the oreslang-llvmc compiler, or any cross-org runner/source transfer.
option(ORESVM_SANITIZERS "Build host-only tests with AddressSanitizer and UBSan" OFF)
if(ORESVM_SANITIZERS)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        message(FATAL_ERROR "ORESVM_SANITIZERS requires GCC or Clang")
    endif()
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

set(ORE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../..")
foreach(test IN ITEMS native_oresvm_isolate_test native_oresvm_guest_call_test
    native_oresvm_resource_admission_test native_oresvm_shutdown_race_test)
    add_executable("${test}" "${ORE_ROOT}/tests/${test}.cpp")
    target_include_directories("${test}" PRIVATE "${ORE_ROOT}/src/main/cpp")
    target_compile_features("${test}" PRIVATE cxx_std_17)
    target_compile_options("${test}" PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra -Wpedantic -Werror -UNDEBUG>)
    target_link_libraries("${test}" PRIVATE Threads::Threads)
    add_test(NAME "${test}" COMMAND "${test}")
    set_tests_properties("${test}" PROPERTIES TIMEOUT 30)
endforeach()

# The handwritten ABI fixture is tested independently of the full compiler.
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    find_program(ORESVM_CLANG clang)
    if(ORESVM_CLANG)
        set(ORESVM_AOT_OBJECT "${CMAKE_CURRENT_BINARY_DIR}/oresvm-guest-counter.o")
        add_custom_command(OUTPUT "${ORESVM_AOT_OBJECT}"
            COMMAND "${ORESVM_CLANG}" -x ir -c
                "${ORE_ROOT}/tests/native_oresvm_guest_counter.ll"
                -o "${ORESVM_AOT_OBJECT}"
            DEPENDS "${ORE_ROOT}/tests/native_oresvm_guest_counter.ll" VERBATIM)
        set_source_files_properties("${ORESVM_AOT_OBJECT}" PROPERTIES
            GENERATED TRUE EXTERNAL_OBJECT TRUE)
        add_executable(native_oresvm_guest_aot_test
            "${ORE_ROOT}/tests/native_oresvm_guest_aot_test.cpp" "${ORESVM_AOT_OBJECT}")
        target_include_directories(native_oresvm_guest_aot_test PRIVATE
            "${ORE_ROOT}/src/main/cpp")
        target_compile_features(native_oresvm_guest_aot_test PRIVATE cxx_std_17)
        target_compile_options(native_oresvm_guest_aot_test PRIVATE
            $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra -Wpedantic -Werror -UNDEBUG>)
        target_link_libraries(native_oresvm_guest_aot_test PRIVATE Threads::Threads)
        add_test(NAME native_oresvm_guest_aot_test COMMAND native_oresvm_guest_aot_test)
        set_tests_properties(native_oresvm_guest_aot_test PROPERTIES TIMEOUT 30)
    endif()
endif()
