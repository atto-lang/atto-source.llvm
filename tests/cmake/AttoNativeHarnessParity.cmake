# Linux-native AttoLang portable TestKit M0. Only source-owning CMake/CTest.
# Source-free inventory must not be confused with real compiler guest execution.
if(BUILD_TESTING AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_test(NAME atto_native_harness_inventory_source_only
        COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_atto_native_parity_manifest.sh")
    set_tests_properties(atto_native_harness_inventory_source_only
        PROPERTIES TIMEOUT 120 LABELS "attolang;source-free")

    # The compiler target is built by the calling CMake build; CTest must run
    # the production binary. Missing Clang, compiler, source or runner fail.
    add_test(NAME atto_native_harness_compiler_guest_o0_o2
        COMMAND "${CMAKE_COMMAND}" -E env
            "ORESLANG_LLVMC=$<TARGET_FILE:oreslang-llvmc>"
            "CLANG=clang"
            "ORES_NO_GC_IR_AUDIT=${CMAKE_CURRENT_BINARY_DIR}/oreslang-no-gc-ir-audit"
            bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/atto_native_harness_parity.sh" --run)
    set_tests_properties(atto_native_harness_compiler_guest_o0_o2
        PROPERTIES TIMEOUT 900 LABELS "attolang;compiler-backed;native")
endif()
