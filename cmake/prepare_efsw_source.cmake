# Materialize a verified clean dependency checkout for build-private patching.
#
# CACHE_SOURCE is a shared CPM cache and is never modified. DESTINATION is a
# build-private pinned checkout or explicitly typed approved-content snapshot.
# Snapshots never claim the approved revision as an actual private Git commit.

foreach(_required_variable
    DEPENDENCY
    CACHE_SOURCE
    DESTINATION
    EXPECTED_REVISION
    CLEAN_TREE_HASH
    APPROVED_CACHE_TREE_HASHES)
  if(NOT DEFINED ${_required_variable} OR "${${_required_variable}}" STREQUAL "")
    message(FATAL_ERROR "${_required_variable} is required")
  endif()
endforeach()

if(NOT DEFINED LOCK_TIMEOUT)
  set(LOCK_TIMEOUT 120)
endif()
if(NOT DEFINED GIT_TIMEOUT)
  set(GIT_TIMEOUT 120)
endif()

get_filename_component(CACHE_SOURCE "${CACHE_SOURCE}" ABSOLUTE)
get_filename_component(DESTINATION "${DESTINATION}" ABSOLUTE)
set(_destination_leaf_is_symlink FALSE)
if(IS_SYMLINK "${DESTINATION}")
  set(_destination_leaf_is_symlink TRUE)
endif()
if(NOT DEFINED PATCHED_TREE_HASH)
  set(PATCHED_TREE_HASH "")
endif()

# Standalone script mode does not inherit a sibling directory's interpreter.
# Validate the explicit host prerequisite before starting any Git operation.
if(NOT DEFINED PREPARE_PYTHON_EXECUTABLE OR "${PREPARE_PYTHON_EXECUTABLE}" STREQUAL "")
  find_package(Python3 3.8 QUIET COMPONENTS Interpreter)
  if(NOT Python3_Interpreter_FOUND)
    message(FATAL_ERROR "efsw preparation requires a host Python interpreter >= 3.8")
  endif()
  set(PREPARE_PYTHON_EXECUTABLE "${Python3_EXECUTABLE}")
endif()
if(NOT IS_ABSOLUTE "${PREPARE_PYTHON_EXECUTABLE}")
  message(FATAL_ERROR "efsw preparation Python interpreter must be an absolute path")
endif()
if(NOT EXISTS "${PREPARE_PYTHON_EXECUTABLE}" OR IS_DIRECTORY "${PREPARE_PYTHON_EXECUTABLE}")
  message(FATAL_ERROR "efsw preparation Python interpreter is unavailable: '${PREPARE_PYTHON_EXECUTABLE}'")
endif()
get_filename_component(_git_owner_script
                       "${CMAKE_CURRENT_LIST_DIR}/../scripts/prepare_efsw_git.py" ABSOLUTE)
execute_process(
  COMMAND "${PREPARE_PYTHON_EXECUTABLE}" -I -S "${_git_owner_script}" --check-interpreter
  RESULT_VARIABLE _interpreter_result
  OUTPUT_QUIET
  ERROR_VARIABLE _interpreter_error
)
if(NOT _interpreter_result EQUAL 0)
  message(FATAL_ERROR
          "efsw preparation Python interpreter check failed: ${_interpreter_error}")
endif()

include("${CMAKE_CURRENT_LIST_DIR}/verify_pinned_source.cmake")
_klogg_owned_git_startup_arguments(_git_startup_arguments)

# Status 5 means family quiescence was not established. Never delete or publish
# private state that an incompletely contained command may still be using.
function(_klogg_prepare_require_owned_completion result diagnostic)
  if("${result}" STREQUAL "5")
    message(FATAL_ERROR
            "Owned ${DEPENDENCY} Git cleanup incomplete; preserving private source state: ${diagnostic}")
  endif()
endfunction()

get_filename_component(_snapshot_script
                       "${CMAKE_CURRENT_LIST_DIR}/../scripts/prepare_efsw_snapshot.py" ABSOLUTE)
