# Actual native compiler admission gate. A negative parse must never emit IR.
if(NOT DEFINED COMPILER OR NOT DEFINED WORKDIR)
  message(FATAL_ERROR "COMPILER and WORKDIR are required")
endif()
file(MAKE_DIRECTORY "${WORKDIR}")
set(cases
  "state-container"
  "state-container-public"
  "virtual-actor"
  "virtual-actor-public")
set(source_state-container "define state container Counter as end\n")
set(source_state-container-public "pub define state container Counter as end\n")
set(source_virtual-actor "define virtual actor Cart as end\n")
set(source_virtual-actor-public "define pub virtual actor Cart as end\n")
foreach(case_name IN LISTS cases)
  set(case_source "${source_${case_name}}")
  set(path "${WORKDIR}/${case_name}.ores")
  file(WRITE "${path}" "${case_source}")
  if(case_name MATCHES "^state-")
    set(diagnostic "STATE_CONTAINER_BACKEND_UNSUPPORTED")
  else()
    set(diagnostic "VIRTUAL_ACTOR_BACKEND_UNSUPPORTED")
  endif()
  file(GLOB before RELATIVE "${WORKDIR}" "${WORKDIR}/*")
  execute_process(COMMAND "${COMPILER}" "${path}"
    WORKING_DIRECTORY "${WORKDIR}"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error
    TIMEOUT 10)
  if("${result}" STREQUAL "0")
    message(FATAL_ERROR "unsupported ${case_name} compiled successfully")
  endif()
  if(NOT "${error}" MATCHES "${diagnostic}")
    message(FATAL_ERROR "${case_name} failed without expected diagnostic: ${error}")
  endif()
  if(NOT "${output}" STREQUAL "")
    message(FATAL_ERROR "${case_name} emitted output on failed compilation")
  endif()
  file(GLOB after RELATIVE "${WORKDIR}" "${WORKDIR}/*")
  if(NOT "${before}" STREQUAL "${after}")
    message(FATAL_ERROR "${case_name} emitted an unexpected compilation artifact")
  endif()
endforeach()
