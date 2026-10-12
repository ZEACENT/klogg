# Verify patched dependency sources before applying local patches.
#
# CPM source-cache artifacts may omit .git metadata. Verify the exact Git
# revision when metadata exists, then always verify a deterministic SHA-256
# digest of the complete source tree (excluding .git). Callers provide the
# approved clean and locally-patched tree digests so repeated configurations
# remain idempotent without accepting any other dirty cache state.
function(klogg_source_tree_sha256 source_dir output_variable)
  file(GLOB_RECURSE _source_files LIST_DIRECTORIES false RELATIVE "${source_dir}"
       "${source_dir}/*")
  list(FILTER _source_files EXCLUDE REGEX "^\\.git(/|$)")
  list(SORT _source_files)

  set(_manifest "")
  foreach(_source_file IN LISTS _source_files)
    file(SHA256 "${source_dir}/${_source_file}" _source_hash)
    set(_manifest "${_manifest}${_source_file}:${_source_hash}\n")
  endforeach()
  string(SHA256 _tree_hash "${_manifest}")
  set(${output_variable} "${_tree_hash}" PARENT_SCOPE)
endfunction()

# Python can coerce LC_CTYPE during startup. Capture the CMake-side state and
# preserve a present value as one equals-form argument, including an empty one.
# Escape only the CMake list separator; never modify the caller environment.
function(_klogg_owned_git_startup_arguments output_variable)
  if(DEFINED ENV{LC_CTYPE})
    string(REPLACE ";" "\\;" _locale_value "$ENV{LC_CTYPE}")
    set(_arguments --startup-lc-ctype-state present "--startup-lc-ctype=${_locale_value}")
  else()
    set(_arguments --startup-lc-ctype-state absent)
  endif()
  set(${output_variable} "${_arguments}" PARENT_SCOPE)
endfunction()

function(klogg_require_pinned_revision dependency source_dir expected_revision)
  # Preparation explicitly supplies its command owner. All other callers keep
  # the legacy revision probe and incomplete-metadata fallback unchanged.
  cmake_parse_arguments(PARSE_ARGV 3 _revision ""
                        "GIT_OWNER;PYTHON_EXECUTABLE;GIT_TIMEOUT" "")
  if(NOT IS_DIRECTORY "${source_dir}")
    message(FATAL_ERROR "${dependency} source directory does not exist: '${source_dir}'")
  endif()

  if(EXISTS "${source_dir}/.git")
    if(_revision_GIT_OWNER)
      if(NOT _revision_PYTHON_EXECUTABLE OR NOT _revision_GIT_TIMEOUT)
        message(FATAL_ERROR "Owned Git revision verification requires an interpreter and timeout")
      endif()
      _klogg_owned_git_startup_arguments(_startup_arguments)
      execute_process(
        COMMAND "${_revision_PYTHON_EXECUTABLE}" -I -S "${_revision_GIT_OWNER}"
                ${_startup_arguments}
                --timeout "${_revision_GIT_TIMEOUT}" --operation inherit
                -- -C "${source_dir}" rev-parse HEAD
        RESULT_VARIABLE _revision_result
        OUTPUT_VARIABLE _actual_revision
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_VARIABLE _revision_error
      )
      # Only an ordinary Git failure may use the approved-tree fallback.
      # Owner timeout/spawn/containment/cleanup failures are distinct and fatal.
      if(NOT _revision_result EQUAL 0 AND NOT _revision_result EQUAL 1)
        message(FATAL_ERROR
                "${dependency} owned Git revision verification failed (status ${_revision_result}): ${_revision_error}")
      endif()
    else()
      execute_process(
        COMMAND git -C "${source_dir}" rev-parse HEAD
        RESULT_VARIABLE _revision_result
        OUTPUT_VARIABLE _actual_revision
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
      )
    endif()
    if(_revision_result EQUAL 0)
      if(NOT "${_actual_revision}" STREQUAL "${expected_revision}")
        message(
          FATAL_ERROR
            "${dependency} source must be pinned at ${expected_revision}, got '${_actual_revision}' from '${source_dir}'"
        )
      endif()
    else()
      message(
        STATUS
          "${dependency} Git metadata is incomplete; verifying the exact approved source-tree digest"
      )
    endif()
  endif()
endfunction()

function(klogg_require_pinned_source dependency source_dir expected_revision)
  cmake_parse_arguments(PARSE_ARGV 3 _source ""
                        "GIT_OWNER;PYTHON_EXECUTABLE;GIT_TIMEOUT" "")
  if(_source_GIT_OWNER)
    klogg_require_pinned_revision(
      "${dependency}" "${source_dir}" "${expected_revision}"
      GIT_OWNER "${_source_GIT_OWNER}"
      PYTHON_EXECUTABLE "${_source_PYTHON_EXECUTABLE}"
      GIT_TIMEOUT "${_source_GIT_TIMEOUT}"
    )
  else()
    klogg_require_pinned_revision("${dependency}" "${source_dir}" "${expected_revision}")
  endif()

  if(NOT _source_UNPARSED_ARGUMENTS)
    message(FATAL_ERROR "${dependency} source verification requires approved tree SHA-256 digests")
  endif()

  klogg_source_tree_sha256("${source_dir}" _actual_tree_hash)
  set(_approved_tree_hashes ${_source_UNPARSED_ARGUMENTS})
  list(FIND _approved_tree_hashes "${_actual_tree_hash}" _approved_tree_index)
  if(_approved_tree_index EQUAL -1)
    message(
      FATAL_ERROR
        "${dependency} source tree SHA-256 mismatch: got ${_actual_tree_hash} from '${source_dir}'"
    )
  endif()
endfunction()

# Verify dependency bytes before evaluating their CMakeLists.txt. This ordering
# is a security boundary for source-cache artifacts: a mismatched dependency
# must not execute CMake code before the integrity gate rejects it.
function(klogg_add_verified_subdirectory dependency source_dir binary_dir expected_revision)
  if(NOT ARGN)
    message(FATAL_ERROR "${dependency} verified subdirectory requires approved tree digests")
  endif()

  klogg_require_pinned_source(
    "${dependency}" "${source_dir}" "${expected_revision}" ${ARGN}
  )
  add_subdirectory("${source_dir}" "${binary_dir}" EXCLUDE_FROM_ALL)
endfunction()