function(klogg_prepare_write_private_marker kind)
  if(UNIX OR "${kind}" STREQUAL "approved-content-snapshot")
    execute_process(
      COMMAND "${PREPARE_PYTHON_EXECUTABLE}" -I -S "${_snapshot_script}"
              --operation write-marker --kind "${kind}" --marker "${_destination_marker}"
              --revision "${EXPECTED_REVISION}" --clean-tree "${CLEAN_TREE_HASH}"
              "--final-tree=${PATCHED_TREE_HASH}"
      RESULT_VARIABLE _marker_result
      ERROR_VARIABLE _marker_error
      TIMEOUT ${GIT_TIMEOUT}
    )
    if(NOT _marker_result EQUAL 0)
      message(FATAL_ERROR "Failed to write owned private marker (status ${_marker_result}): ${_marker_error}")
    endif()
  else()
    file(WRITE "${_destination_marker}" "${EXPECTED_REVISION}\n")
  endif()
endfunction()

# Reject source links/specials before CMake's digest walker can follow or open
# them. Normal Windows Git materialization retains its existing contract.
if(UNIX AND IS_DIRECTORY "${CACHE_SOURCE}")
  get_filename_component(_preflight_source "${CACHE_SOURCE}" REALPATH)
  execute_process(
    COMMAND "${PREPARE_PYTHON_EXECUTABLE}" -I -S "${_snapshot_script}"
            --operation preflight --source "${_preflight_source}"
    RESULT_VARIABLE _preflight_result
    ERROR_VARIABLE _preflight_error
    TIMEOUT ${GIT_TIMEOUT}
  )
  if(NOT _preflight_result EQUAL 0)
    message(FATAL_ERROR "${DEPENDENCY} source preflight failed: ${_preflight_error}")
  endif()
endif()

# A shared cache may be clean or contain a previously approved patch series.
# The clone and detached checkout below intentionally use its pinned commit,
# not its working tree bytes.
klogg_require_pinned_source(
  "${DEPENDENCY}"
  "${CACHE_SOURCE}"
  "${EXPECTED_REVISION}"
  GIT_OWNER "${_git_owner_script}"
  PYTHON_EXECUTABLE "${PREPARE_PYTHON_EXECUTABLE}"
  GIT_TIMEOUT "${GIT_TIMEOUT}"
  VERIFICATION_MODE_OUTPUT _cache_verification_mode
  ${APPROVED_CACHE_TREE_HASHES}
)
get_filename_component(CACHE_SOURCE "${CACHE_SOURCE}" REALPATH)

# Trust only the verified cache's own Git directory for the local clone.
# Never resolve a gitfile, symlink, or common-directory redirect into an
# ownership exception for metadata outside this source tree.
if((EXISTS "${CACHE_SOURCE}/.git" OR IS_SYMLINK "${CACHE_SOURCE}/.git")
   AND (NOT IS_DIRECTORY "${CACHE_SOURCE}/.git" OR IS_SYMLINK "${CACHE_SOURCE}/.git"
        OR EXISTS "${CACHE_SOURCE}/.git/commondir"
        OR IS_SYMLINK "${CACHE_SOURCE}/.git/commondir"))
  message(FATAL_ERROR
          "${DEPENDENCY} cache source does not retain ordinary Git metadata: '${CACHE_SOURCE}'")
endif()

# Resolve the destination through its nearest existing ancestor. This handles
# a destination below a symlink while retaining CMake 3.14 compatibility.
set(_destination_existing_ancestor "${DESTINATION}")
while(NOT EXISTS "${_destination_existing_ancestor}")
  # EXISTS follows links. Never append an unresolved dangling alias to the
  # approved spelling: later realpath-based creation could enter the cache.
  if(IS_SYMLINK "${_destination_existing_ancestor}")
    message(FATAL_ERROR "Cannot resolve dangling symlink in private destination '${DESTINATION}'")
  endif()
  get_filename_component(_destination_parent_candidate
                         "${_destination_existing_ancestor}" DIRECTORY)
  if("${_destination_parent_candidate}" STREQUAL "${_destination_existing_ancestor}")
    message(FATAL_ERROR "Cannot resolve private destination '${DESTINATION}'")
  endif()
  set(_destination_existing_ancestor "${_destination_parent_candidate}")
