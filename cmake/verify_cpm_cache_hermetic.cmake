# Verify that a configure adds or changes no shared CPM bootstrap artifacts.
#
# The shared CPM source cache (CPM_SOURCE_CACHE, i.e. cpm_cache/) is seeded only
# by cmake/prefetch_cpm: it fetches every pinned dependency source with
# DOWNLOAD_ONLY YES and therefore never evaluates a dependency's own
# CMakeLists.txt. That step is retried, and scripts/check_cpm_cache_contract.sh
# verifies the extracted sources afterwards.
#
# A dependency that manages its own dependencies during configure instead
# downloads its package-manager bootstrap into <CPM_SOURCE_CACHE>/cpm/, the
# directory CPM reserves for that bootstrap. CRoaring did exactly that: its
# vendored cmake/CPM.cmake fetched the CPM 0.38.6 release asset into that
# directory on every configure whose cache did not already hold a matching file.
# CMake skips that fetch only when the destination already matches EXPECTED_HASH,
# so a cold cache - for example right after a dependency edit rotated the CI
# cache key - made every job fetch it live. When GitHub's release CDN answered
# with an error the configure died with "file DOWNLOAD cannot compute hash on
# failed download" before any analysis ran (Static analysis master run
# 35614385003, 2026-09-21).
#
# Capturing the bootstrap artifacts before dependency processing and requiring
# them to stay unchanged turns that silent, timing-dependent network dependency
# into a deterministic configure failure that names the file and the remedy.
# A configure without a shared cache needs no such guarantee: CMake then places
# bootstrap artifacts under CMAKE_BINARY_DIR, outside the prefetch contract.
#
# Scope: this checks the CPM bootstrap directory only. A dependency that
# downloads outside it (for example into its own source tree) is out of scope.
# Snapshot records use CMake lists and a path|SHA256 delimiter. Reject paths that
# cannot be represented unambiguously rather than trusting a partial filename.
function(_klogg_cpm_check_path path)
  foreach(_unsupported IN ITEMS ";" "|" "[" "]" "\\" "\n" "\r")
    string(FIND "${path}" "${_unsupported}" _index)
    if(NOT _index EQUAL -1)
      message(FATAL_ERROR "Unsupported CPM bootstrap path: ${path}")
    endif()
  endforeach()
endfunction()

function(_klogg_cpm_bootstrap_snapshot output_variable allow_symlinks)
  _klogg_cpm_check_path("${CPM_SOURCE_CACHE}")
  get_filename_component(_cache_directory "${CPM_SOURCE_CACHE}" ABSOLUTE)
  set(_bootstrap_directory "${_cache_directory}/cpm")
  foreach(_directory IN ITEMS "${_cache_directory}" "${_bootstrap_directory}")
    if(IS_SYMLINK "${_directory}")
      message(FATAL_ERROR "Unsupported symlink CPM bootstrap directory: ${_directory}")
    elseif(EXISTS "${_directory}" AND NOT IS_DIRECTORY "${_directory}")
      message(FATAL_ERROR "Unsupported CPM bootstrap directory: ${_directory}")
    endif()
  endforeach()

  # A failed cleanup must not turn an offending file into the next baseline.
  # Never read or overwrite an existing marker, including a dangling symlink.
  set(_cleanup_marker "${_cache_directory}/.klogg-cpm-cleanup-pending")
  if(EXISTS "${_cleanup_marker}" OR IS_SYMLINK "${_cleanup_marker}")
    message(FATAL_ERROR
      "Previous CPM bootstrap cleanup is incomplete: ${_cleanup_marker}. Refresh the contaminated cache with cmake/prefetch_cpm before retrying.")
  endif()

  set(_snapshot "")
  if(IS_DIRECTORY "${_bootstrap_directory}")
    file(GLOB _artifacts "${_bootstrap_directory}/*")
    foreach(_artifact IN LISTS _artifacts)
      _klogg_cpm_check_path("${_artifact}")
      get_filename_component(_parent "${_artifact}" DIRECTORY)
      # Absolute glob results also expose semicolon filenames: a split list
      # fragment is not a direct child, even if its other fragment exists.
      if(NOT _parent STREQUAL _bootstrap_directory)
        message(FATAL_ERROR "Unsupported CPM bootstrap path: ${_artifact}")
      endif()
      if(IS_SYMLINK "${_artifact}")
        if(NOT allow_symlinks)
          message(FATAL_ERROR "Unsupported symlink CPM bootstrap artifact: ${_artifact}")
        endif()
        set(_digest "unsupported-symlink")
      elseif(IS_DIRECTORY "${_artifact}")
        message(FATAL_ERROR "Unsupported directory CPM bootstrap artifact: ${_artifact}")
      else()
        # CMake 3.14 has no regular-file predicate. Bound hashing so a FIFO or
        # another unreadable/unsupported entry cannot hang configure forever.
        execute_process(
          COMMAND "${CMAKE_COMMAND}" -E sha256sum "${_artifact}"
          RESULT_VARIABLE _hash_status OUTPUT_VARIABLE _hash_output
          ERROR_VARIABLE _hash_error TIMEOUT 5
        )
        string(REGEX MATCH "^[0-9a-f]+" _digest "${_hash_output}")
        string(LENGTH "${_digest}" _digest_length)
        if(NOT "${_hash_status}" STREQUAL "0" OR NOT _digest_length EQUAL 64)
          message(FATAL_ERROR
            "Unsupported or unreadable CPM bootstrap artifact: ${_artifact} (${_hash_status}; ${_hash_error})")
        endif()
      endif()
      list(APPEND _snapshot "${_artifact}|${_digest}")
    endforeach()
  endif()
  set(${output_variable} "${_snapshot}" PARENT_SCOPE)
