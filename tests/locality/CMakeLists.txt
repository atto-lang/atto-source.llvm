cmake_minimum_required(VERSION 3.20)
project(atto_native_ready_index_contract LANGUAGES C)
include(CTest)
find_package(Threads REQUIRED)
set(NATIVE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../..")
set(NATIVE_INCLUDE "${NATIVE_ROOT}/src/main/cpp")
set(NATIVE_SCHEDULER "${NATIVE_INCLUDE}/oreslang/runtime/NativeActorScheduler.c")

function(native_index_fixture target source)
  add_executable(${target}
      "${NATIVE_ROOT}/tests/${source}.c"
      "${NATIVE_SCHEDULER}")
  target_include_directories(${target} PRIVATE
      "${NATIVE_INCLUDE}" "${NATIVE_ROOT}/tests")
  target_compile_features(${target} PRIVATE c_std_11)
  target_compile_options(${target} PRIVATE
      -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
  target_link_libraries(${target} PRIVATE Threads::Threads)
  add_test(NAME ${target} COMMAND ${target})
  set_tests_properties(${target} PROPERTIES TIMEOUT 50)
endfunction()

if(BUILD_TESTING)
  add_executable(native_ready_index_bitmap
      "${NATIVE_ROOT}/tests/native_ready_bitmap_test.c")
  target_include_directories(native_ready_index_bitmap PRIVATE "${NATIVE_INCLUDE}")
  target_compile_features(native_ready_index_bitmap PRIVATE c_std_11)
  target_compile_options(native_ready_index_bitmap PRIVATE
      -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
  add_test(NAME native_ready_index_bitmap COMMAND native_ready_index_bitmap)
  set_tests_properties(native_ready_index_bitmap PROPERTIES TIMEOUT 30)

  native_index_fixture(native_actor_scheduler_test native_actor_scheduler_test)
  native_index_fixture(native_actor_exclusive_control_test native_actor_exclusive_control_test)
  native_index_fixture(native_continuation_wake_escrow_test native_continuation_wake_escrow_test)
  native_index_fixture(native_actor_send_stop_race_test native_actor_send_stop_race_test)
  native_index_fixture(native_actor_typed_control_exclusive_test native_actor_typed_control_exclusive_test)
  native_index_fixture(native_actor_ready_bitmap_1024_stress_test native_actor_ready_bitmap_1024_stress_test)
endif()