endwhile()
get_filename_component(_destination_existing_realpath
                       "${_destination_existing_ancestor}" REALPATH)
file(RELATIVE_PATH _destination_remainder
     "${_destination_existing_ancestor}" "${DESTINATION}")
if("${_destination_remainder}" STREQUAL "" OR "${_destination_remainder}" STREQUAL ".")
  set(_canonical_destination "${_destination_existing_realpath}")
else()
  set(_canonical_destination
      "${_destination_existing_realpath}/${_destination_remainder}")
endif()

function(klogg_path_is_at_or_below parent_path child_path output_variable)
  file(RELATIVE_PATH _path_remainder "${parent_path}" "${child_path}")
  # On Windows, distinct drive roots can yield an absolute path. It is not a
  # relative child and must not be accepted as one.
  if(IS_ABSOLUTE "${_path_remainder}")
    set(${output_variable} FALSE PARENT_SCOPE)
  elseif("${_path_remainder}" STREQUAL "" OR "${_path_remainder}" STREQUAL "."
         OR NOT "${_path_remainder}" MATCHES "^\\.\\.(/|$)")
    set(${output_variable} TRUE PARENT_SCOPE)
  else()
    set(${output_variable} FALSE PARENT_SCOPE)
  endif()
endfunction()

klogg_path_is_at_or_below("${CACHE_SOURCE}" "${_canonical_destination}"
                          _destination_within_cache)
klogg_path_is_at_or_below("${_canonical_destination}" "${CACHE_SOURCE}"
                          _cache_within_destination)
if(_destination_within_cache OR _cache_within_destination)
  message(FATAL_ERROR
          "${DEPENDENCY} private destination must not overlap the shared cache source")
endif()

set(DESTINATION "${_canonical_destination}")
get_filename_component(_destination_parent "${DESTINATION}" DIRECTORY)
get_filename_component(_destination_name "${DESTINATION}" NAME)
set(_destination_marker
    "${_destination_parent}/.${_destination_name}.klogg-private-source")

# Snapshot namespace approval precedes even lock-file writes. Descriptor/leaf
# modes alone cannot protect a private path under a foreign-writable ancestor.
if(IS_SYMLINK "${_destination_marker}" OR IS_DIRECTORY "${_destination_marker}")
  message(FATAL_ERROR "${DEPENDENCY} private source marker must not redirect outside its owned parent")
endif()
set(_snapshot_namespace_required FALSE)
if("${_cache_verification_mode}" STREQUAL "approved-content")
  set(_snapshot_namespace_required TRUE)
endif()
if(EXISTS "${_destination_marker}")
  klogg_private_source_kind("${_destination_marker}" "${PREPARE_PYTHON_EXECUTABLE}"
                           "${_snapshot_script}" _existing_kind)
  if("${_existing_kind}" STREQUAL "approved-content-snapshot")
    set(_snapshot_namespace_required TRUE)
  endif()
endif()
if(UNIX OR _snapshot_namespace_required)
  execute_process(
    COMMAND "${PREPARE_PYTHON_EXECUTABLE}" -I -S "${_snapshot_script}"
            --operation prepare-parent --source "${_destination_parent}"
    RESULT_VARIABLE _namespace_result
    ERROR_VARIABLE _namespace_error
    TIMEOUT ${GIT_TIMEOUT}
  )
  if(NOT _namespace_result EQUAL 0)
    message(FATAL_ERROR "${DEPENDENCY} snapshot namespace rejected: ${_namespace_error}")
  endif()
else()
  file(MAKE_DIRECTORY "${_destination_parent}")
endif()