endfunction()

function(klogg_cpm_bootstrap_artifacts output_variable)
  set(_snapshot "")
  if(CPM_SOURCE_CACHE)
    _klogg_cpm_bootstrap_snapshot(_snapshot FALSE)
  endif()
  set(${output_variable} "${_snapshot}" PARENT_SCOPE)
endfunction()

function(klogg_require_hermetic_cpm_cache before_artifacts)
  if(NOT CPM_SOURCE_CACHE)
    return()
  endif()

  _klogg_cpm_bootstrap_snapshot(_after_artifacts TRUE)
  set(_offenders "")
  set(_offender_names "")
  foreach(_record IN LISTS _after_artifacts)
    list(FIND before_artifacts "${_record}" _known_artifact_index)
    if(_known_artifact_index EQUAL -1)
      string(REGEX REPLACE "\\|[^|]*$" "" _artifact "${_record}")
      list(APPEND _offenders "${_artifact}")
      get_filename_component(_name "${_artifact}" NAME)
      if(IS_SYMLINK "${_artifact}")
        string(APPEND _name " (unsupported symlink)")
      endif()
      list(APPEND _offender_names "${_name}")
    endif()
  endforeach()

  if(_offenders)
    get_filename_component(_cache_directory "${CPM_SOURCE_CACHE}" ABSOLUTE)
    set(_cleanup_marker "${_cache_directory}/.klogg-cpm-cleanup-pending")
    file(WRITE "${_cleanup_marker}" "Refresh the contaminated cache with cmake/prefetch_cpm.\n")
    # REMOVE unlinks a symlink, including a directory/dangling symlink, without
    # traversing its target. Never use REMOVE_RECURSE on shared cache contents.
    foreach(_artifact IN LISTS _offenders)
      file(REMOVE "${_artifact}")
    endforeach()
    foreach(_artifact IN LISTS _offenders)
      if(EXISTS "${_artifact}" OR IS_SYMLINK "${_artifact}")
        message(FATAL_ERROR
          "CPM bootstrap cleanup failed: ${_artifact}. Refresh the contaminated cache with cmake/prefetch_cpm before retrying.")
      endif()
    endforeach()
    file(REMOVE "${_cleanup_marker}")
    if(EXISTS "${_cleanup_marker}" OR IS_SYMLINK "${_cleanup_marker}")
      message(FATAL_ERROR
        "CPM bootstrap cleanup marker could not be removed: ${_cleanup_marker}. Refresh the contaminated cache with cmake/prefetch_cpm before retrying.")
    endif()

    string(REPLACE ";" ", " _offenders_text "${_offender_names}")
    message(
      FATAL_ERROR
        "Configure added or changed package-manager bootstrap artifacts in the shared CPM source cache: ${_offenders_text} (under ${CPM_SOURCE_CACHE}/cpm). Offending files were removed so retries cannot trust them.
That cache is seeded only by cmake/prefetch_cpm, so this configure downloaded from a URL that is neither prefetched nor retried nor contract-checked; when that download fails, the whole configure dies with 'file DOWNLOAD cannot compute hash on failed download'.
Disable the dependency's own dependency manager in its cpmaddpackage() OPTIONS (CRoaring: ROARING_USE_CPM OFF), or pre-seed the artifact from cmake/prefetch_cpm."
    )
  endif()
endfunction()
