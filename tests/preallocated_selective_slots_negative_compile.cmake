# Compile-negative contract: a custom deleter must not run move/destructor
# callbacks inside a caller-held actor mailbox lock, even if noexcept.
foreach(required CXX INCLUDE_DIR SOURCE)
  if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
    message(FATAL_ERROR "missing negative compile argument: ${required}")
  endif()
endforeach()
foreach(mode ATTO_NEGATIVE_DELETER_DESTRUCTOR ATTO_NEGATIVE_DELETER_MOVE)
  execute_process(
    COMMAND "${CXX}" "-std=c++17" "-fsyntax-only"
      "-I${INCLUDE_DIR}" "-D${mode}" "${SOURCE}"
    RESULT_VARIABLE rc
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr)
  if(rc EQUAL 0)
    message(FATAL_ERROR "unsafe ${mode} deleter was accepted")
  endif()
  if(NOT stderr MATCHES "actor-owned deleter state must be trivially movable/destructible")
    message(FATAL_ERROR "${mode} rejected for the wrong reason: ${stderr}")
  endif()
endforeach()
message(STATUS "unsafe deleter move/destructor types rejected as expected")
