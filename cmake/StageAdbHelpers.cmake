cmake_minimum_required(VERSION 3.14)

# The generated manifest contains paths only, never executable CMake code.
if(NOT IS_DIRECTORY "${SOURCE_HELPERS}")
  message(FATAL_ERROR "Cannot copy verified ADB helpers: source directory is missing")
endif()
file(STRINGS "${DESTINATIONS_FILE}" helper_destinations ENCODING UTF-8)
list(REMOVE_DUPLICATES helper_destinations)
if(NOT helper_destinations)
  message(FATAL_ERROR "The ADB helper destination manifest is empty")
endif()

foreach(destination IN LISTS helper_destinations)
  get_filename_component(directory_name "${destination}" NAME)
  if(NOT IS_ABSOLUTE "${destination}" OR NOT directory_name STREQUAL "helpers")
    message(FATAL_ERROR "Invalid ADB helper destination: ${destination}")
  endif()
  if(destination STREQUAL SOURCE_HELPERS)
    message(FATAL_ERROR "ADB helpers cannot be staged over their verified source")
  endif()
endforeach()

foreach(destination IN LISTS helper_destinations)
  execute_process(
    COMMAND "${CMAKE_COMMAND}" -E rm -rf "${destination}"
    RESULT_VARIABLE removal_result
  )
  if(NOT "${removal_result}" STREQUAL "0")
    message(FATAL_ERROR "Removing stale ADB helpers failed: ${destination}")
  endif()
  execute_process(
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${SOURCE_HELPERS}" "${destination}"
    RESULT_VARIABLE copy_result
  )
  if(NOT "${copy_result}" STREQUAL "0")
    message(FATAL_ERROR "Copying verified ADB helpers failed: ${destination}")
  endif()
endforeach()