# The lock belongs to the verified actual private destination parent, never to
# the shared source tree. Fresh POSIX parents are already canonical and private.
file(LOCK "${_destination_parent}/klogg-${DEPENDENCY}-prepare.lock"
     GUARD PROCESS
     TIMEOUT ${LOCK_TIMEOUT}
     RESULT_VARIABLE _prepare_lock_result)
if(NOT _prepare_lock_result EQUAL 0)
  message(FATAL_ERROR
          "Failed to lock ${DEPENDENCY} private source parent: ${_prepare_lock_result}")
endif()

if(IS_SYMLINK "${_destination_marker}" OR IS_DIRECTORY "${_destination_marker}")
  message(FATAL_ERROR "${DEPENDENCY} private source marker must not redirect outside its owned parent")
endif()

if(EXISTS "${DESTINATION}")
  if(NOT IS_DIRECTORY "${DESTINATION}")
    message(FATAL_ERROR
            "${DEPENDENCY} private destination already exists and is not an approved clean private source: '${DESTINATION}'")
  endif()

  if(EXISTS "${_destination_marker}" OR IS_SYMLINK "${_destination_marker}")
    klogg_private_source_kind("${_destination_marker}" "${PREPARE_PYTHON_EXECUTABLE}"
                             "${_snapshot_script}" _destination_kind)
    if("${_destination_kind}" STREQUAL "approved-content-snapshot")
      if(_destination_leaf_is_symlink)
        message(FATAL_ERROR "Snapshot private destination must not be a symlink")
      endif()
      execute_process(
        COMMAND "${PREPARE_PYTHON_EXECUTABLE}" -I -S "${_snapshot_script}"
                --operation verify --source "${DESTINATION}" --marker "${_destination_marker}"
                --revision "${EXPECTED_REVISION}" --clean-tree "${CLEAN_TREE_HASH}"
                "--final-tree=${PATCHED_TREE_HASH}"
        RESULT_VARIABLE _snapshot_result
        ERROR_VARIABLE _snapshot_error
        TIMEOUT ${GIT_TIMEOUT}
      )
      if(NOT _snapshot_result EQUAL 0)
        message(FATAL_ERROR "${DEPENDENCY} private snapshot reuse rejected: ${_snapshot_error}")
      endif()
      message(STATUS "${DEPENDENCY} approved-content snapshot already prepared for patch series")
      return()
    endif()
  endif()

  # Reuse only private Git metadata. A redirected worktree or shared Git dir
  # could otherwise let the later patch helper alter the shared cache.
  if(NOT IS_DIRECTORY "${DESTINATION}/.git" OR IS_SYMLINK "${DESTINATION}/.git")
    message(FATAL_ERROR
            "${DEPENDENCY} private destination does not retain usable Git metadata: '${DESTINATION}'")
  endif()
  get_filename_component(_expected_git_dir "${DESTINATION}/.git" ABSOLUTE)
  execute_process(
    COMMAND "${PREPARE_PYTHON_EXECUTABLE}" -I -S "${_git_owner_script}"
            ${_git_startup_arguments}
            --timeout "${GIT_TIMEOUT}" --operation inherit -- -C "${DESTINATION}" rev-parse --show-toplevel
    RESULT_VARIABLE _destination_top_level_result
    OUTPUT_VARIABLE _destination_top_level
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_VARIABLE _destination_top_level_error
  )
  _klogg_prepare_require_owned_completion("${_destination_top_level_result}" "${_destination_top_level_error}")
  if(NOT _destination_top_level_result EQUAL 0
     OR NOT "${_destination_top_level}" STREQUAL "${DESTINATION}")
    message(FATAL_ERROR
            "${DEPENDENCY} private destination Git worktree is not private: '${DESTINATION}'")
  endif()
  execute_process(
    COMMAND "${PREPARE_PYTHON_EXECUTABLE}" -I -S "${_git_owner_script}"
            ${_git_startup_arguments}
            --timeout "${GIT_TIMEOUT}" --operation inherit -- -C "${DESTINATION}" rev-parse --absolute-git-dir
    RESULT_VARIABLE _destination_git_dir_result
    OUTPUT_VARIABLE _destination_git_dir
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_VARIABLE _destination_git_dir_error
  )
  _klogg_prepare_require_owned_completion("${_destination_git_dir_result}" "${_destination_git_dir_error}")
  if(NOT _destination_git_dir_result EQUAL 0
     OR NOT "${_destination_git_dir}" STREQUAL "${_expected_git_dir}")
    message(FATAL_ERROR
            "${DEPENDENCY} private destination Git metadata is not private: '${DESTINATION}'")
  endif()
  execute_process(
    COMMAND "${PREPARE_PYTHON_EXECUTABLE}" -I -S "${_git_owner_script}"
            ${_git_startup_arguments}
            --timeout "${GIT_TIMEOUT}" --operation inherit -- -C "${DESTINATION}" rev-parse HEAD
    RESULT_VARIABLE _destination_revision_result
    OUTPUT_VARIABLE _destination_revision
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_VARIABLE _destination_revision_error
  )
  _klogg_prepare_require_owned_completion("${_destination_revision_result}" "${_destination_revision_error}")
  if(NOT _destination_revision_result EQUAL 0)
    message(FATAL_ERROR
            "${DEPENDENCY} private destination does not retain usable Git metadata: '${DESTINATION}'")
  endif()
  if(NOT "${_destination_revision}" STREQUAL "${EXPECTED_REVISION}")
    message(FATAL_ERROR
            "${DEPENDENCY} private destination must be pinned at ${EXPECTED_REVISION}, got '${_destination_revision}'")
  endif()

  if(NOT EXISTS "${_destination_marker}")
    message(FATAL_ERROR
            "${DEPENDENCY} private destination was not prepared by this helper: '${DESTINATION}'")
  endif()
  file(READ "${_destination_marker}" _marker_revision)
  if(NOT "${_marker_revision}" STREQUAL "${EXPECTED_REVISION}\n")
    message(FATAL_ERROR
            "${DEPENDENCY} private destination marker does not match the expected revision: '${DESTINATION}'")
  endif()

  # Do not inspect, reset, or delete an existing private worktree. The patch
  # helper owns recognition and recovery of its clean, patched, or interrupted
  # series state after this preparation boundary has reused the private pin.
  klogg_require_pinned_revision(
    "${DEPENDENCY}"
    "${DESTINATION}"
    "${EXPECTED_REVISION}"
    GIT_OWNER "${_git_owner_script}"
    PYTHON_EXECUTABLE "${PREPARE_PYTHON_EXECUTABLE}"
    GIT_TIMEOUT "${GIT_TIMEOUT}"
  )
  message(STATUS "${DEPENDENCY} private source already prepared for patch series")
  return()
