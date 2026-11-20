# Worker-only native recovery methods must remain private to trusted worker.
if(NOT DEFINED GPU_ACTOR_CXX_COMPILER OR NOT EXISTS "${GPU_ACTOR_CXX_COMPILER}")
    message(FATAL_ERROR "GPU_RECOVERY_GATE_MISSING_COMPILER")
endif()
if(NOT DEFINED GPU_ACTOR_INCLUDE_ROOT OR NOT IS_DIRECTORY "${GPU_ACTOR_INCLUDE_ROOT}")
    message(FATAL_ERROR "GPU_RECOVERY_GATE_MISSING_INCLUDES")
endif()
if(NOT DEFINED GPU_ACTOR_NEGATIVE_SOURCE OR NOT EXISTS "${GPU_ACTOR_NEGATIVE_SOURCE}")
    message(FATAL_ERROR "GPU_RECOVERY_GATE_MISSING_SOURCE")
endif()
execute_process(
    COMMAND "${GPU_ACTOR_CXX_COMPILER}" -std=c++17
        -Wall -Wextra -Wpedantic -Werror
        "-I${GPU_ACTOR_INCLUDE_ROOT}" -fsyntax-only "${GPU_ACTOR_NEGATIVE_SOURCE}"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors TIMEOUT 25)
if(result STREQUAL "0")
    message(FATAL_ERROR "GPU_RECOVERY_PRIVILEGED_METHODS_EXPOSED")
endif()
if(NOT (errors MATCHES "private within this context" OR
        errors MATCHES "is a private member"))
    message(FATAL_ERROR "GPU_RECOVERY_NEGATIVE_UNRELATED_COMPILER_FAILURE: ${errors}")
endif()
foreach(method IN ITEMS recover_unknown_begin_on_worker
        recover_unknown_kernel_on_worker
        recover_device_loss_on_worker
        recover_release_failure_on_worker
        matches_recovery_turn)
    if(NOT errors MATCHES "${method}")
        message(FATAL_ERROR "GPU_RECOVERY_NEGATIVE_MISSING_METHOD: ${method}")
    endif()
endforeach()
message(STATUS "GPU_RECOVERY_WORKER_ONLY_AUTHORITY_DENIED compiler=${GPU_ACTOR_CXX_COMPILER}")
