# Standalone cross-target Clang C++ bool ABI reference for the LLVM Assert
# primitive guest path. This does NOT compile an Oreslang source module.
cmake_minimum_required(VERSION 3.20)
if(NOT DEFINED CLANG_CXX OR "${CLANG_CXX}" STREQUAL "")
    message(FATAL_ERROR "CLANG_CXX is required for bool C ABI oracle")
endif()
set(out_dir "${CMAKE_CURRENT_BINARY_DIR}/oreslang-assert-bool-c-abi")
file(MAKE_DIRECTORY "${out_dir}")
file(WRITE "${out_dir}/probe.cpp"
    "extern \"C\" bool oreslang_assert_scalar_bool_abi_probe(bool b) { return b; }\n")
# The return i1 extension differs by target: x86 and Darwin arm64 use
# zeroext; Linux AArch64 does not. Parameter attributes aren't a surrogate
# for the return attribute, so isolate the definition signature.
set(triples
    "x86_64-unknown-linux-gnu"
    "i686-unknown-linux-gnu"
    "aarch64-unknown-linux-gnu"
    "arm64-apple-macosx13.0")
set(expected_extension TRUE TRUE FALSE TRUE)
set(index 0)
foreach(triple IN LISTS triples)
    list(GET expected_extension ${index} expected)
    execute_process(
        COMMAND "${CLANG_CXX}" "--target=${triple}" -std=c++17 -O0
                -S -emit-llvm -x c++ "${out_dir}/probe.cpp" -o -
        RESULT_VARIABLE status OUTPUT_VARIABLE ir ERROR_VARIABLE diagnostic
        TIMEOUT 25)
    if(NOT "${status}" STREQUAL "0")
        message(FATAL_ERROR "Clang target ${triple} failed: ${status} ${diagnostic}")
    endif()
    string(REGEX MATCH "define[^\n]*@oreslang_assert_scalar_bool_abi_probe\\(" definition "${ir}")
    if(NOT definition)
        message(FATAL_ERROR "No LLVM definition for ${triple}; cannot infer bool return ABI")
    endif()
    if(NOT definition MATCHES "i1[ ]+@oreslang_assert_scalar_bool_abi_probe\\(")
        message(FATAL_ERROR "Unexpected non-i1 bool return ABI for ${triple}: ${definition}")
    endif()
    set(has_zeroext FALSE)
    if(definition MATCHES "(^|[ \t])zeroext[ \t]+i1[ \t]+@oreslang_assert_scalar_bool_abi_probe\\(")
        set(has_zeroext TRUE)
    endif()
    if(NOT "${has_zeroext}" STREQUAL "${expected}")
        message(FATAL_ERROR "Bool return ABI mismatch: ${triple} expected zeroext=${expected}, observed=${has_zeroext}; ${definition}")
    endif()
    math(EXPR index "${index} + 1")
endforeach()
message(STATUS "NATIVE_ASSERT_BOOL_C_ABI_TARGET_ORACLE_PASS: four clang targets")
