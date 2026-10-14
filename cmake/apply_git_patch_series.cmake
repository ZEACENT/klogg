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

set(_patch_command git)
set(_snapshot_mode FALSE)
function(klogg_patch_require_owned_completion result diagnostic)
  if(_snapshot_mode AND NOT "${result}" STREQUAL "0" AND NOT "${result}" STREQUAL "1")
    message(FATAL_ERROR "Owned snapshot patch command failed (status ${result}); preserving private state: ${diagnostic}")
  endif()
endfunction()

function(klogg_reverse_patch_list patch_list require_all)
  set(_patches ${patch_list})
  list(LENGTH _patches _patch_count)
  while(_patch_count GREATER 0)
    math(EXPR _patch_count "${_patch_count} - 1")
    list(GET _patches ${_patch_count} _patch_file)
    execute_process(
      COMMAND ${_patch_command} apply --reverse --check "${_patch_file}"
      WORKING_DIRECTORY "${REPO_DIR}"
      RESULT_VARIABLE _reverse_check_result
      ERROR_VARIABLE _reverse_check_error
    )
    klogg_patch_require_owned_completion("${_reverse_check_result}" "${_reverse_check_error}")
    if(_reverse_check_result EQUAL 0)
      execute_process(
        COMMAND ${_patch_command} apply --reverse "${_patch_file}"
        WORKING_DIRECTORY "${REPO_DIR}"
        RESULT_VARIABLE _reverse_result
        ERROR_VARIABLE _reverse_error
      )
      klogg_patch_require_owned_completion("${_reverse_result}" "${_reverse_error}")
      if(_reverse_error)
        message("${_reverse_error}")
      endif()
      if(NOT _reverse_result EQUAL 0)
        message(FATAL_ERROR "Failed to roll back patch: ${_patch_file}")
      endif()
      message(STATUS "Rolled back patch: ${_patch_file}")
    elseif(require_all)
      message(
        FATAL_ERROR
          "Cannot cleanly roll back patch: ${_patch_file}. ${DEPENDENCY} source is left in an unverified state."
      )
    endif()
  endwhile()
endfunction()

function(klogg_rollback_applied_patches)
  if(_applied_patches)
    klogg_reverse_patch_list("${_applied_patches}" TRUE)
    klogg_source_tree_sha256("${REPO_DIR}" _post_rollback_hash)
    if(NOT "${_post_rollback_hash}" STREQUAL "${CLEAN_TREE_HASH}")
      message(
        FATAL_ERROR
          "${DEPENDENCY} rollback produced ${_post_rollback_hash}, expected approved clean tree ${CLEAN_TREE_HASH}"
      )
    endif()
  endif()
endfunction()

get_filename_component(REPO_DIR "${REPO_DIR}" REALPATH)
get_filename_component(REPO_PARENT_DIR "${REPO_DIR}" DIRECTORY)
include("${CMAKE_CURRENT_LIST_DIR}/verify_pinned_source.cmake")
get_filename_component(_repo_name "${REPO_DIR}" NAME)
set(_private_marker "${REPO_PARENT_DIR}/.${_repo_name}.klogg-private-source")
# Classification and namespace checks read only; integrity/patch verification
# stays under the transaction lock below. No lock writes precede namespace trust.
if(EXISTS "${_private_marker}" OR IS_SYMLINK "${_private_marker}")
  if(NOT DEFINED PREPARE_PYTHON_EXECUTABLE)
    find_package(Python3 3.8 REQUIRED COMPONENTS Interpreter)
    set(PREPARE_PYTHON_EXECUTABLE "${Python3_EXECUTABLE}")
  endif()
  get_filename_component(_snapshot_script
                         "${CMAKE_CURRENT_LIST_DIR}/../scripts/prepare_efsw_snapshot.py" ABSOLUTE)
  klogg_private_source_kind("${_private_marker}" "${PREPARE_PYTHON_EXECUTABLE}"
                           "${_snapshot_script}" _prelock_kind)
  if(UNIX OR "${_prelock_kind}" STREQUAL "approved-content-snapshot")
    execute_process(
      COMMAND "${PREPARE_PYTHON_EXECUTABLE}" -I -S "${_snapshot_script}"
              --operation namespace --source "${REPO_PARENT_DIR}"
      RESULT_VARIABLE _namespace_result
      ERROR_VARIABLE _namespace_error
      TIMEOUT 30
    )
    if(NOT _namespace_result EQUAL 0)
      message(FATAL_ERROR "${DEPENDENCY} snapshot namespace rejected: ${_namespace_error}")
    endif()
  endif()