endif()

# A marker left before a failed atomic publish identifies no source tree and
# must not prevent a later clean preparation.
if(EXISTS "${_destination_marker}")
  file(REMOVE "${_destination_marker}")
endif()

set(_staging_attempt 0)
set(_staging_directory "")
set(_clone_config_context "")
while(_staging_attempt LESS 16)
  string(RANDOM LENGTH 16 ALPHABET 0123456789abcdef _staging_suffix)
  set(_staging_directory
      "${_destination_parent}/.${_destination_name}.prepare-${_staging_suffix}")
  set(_clone_config_context
      "${_destination_parent}/.${_destination_name}.clone-config-${_staging_suffix}")
  if(EXISTS "${_staging_directory}" OR IS_SYMLINK "${_staging_directory}"
     OR EXISTS "${_clone_config_context}" OR IS_SYMLINK "${_clone_config_context}")
    math(EXPR _staging_attempt "${_staging_attempt} + 1")
  else()
    break()
  endif()
endwhile()
if(EXISTS "${_staging_directory}" OR IS_SYMLINK "${_staging_directory}"
   OR EXISTS "${_clone_config_context}" OR IS_SYMLINK "${_clone_config_context}")
  message(FATAL_ERROR
          "Failed to allocate unique ${DEPENDENCY} private source staging/config context")
