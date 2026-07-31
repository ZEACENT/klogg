if(NOT DEFINED DEPENDENCY)
  message(FATAL_ERROR "DEPENDENCY is required")
endif()
if(NOT DEFINED REPO_DIR)
  message(FATAL_ERROR "REPO_DIR is required")
endif()
if(NOT DEFINED EXPECTED_REVISION)
  message(FATAL_ERROR "EXPECTED_REVISION is required")
endif()
if(NOT DEFINED CLEAN_TREE_HASH)
  message(FATAL_ERROR "CLEAN_TREE_HASH is required")
endif()
if(NOT DEFINED APPROVED_TREE_HASHES)
  message(FATAL_ERROR "APPROVED_TREE_HASHES is required")
endif()
if(NOT DEFINED PATCHED_TREE_HASH)
  message(FATAL_ERROR "PATCHED_TREE_HASH is required")
endif()
if(NOT DEFINED PATCH_FILES)
  message(FATAL_ERROR "PATCH_FILES is required")
endif()
if(NOT DEFINED LOCK_TIMEOUT)
  set(LOCK_TIMEOUT 120)
endif()

function(klogg_reverse_patch_list patch_list)
  set(_patches ${patch_list})
  list(LENGTH _patches _patch_count)
  while(_patch_count GREATER 0)
    math(EXPR _patch_count "${_patch_count} - 1")
    list(GET _patches ${_patch_count} _patch_file)
    execute_process(
      COMMAND git apply --reverse --check "${_patch_file}"
      WORKING_DIRECTORY "${REPO_DIR}"
      RESULT_VARIABLE _reverse_check_result
      ERROR_QUIET
    )
    if(_reverse_check_result EQUAL 0)
      execute_process(
        COMMAND git apply --reverse "${_patch_file}"
        WORKING_DIRECTORY "${REPO_DIR}"
        RESULT_VARIABLE _reverse_result
      )
      if(NOT _reverse_result EQUAL 0)
        message(FATAL_ERROR "Failed to roll back patch: ${_patch_file}")
      endif()
      message(STATUS "Rolled back patch: ${_patch_file}")
    endif()
  endwhile()
endfunction()

function(klogg_rollback_applied_patches)
  if(_applied_patches)
    klogg_reverse_patch_list("${_applied_patches}")
  endif()
endfunction()

get_filename_component(REPO_DIR "${REPO_DIR}" REALPATH)
get_filename_component(REPO_PARENT_DIR "${REPO_DIR}" DIRECTORY)
if(DEFINED BEFORE_LOCK_MARKER)
  file(WRITE "${BEFORE_LOCK_MARKER}" "waiting")
endif()
file(LOCK "${REPO_PARENT_DIR}/klogg-patch.lock"
     GUARD PROCESS
     TIMEOUT ${LOCK_TIMEOUT}
     RESULT_VARIABLE patch_lock_result)
if(NOT patch_lock_result EQUAL 0)
  message(FATAL_ERROR
          "Failed to lock ${DEPENDENCY} source before verification and patching: ${patch_lock_result}")
endif()
if(DEFINED LOCK_ACQUIRED_MARKER)
  file(WRITE "${LOCK_ACQUIRED_MARKER}" "acquired")
endif()

include("${CMAKE_CURRENT_LIST_DIR}/verify_pinned_source.cmake")
klogg_require_pinned_revision(
  "${DEPENDENCY}"
  "${REPO_DIR}"
  "${EXPECTED_REVISION}"
)

klogg_source_tree_sha256("${REPO_DIR}" _initial_tree_hash)
if("${_initial_tree_hash}" STREQUAL "${PATCHED_TREE_HASH}")
  message(STATUS "${DEPENDENCY} patch series already applied")
  return()
endif()

if(NOT "${_initial_tree_hash}" STREQUAL "${CLEAN_TREE_HASH}")
  message(STATUS "Recovering interrupted ${DEPENDENCY} patch series")
  klogg_reverse_patch_list("${PATCH_FILES}")
  klogg_source_tree_sha256("${REPO_DIR}" _recovered_tree_hash)
  if(NOT "${_recovered_tree_hash}" STREQUAL "${CLEAN_TREE_HASH}")
    message(
      FATAL_ERROR
        "${DEPENDENCY} source tree SHA-256 mismatch after patch recovery: got ${_recovered_tree_hash}, expected ${CLEAN_TREE_HASH}"
    )
  endif()
endif()

set(_applied_patches "")
set(_patch_index 0)
foreach(PATCH_FILE IN LISTS PATCH_FILES)
  math(EXPR _patch_index "${_patch_index} + 1")
  if(NOT EXISTS "${PATCH_FILE}")
    klogg_rollback_applied_patches()
    message(FATAL_ERROR "Patch file does not exist: ${PATCH_FILE}")
  endif()

  execute_process(
    COMMAND git apply --check "${PATCH_FILE}"
    WORKING_DIRECTORY "${REPO_DIR}"
    RESULT_VARIABLE patch_check_result
    ERROR_QUIET
  )
  if(NOT patch_check_result EQUAL 0)
    klogg_rollback_applied_patches()
    message(FATAL_ERROR
            "Failed to apply patch (context mismatch): ${PATCH_FILE}. "
            "The dependency source was restored to its approved clean state.")
  endif()

  execute_process(
    COMMAND git apply "${PATCH_FILE}"
    WORKING_DIRECTORY "${REPO_DIR}"
    RESULT_VARIABLE patch_apply_result
  )
  if(NOT patch_apply_result EQUAL 0)
    klogg_rollback_applied_patches()
    message(FATAL_ERROR "Failed to apply patch: ${PATCH_FILE}")
  endif()
  list(APPEND _applied_patches "${PATCH_FILE}")
  message(STATUS "Applied patch: ${PATCH_FILE}")

  if(DEFINED PAUSE_AFTER_PATCH_INDEX
     AND _patch_index EQUAL PAUSE_AFTER_PATCH_INDEX)
    if(DEFINED PAUSE_MARKER)
      file(WRITE "${PAUSE_MARKER}" "paused")
    endif()
    if(DEFINED PAUSE_RELEASE_MARKER)
      set(_pause_waits 0)
      while(NOT EXISTS "${PAUSE_RELEASE_MARKER}" AND _pause_waits LESS 10)
        execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 1)
        math(EXPR _pause_waits "${_pause_waits} + 1")
      endwhile()
      if(NOT EXISTS "${PAUSE_RELEASE_MARKER}")
        klogg_rollback_applied_patches()
        message(FATAL_ERROR "Timed out waiting to resume patch transaction")
      endif()
    else()
      execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 1)
    endif()
  endif()
endforeach()

# The transaction must finish in the exact final patched state, not merely one
# of the input states accepted before patching.
klogg_source_tree_sha256("${REPO_DIR}" _final_tree_hash)
if(NOT "${_final_tree_hash}" STREQUAL "${PATCHED_TREE_HASH}")
  klogg_rollback_applied_patches()
  message(
    FATAL_ERROR
      "${DEPENDENCY} patch series produced ${_final_tree_hash}, expected ${PATCHED_TREE_HASH}; restored the approved clean tree"
  )
endif()