endif()
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
get_filename_component(_repo_name "${REPO_DIR}" NAME)
set(_private_marker "${REPO_PARENT_DIR}/.${_repo_name}.klogg-private-source")
if(EXISTS "${_private_marker}" OR IS_SYMLINK "${_private_marker}")
  if(NOT DEFINED PREPARE_PYTHON_EXECUTABLE)
    find_package(Python3 3.8 REQUIRED COMPONENTS Interpreter)
    set(PREPARE_PYTHON_EXECUTABLE "${Python3_EXECUTABLE}")
  endif()
  get_filename_component(_snapshot_script
                         "${CMAKE_CURRENT_LIST_DIR}/../scripts/prepare_efsw_snapshot.py" ABSOLUTE)
  klogg_private_source_kind("${_private_marker}" "${PREPARE_PYTHON_EXECUTABLE}"
                           "${_snapshot_script}" _source_kind)
  if("${_source_kind}" STREQUAL "approved-content-snapshot")
    if(NOT DEFINED GIT_TIMEOUT)
      set(GIT_TIMEOUT 120)
    endif()
    execute_process(
      COMMAND "${PREPARE_PYTHON_EXECUTABLE}" -I -S "${_snapshot_script}"
              --operation verify --source "${REPO_DIR}" --marker "${_private_marker}"
              --revision "${EXPECTED_REVISION}" --clean-tree "${CLEAN_TREE_HASH}"
              "--final-tree=${PATCHED_TREE_HASH}"
      RESULT_VARIABLE _snapshot_result
      ERROR_VARIABLE _snapshot_error
      TIMEOUT ${GIT_TIMEOUT}
    )
    if(NOT _snapshot_result EQUAL 0)
      message(FATAL_ERROR "${DEPENDENCY} snapshot patch input rejected: ${_snapshot_error}")
    endif()
    get_filename_component(_git_owner_script
                           "${CMAKE_CURRENT_LIST_DIR}/../scripts/prepare_efsw_git.py" ABSOLUTE)
    _klogg_owned_git_startup_arguments(_startup_arguments)
    set(_patch_command "${PREPARE_PYTHON_EXECUTABLE}" -I -S "${_git_owner_script}"
        ${_startup_arguments} --timeout "${GIT_TIMEOUT}" --operation snapshot-patch
        --ceiling "${REPO_PARENT_DIR}" -- -c core.autocrlf=false)
    set(_snapshot_mode TRUE)
  else()
    if(NOT DEFINED GIT_TIMEOUT)
      set(GIT_TIMEOUT 120)
    endif()
    get_filename_component(_git_owner_script
                           "${CMAKE_CURRENT_LIST_DIR}/../scripts/prepare_efsw_git.py" ABSOLUTE)
    klogg_require_private_git_source("${DEPENDENCY}" "${REPO_DIR}" "${EXPECTED_REVISION}"
                                    "${_private_marker}" "${PREPARE_PYTHON_EXECUTABLE}"
                                    "${_git_owner_script}" "${GIT_TIMEOUT}")
  endif()
endif()

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
  klogg_reverse_patch_list("${PATCH_FILES}" FALSE)
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
    COMMAND ${_patch_command} apply --check "${PATCH_FILE}"
    WORKING_DIRECTORY "${REPO_DIR}"
    RESULT_VARIABLE patch_check_result
    ERROR_VARIABLE _patch_check_error
  )
  klogg_patch_require_owned_completion("${patch_check_result}" "${_patch_check_error}")
  if(NOT patch_check_result EQUAL 0)
    # Report the first error before recovery: rollback may itself terminate,
    # and ordinary Git failure is not necessarily a patch context mismatch.
    if(_patch_check_error)
      message("${_patch_check_error}")
    endif()
    klogg_rollback_applied_patches()
    message(FATAL_ERROR
            "Failed to check patch: ${PATCH_FILE}. "
            "The dependency source was restored to its approved clean state.")
  endif()

  execute_process(
    COMMAND ${_patch_command} apply "${PATCH_FILE}"
    WORKING_DIRECTORY "${REPO_DIR}"
    RESULT_VARIABLE patch_apply_result
    ERROR_VARIABLE _patch_apply_error
  )
  klogg_patch_require_owned_completion("${patch_apply_result}" "${_patch_apply_error}")
  if(_patch_apply_error)
    message("${_patch_apply_error}")
  endif()
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