endif()

# Select content materialization before clone, only after the shared verifier
# accepted absent/incomplete metadata. A failed readable-pin clone never falls
# back here. Only exact CLEAN bytes are captured, not an approved patched cache.
if("${_cache_verification_mode}" STREQUAL "approved-content")
  execute_process(
    COMMAND "${PREPARE_PYTHON_EXECUTABLE}" -I -S "${_snapshot_script}"
            --operation materialize --source "${CACHE_SOURCE}" --destination "${_staging_directory}"
            --revision "${EXPECTED_REVISION}" --clean-tree "${CLEAN_TREE_HASH}"
            "--final-tree=${PATCHED_TREE_HASH}"
    RESULT_VARIABLE _snapshot_result
    OUTPUT_QUIET
    ERROR_VARIABLE _snapshot_error
    TIMEOUT ${GIT_TIMEOUT}
  )
  # The single-process descriptor helper owns cleanup. Never recursively delete
  # its staging after a timeout, identity uncertainty or incomplete cleanup.
  if(NOT _snapshot_result EQUAL 0)
    message(FATAL_ERROR "Failed to capture clean ${DEPENDENCY} private snapshot: ${_snapshot_error}")
  endif()
  if(EXISTS "${DESTINATION}" OR IS_SYMLINK "${DESTINATION}")
    file(REMOVE_RECURSE "${_staging_directory}")
    message(FATAL_ERROR "${DEPENDENCY} private destination appeared during snapshot preparation")
  endif()
  klogg_prepare_write_private_marker("approved-content-snapshot")
  file(RENAME "${_staging_directory}" "${DESTINATION}")
  message(STATUS "Prepared clean private ${DEPENDENCY} approved-content snapshot at '${DESTINATION}'")
  return()
endif()

# Local transports can discard command-scope configuration before upload-pack.
# Select an owned, temporary GLOBAL-scope protected input only for the clone
# and its children. Do not modify the caller's global file or CMake environment.
file(MAKE_DIRECTORY "${_clone_config_context}")
set(_clone_config_file "${_clone_config_context}/gitconfig")
# Even --file config operations discover repositories. Construct the file in
# its owned context, with discovery bounded before any caller repository and
# without inherited Git-directory redirects; preserve the caller elsewhere.
execute_process(
  COMMAND "${PREPARE_PYTHON_EXECUTABLE}" -I -S "${_git_owner_script}"
          ${_git_startup_arguments}
          --timeout "${GIT_TIMEOUT}" --operation config --ceiling "${_destination_parent}"
          -- config --file "${_clone_config_file}" --add safe.directory ""
  WORKING_DIRECTORY "${_clone_config_context}"
  RESULT_VARIABLE _config_result
  ERROR_VARIABLE _config_error
)
_klogg_prepare_require_owned_completion("${_config_result}" "${_config_error}")
if(NOT _config_result EQUAL 0)
  file(REMOVE_RECURSE "${_clone_config_context}" "${_staging_directory}")
  message(FATAL_ERROR "Failed to configure ${DEPENDENCY} cache clone trust reset: ${_config_error}")
endif()
execute_process(
  COMMAND "${PREPARE_PYTHON_EXECUTABLE}" -I -S "${_git_owner_script}"
          ${_git_startup_arguments}
          --timeout "${GIT_TIMEOUT}" --operation config --ceiling "${_destination_parent}"
          -- config --file "${_clone_config_file}" --add safe.directory "${CACHE_SOURCE}/.git"
  WORKING_DIRECTORY "${_clone_config_context}"
  RESULT_VARIABLE _config_result
  ERROR_VARIABLE _config_error
)
_klogg_prepare_require_owned_completion("${_config_result}" "${_config_error}")
if(NOT _config_result EQUAL 0)
  file(REMOVE_RECURSE "${_clone_config_context}" "${_staging_directory}")
  message(FATAL_ERROR "Failed to configure ${DEPENDENCY} cache clone trust path: ${_config_error}")
