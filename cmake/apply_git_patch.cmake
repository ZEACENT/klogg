if( NOT DEFINED PATCH_FILE )
  message( FATAL_ERROR "PATCH_FILE is required" )
endif()

if( NOT DEFINED REPO_DIR )
  set( REPO_DIR "." )
endif()

get_filename_component( REPO_DIR "${REPO_DIR}" REALPATH )
get_filename_component( REPO_PARENT_DIR "${REPO_DIR}" DIRECTORY )
# CPM may keep its own cmake.lock for the lifetime of the configure process
# after first populating a cache entry. Use a sibling lock for klogg's
# check/apply transaction so child cmake -P processes do not deadlock while
# concurrent build directories still serialize patching the shared source.
file( LOCK "${REPO_PARENT_DIR}/klogg-patch.lock"
      GUARD PROCESS
      TIMEOUT 120
      RESULT_VARIABLE patch_lock_result )
if( NOT patch_lock_result EQUAL 0 )
  message( FATAL_ERROR
           "Failed to lock dependency source before applying ${PATCH_FILE}: ${patch_lock_result}" )
endif()

execute_process(
  COMMAND git apply --check "${PATCH_FILE}"
  WORKING_DIRECTORY "${REPO_DIR}"
  RESULT_VARIABLE patch_check_result
)

if( patch_check_result EQUAL 0 )
  execute_process(
    COMMAND git apply "${PATCH_FILE}"
    WORKING_DIRECTORY "${REPO_DIR}"
    RESULT_VARIABLE patch_apply_result
  )
  if( NOT patch_apply_result EQUAL 0 )
    message( FATAL_ERROR "Failed to apply patch: ${PATCH_FILE}" )
  endif()
  message( STATUS "Applied patch: ${PATCH_FILE}" )
  return()
endif()

execute_process(
  COMMAND git apply --reverse --check "${PATCH_FILE}"
  WORKING_DIRECTORY "${REPO_DIR}"
  RESULT_VARIABLE patch_reverse_check_result
)

if( patch_reverse_check_result EQUAL 0 )
  message( STATUS "Patch already applied: ${PATCH_FILE}" )
  return()
endif()

message( FATAL_ERROR
         "Failed to apply patch (context mismatch): ${PATCH_FILE}. "
         "Please delete the dependency build directory and reconfigure." )
