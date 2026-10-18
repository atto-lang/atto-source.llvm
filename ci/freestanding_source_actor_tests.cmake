# Include only from the LLVM source repository's root CMakeLists.txt,
# after include(CTest). These are host-native tests with libc/POSIX for
# assertions/trap isolation; production freestanding sources remain no-libc.
if(BUILD_TESTING AND UNIX)
  set(ORE_ABI_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
  set(ORE_ABI_RUNTIME
      "${ORE_ABI_ROOT}/src/main/cpp/oreslang/runtime/FreestandingActorKernel.c"
      "${ORE_ABI_ROOT}/src/main/cpp/oreslang/runtime/FreestandingSourceActor.c")
  add_executable(ores-freestanding-actor-abi-test
      "${ORE_ABI_ROOT}/tests/freestanding_source_actor_abi_test.c"
      ${ORE_ABI_RUNTIME})
  target_include_directories(ores-freestanding-actor-abi-test PRIVATE
      "${ORE_ABI_ROOT}/src/main/cpp")
  target_compile_features(ores-freestanding-actor-abi-test PRIVATE c_std_11)
  target_compile_options(ores-freestanding-actor-abi-test PRIVATE
      -UNDEBUG -Wall -Wextra -Wpedantic -Werror)
  add_test(NAME freestanding_source_actor_abi_host
      COMMAND ores-freestanding-actor-abi-test)
  foreach(variant positive negative)
    add_executable(ores-freestanding-source-firmware-${variant}
        "${ORE_ABI_ROOT}/tests/freestanding_source_actor_firmware.c"
        "${ORE_ABI_ROOT}/tests/freestanding_source_actor_host_firmware_main.c"
        ${ORE_ABI_RUNTIME})
    target_include_directories(ores-freestanding-source-firmware-${variant} PRIVATE
        "${ORE_ABI_ROOT}/src/main/cpp")
    target_compile_features(ores-freestanding-source-firmware-${variant} PRIVATE c_std_11)
    target_compile_options(ores-freestanding-source-firmware-${variant} PRIVATE
        -UNDEBUG -Wall -Wextra -Wpedantic -Werror)
    if(variant STREQUAL "negative")
      target_compile_definitions(ores-freestanding-source-firmware-${variant}
          PRIVATE NEGATIVE_GUEST)
    endif()
    add_test(NAME freestanding_source_firmware_${variant}_host
        COMMAND ores-freestanding-source-firmware-${variant})
  endforeach()
endif()

# Exact compiler-generated Oreslang actor integration. This gate cannot
# silently replace .ores source with C callbacks: a built oreslang-llvmc is
# mandatory, as are clang, readelf and QEMU when required mode is enabled.
option(ORE_REQUIRE_SOURCE_ACTOR_QEMU
    "Require Oreslang compiler-generated freestanding source actors in QEMU" OFF)
if(BUILD_TESTING)
  find_package(Python3 COMPONENTS Interpreter REQUIRED)
  find_program(ORE_SOURCE_ACTOR_CLANG NAMES clang-18 clang)
  find_program(ORE_SOURCE_ACTOR_READELF NAMES readelf llvm-readelf)
  find_program(ORE_SOURCE_ACTOR_QEMU NAMES qemu-system-arm)
  if(ORE_REQUIRE_SOURCE_ACTOR_QEMU AND
     (NOT TARGET oreslang-llvmc OR NOT ORE_SOURCE_ACTOR_CLANG OR
      NOT ORE_SOURCE_ACTOR_READELF OR NOT ORE_SOURCE_ACTOR_QEMU))
    message(FATAL_ERROR "Compiler-generated freestanding Oreslang actor QEMU gate needs oreslang-llvmc, clang, readelf and qemu-system-arm")
  endif()
  if(TARGET oreslang-llvmc AND ORE_SOURCE_ACTOR_CLANG AND
     ORE_SOURCE_ACTOR_READELF AND ORE_SOURCE_ACTOR_QEMU)
    add_test(NAME freestanding_source_defined_actor_qemu
      COMMAND "${Python3_EXECUTABLE}"
              "${CMAKE_CURRENT_SOURCE_DIR}/tests/freestanding_source_actor_compiler_qemu.py"
              --compiler "$<TARGET_FILE:oreslang-llvmc>"
              --clang "${ORE_SOURCE_ACTOR_CLANG}"
              --readelf "${ORE_SOURCE_ACTOR_READELF}"
              --qemu "${ORE_SOURCE_ACTOR_QEMU}")
    set_tests_properties(freestanding_source_defined_actor_qemu
      PROPERTIES TIMEOUT 150)
  endif()
endif()