endif()

execute_process(
  COMMAND "${PREPARE_PYTHON_EXECUTABLE}" -I -S "${_git_owner_script}"
          ${_git_startup_arguments}
          --timeout "${GIT_TIMEOUT}" --operation clone --global-config "${_clone_config_file}"
          -- -c core.autocrlf=false clone --config core.autocrlf=false
          --no-hardlinks --no-checkout "${CACHE_SOURCE}" "${_staging_directory}"
  RESULT_VARIABLE _clone_result
  ERROR_VARIABLE _clone_error
)
_klogg_prepare_require_owned_completion("${_clone_result}" "${_clone_error}")
# Also remove a Git config lock left by a handled failure. Abrupt termination
# can leave private residue; allocation above never reuses an occupied context.
file(REMOVE_RECURSE "${_clone_config_context}")
if(NOT _clone_result EQUAL 0)
  file(REMOVE_RECURSE "${_staging_directory}")
  message(FATAL_ERROR
          "Failed to clone ${DEPENDENCY} cache source into private staging: ${_clone_error}")
endif()

execute_process(
  COMMAND "${PREPARE_PYTHON_EXECUTABLE}" -I -S "${_git_owner_script}"
          ${_git_startup_arguments}
          --timeout "${GIT_TIMEOUT}" --operation inherit
          -- -c core.autocrlf=false -C "${_staging_directory}" checkout --detach
          "${EXPECTED_REVISION}"
  RESULT_VARIABLE _checkout_result
  ERROR_VARIABLE _checkout_error
)
_klogg_prepare_require_owned_completion("${_checkout_result}" "${_checkout_error}")
if(NOT _checkout_result EQUAL 0)
  file(REMOVE_RECURSE "${_staging_directory}")
  message(FATAL_ERROR
          "Failed to check out pinned ${DEPENDENCY} revision in private staging: ${_checkout_error}")
endif()

execute_process(
  COMMAND "${PREPARE_PYTHON_EXECUTABLE}" -I -S "${_git_owner_script}"
          ${_git_startup_arguments}
          --timeout "${GIT_TIMEOUT}" --operation inherit -- -C "${_staging_directory}" rev-parse HEAD
  RESULT_VARIABLE _staged_revision_result
  OUTPUT_VARIABLE _staged_revision
  OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_VARIABLE _staged_revision_error
)
_klogg_prepare_require_owned_completion("${_staged_revision_result}" "${_staged_revision_error}")
if(NOT _staged_revision_result EQUAL 0
   OR NOT "${_staged_revision}" STREQUAL "${EXPECTED_REVISION}")
  file(REMOVE_RECURSE "${_staging_directory}")
  message(FATAL_ERROR
          "Private ${DEPENDENCY} checkout is not pinned at ${EXPECTED_REVISION}")
endif()

klogg_source_tree_sha256("${_staging_directory}" _staged_tree_hash)
if(NOT "${_staged_tree_hash}" STREQUAL "${CLEAN_TREE_HASH}")
  file(REMOVE_RECURSE "${_staging_directory}")
  message(FATAL_ERROR
          "Private ${DEPENDENCY} checkout tree SHA-256 mismatch: got ${_staged_tree_hash}, expected ${CLEAN_TREE_HASH}")
endif()

# A cooperating concurrent preparation cannot replace an unknown destination:
# the parent lock covers the marker, final check, and atomic publish.
klogg_prepare_write_private_marker("pinned-git")
if(EXISTS "${DESTINATION}")
  file(REMOVE_RECURSE "${_staging_directory}")
  file(REMOVE "${_destination_marker}")
  message(FATAL_ERROR
          "${DEPENDENCY} private destination appeared during preparation: '${DESTINATION}'")
endif()
file(RENAME "${_staging_directory}" "${DESTINATION}")
message(STATUS "Prepared clean private ${DEPENDENCY} source at '${DESTINATION}'")
