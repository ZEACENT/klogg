# Verify that a configure leaves the shared CPM source cache untouched.
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
function(klogg_cpm_bootstrap_artifacts output_variable)
  set(_bootstrap_directory "${CPM_SOURCE_CACHE}/cpm")
  set(_artifacts "")
  if(CPM_SOURCE_CACHE AND IS_DIRECTORY "${_bootstrap_directory}")
    file(GLOB _artifacts RELATIVE "${_bootstrap_directory}" "${_bootstrap_directory}/*")
  endif()
  set(${output_variable} "${_artifacts}" PARENT_SCOPE)
endfunction()

function(klogg_require_hermetic_cpm_cache before_artifacts)
  if(NOT CPM_SOURCE_CACHE)
    return()
  endif()

  klogg_cpm_bootstrap_artifacts(_after_artifacts)

  set(_new_artifacts "")
  foreach(_artifact IN LISTS _after_artifacts)
    list(FIND before_artifacts "${_artifact}" _known_artifact_index)
    if(_known_artifact_index EQUAL -1)
      list(APPEND _new_artifacts "${_artifact}")
    endif()
  endforeach()

  if(_new_artifacts)
    string(REPLACE ";" ", " _new_artifacts_text "${_new_artifacts}")
    message(
      FATAL_ERROR
        "Configure added package-manager bootstrap artifacts to the shared CPM source cache: ${_new_artifacts_text} (under ${CPM_SOURCE_CACHE}/cpm).
That cache is seeded only by cmake/prefetch_cpm, so this configure downloaded from a URL that is neither prefetched nor retried nor contract-checked; when that download fails, the whole configure dies with 'file DOWNLOAD cannot compute hash on failed download'.
Disable the dependency's own dependency manager in its cpmaddpackage() OPTIONS (CRoaring: ROARING_USE_CPM OFF), or pre-seed the artifact from cmake/prefetch_cpm."
    )
  endif()
endfunction()
