# Materialize a verified clean dependency checkout for build-private patching.
#
# CACHE_SOURCE is a shared CPM cache and is never modified. DESTINATION is a
# build-private checkout that retains Git metadata so the patch helper can
# verify and recover its patch series.

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

include("${CMAKE_CURRENT_LIST_DIR}/verify_pinned_source.cmake")

# A shared cache may be clean or contain a previously approved patch series.
# The clone and detached checkout below intentionally use its pinned commit,
# not its working tree bytes.
klogg_require_pinned_source(
  "${DEPENDENCY}"
  "${CACHE_SOURCE}"
  "${EXPECTED_REVISION}"
  ${APPROVED_CACHE_TREE_HASHES}
)
get_filename_component(CACHE_SOURCE "${CACHE_SOURCE}" REALPATH)

# Resolve the destination through its nearest existing ancestor. This handles
# a destination below a symlink while retaining CMake 3.14 compatibility.
set(_destination_existing_ancestor "${DESTINATION}")
while(NOT EXISTS "${_destination_existing_ancestor}")
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

# The lock belongs to the private destination parent, never to the shared
# source tree or its content-addressed cache location.
file(MAKE_DIRECTORY "${_destination_parent}")
file(LOCK "${_destination_parent}/klogg-${DEPENDENCY}-prepare.lock"
     GUARD PROCESS
     TIMEOUT ${LOCK_TIMEOUT}
     RESULT_VARIABLE _prepare_lock_result)
if(NOT _prepare_lock_result EQUAL 0)
  message(FATAL_ERROR
          "Failed to lock ${DEPENDENCY} private source parent: ${_prepare_lock_result}")
endif()

if(EXISTS "${DESTINATION}")
  if(NOT IS_DIRECTORY "${DESTINATION}")
    message(FATAL_ERROR
            "${DEPENDENCY} private destination already exists and is not an approved clean private source: '${DESTINATION}'")
  endif()

  # Reuse only private Git metadata. A redirected worktree or shared Git dir
  # could otherwise let the later patch helper alter the shared cache.
  if(NOT IS_DIRECTORY "${DESTINATION}/.git" OR IS_SYMLINK "${DESTINATION}/.git")
    message(FATAL_ERROR
            "${DEPENDENCY} private destination does not retain usable Git metadata: '${DESTINATION}'")
  endif()
  get_filename_component(_expected_git_dir "${DESTINATION}/.git" ABSOLUTE)
  execute_process(
    COMMAND git -C "${DESTINATION}" rev-parse --show-toplevel
    RESULT_VARIABLE _destination_top_level_result
    OUTPUT_VARIABLE _destination_top_level
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    TIMEOUT ${GIT_TIMEOUT}
  )
  if(NOT _destination_top_level_result EQUAL 0
     OR NOT "${_destination_top_level}" STREQUAL "${DESTINATION}")
    message(FATAL_ERROR
            "${DEPENDENCY} private destination Git worktree is not private: '${DESTINATION}'")
  endif()
  execute_process(
    COMMAND git -C "${DESTINATION}" rev-parse --absolute-git-dir
    RESULT_VARIABLE _destination_git_dir_result
    OUTPUT_VARIABLE _destination_git_dir
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    TIMEOUT ${GIT_TIMEOUT}
  )
  if(NOT _destination_git_dir_result EQUAL 0
     OR NOT "${_destination_git_dir}" STREQUAL "${_expected_git_dir}")
    message(FATAL_ERROR
            "${DEPENDENCY} private destination Git metadata is not private: '${DESTINATION}'")
  endif()
  execute_process(
    COMMAND git -C "${DESTINATION}" rev-parse HEAD
    RESULT_VARIABLE _destination_revision_result
    OUTPUT_VARIABLE _destination_revision
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    TIMEOUT ${GIT_TIMEOUT}
  )
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
while(_staging_attempt LESS 16)
  string(RANDOM LENGTH 16 ALPHABET 0123456789abcdef _staging_suffix)
  set(_staging_directory
      "${_destination_parent}/.${_destination_name}.prepare-${_staging_suffix}")
  if(NOT EXISTS "${_staging_directory}")
    break()
  endif()
  math(EXPR _staging_attempt "${_staging_attempt} + 1")
endwhile()
if(EXISTS "${_staging_directory}")
  message(FATAL_ERROR "Failed to allocate unique ${DEPENDENCY} private source staging directory")
endif()

execute_process(
  COMMAND git -c core.autocrlf=false clone --config core.autocrlf=false
          --no-hardlinks --no-checkout "${CACHE_SOURCE}" "${_staging_directory}"
  RESULT_VARIABLE _clone_result
  ERROR_VARIABLE _clone_error
  TIMEOUT ${GIT_TIMEOUT}
)
if(NOT _clone_result EQUAL 0)
  file(REMOVE_RECURSE "${_staging_directory}")
  message(FATAL_ERROR
          "Failed to clone ${DEPENDENCY} cache source into private staging: ${_clone_error}")
endif()

execute_process(
  COMMAND git -c core.autocrlf=false -C "${_staging_directory}" checkout --detach
          "${EXPECTED_REVISION}"
  RESULT_VARIABLE _checkout_result
  ERROR_VARIABLE _checkout_error
  TIMEOUT ${GIT_TIMEOUT}
)
if(NOT _checkout_result EQUAL 0)
  file(REMOVE_RECURSE "${_staging_directory}")
  message(FATAL_ERROR
          "Failed to check out pinned ${DEPENDENCY} revision in private staging: ${_checkout_error}")
endif()

execute_process(
  COMMAND git -C "${_staging_directory}" rev-parse HEAD
  RESULT_VARIABLE _staged_revision_result
  OUTPUT_VARIABLE _staged_revision
  OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_QUIET
  TIMEOUT ${GIT_TIMEOUT}
)
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
file(WRITE "${_destination_marker}" "${EXPECTED_REVISION}\n")
if(EXISTS "${DESTINATION}")
  file(REMOVE_RECURSE "${_staging_directory}")
  file(REMOVE "${_destination_marker}")
  message(FATAL_ERROR
          "${DEPENDENCY} private destination appeared during preparation: '${DESTINATION}'")
endif()
file(RENAME "${_staging_directory}" "${DESTINATION}")
message(STATUS "Prepared clean private ${DEPENDENCY} source at '${DESTINATION}'")
