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

function(klogg_require_pinned_revision dependency source_dir expected_revision)
  if(NOT IS_DIRECTORY "${source_dir}")
    message(FATAL_ERROR "${dependency} source directory does not exist: '${source_dir}'")
  endif()

  if(EXISTS "${source_dir}/.git")
    execute_process(
      COMMAND git -C "${source_dir}" rev-parse HEAD
      RESULT_VARIABLE _revision_result
      OUTPUT_VARIABLE _actual_revision
      OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_QUIET
    )
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
  klogg_require_pinned_revision("${dependency}" "${source_dir}" "${expected_revision}")

  if(NOT ARGN)
    message(FATAL_ERROR "${dependency} source verification requires approved tree SHA-256 digests")
  endif()

  klogg_source_tree_sha256("${source_dir}" _actual_tree_hash)
  set(_approved_tree_hashes ${ARGN})
  list(FIND _approved_tree_hashes "${_actual_tree_hash}" _approved_tree_index)
  if(_approved_tree_index EQUAL -1)
    message(
      FATAL_ERROR
        "${dependency} source tree SHA-256 mismatch: got ${_actual_tree_hash} from '${source_dir}'"
    )
  endif()
endfunction()
