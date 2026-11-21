# Permanent compile-negative security test: ordinary host code may not pass
# Boolean completion/release claims to the V0 actor ledger.
# No OpenCL device, secrets, GitHub token or complete LLVM build is required.
if(NOT DEFINED GPU_ACTOR_CXX_COMPILER OR NOT EXISTS "${GPU_ACTOR_CXX_COMPILER}")
  message(FATAL_ERROR "GPU_ACTOR_V0_GATE_MISSING_COMPILER")
endif()
if(NOT DEFINED GPU_ACTOR_INCLUDE_ROOT OR NOT IS_DIRECTORY "${GPU_ACTOR_INCLUDE_ROOT}")
  message(FATAL_ERROR "GPU_ACTOR_V0_GATE_MISSING_INCLUDES")
endif()
if(NOT DEFINED GPU_ACTOR_NEGATIVE_SOURCE OR NOT EXISTS "${GPU_ACTOR_NEGATIVE_SOURCE}")
  message(FATAL_ERROR "GPU_ACTOR_V0_GATE_MISSING_NEGATIVE_SOURCE")
endif()
execute_process(
  COMMAND "${GPU_ACTOR_CXX_COMPILER}" -std=c++17 -Wall -Wextra -Wpedantic -Werror
          "-I${GPU_ACTOR_INCLUDE_ROOT}" -fsyntax-only "${GPU_ACTOR_NEGATIVE_SOURCE}"
  RESULT_VARIABLE result
  OUTPUT_VARIABLE output
  ERROR_VARIABLE errors
  TIMEOUT 25)
if(result STREQUAL "0")
  message(FATAL_ERROR "GPU_ACTOR_V0_BOOL_AUTHORITY_EXTERNALLY_ACCESSIBLE")
endif()
if(NOT (errors MATCHES "private within this context" OR errors MATCHES "is a private member"))
  message(FATAL_ERROR "GPU_ACTOR_V0_NEGATIVE_TEST_FAILED_UNRELATED: ${errors}")
endif()
foreach(method IN ITEMS
    finish_after_provider_event
    acknowledge_cancelled_provider_event
    acknowledge_backend_buffer_release
    retire_gracefully_after_backend_release)
  if(NOT errors MATCHES "${method}")
    message(FATAL_ERROR "GPU_ACTOR_V0_NEGATIVE_TEST_MISSING_METHOD: ${method}")
  endif()
endforeach()
message(STATUS "GPU_ACTOR_V0_PRODUCTION_AUTHORITY_DENIED compiler=${GPU_ACTOR_CXX_COMPILER}")
