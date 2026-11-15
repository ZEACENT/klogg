if(NOT DEFINED SOURCE OR NOT EXISTS "${SOURCE}")
  message(FATAL_ERROR "The control CLI must be built before bundle staging")
endif()
if(NOT DEFINED DESTINATION_FILE OR NOT EXISTS "${DESTINATION_FILE}")
  message(FATAL_ERROR "The control CLI bundle destination is unavailable")
endif()
file(READ "${DESTINATION_FILE}" destination)
string(STRIP "${destination}" destination)
if(destination STREQUAL "" OR NOT IS_ABSOLUTE "${destination}")
  message(FATAL_ERROR "The control CLI bundle destination must be absolute")
endif()
file(MAKE_DIRECTORY "${destination}")
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${SOURCE}" "${destination}/kloggctl"
  RESULT_VARIABLE result
)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "Could not stage the control CLI in the application bundle")
endif()
