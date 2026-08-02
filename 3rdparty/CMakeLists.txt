set(QAPPLICATION_CLASS QApplication)

include(CPM)
include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/MsvcAsanDependencies.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/verify_pinned_source.cmake)

set(_TMP_CPM_USE_LOCAL_PACKAGES ${CPM_USE_LOCAL_PACKAGES})

function(klogg_apply_pinned_patch_series dependency source_dir expected_revision clean_hash patched_hash)
  set(_patch_files ${ARGN})
  execute_process(
    COMMAND
      ${CMAKE_COMMAND}
      -DDEPENDENCY=${dependency}
      -DREPO_DIR=${source_dir}
      -DEXPECTED_REVISION=${expected_revision}
      -DCLEAN_TREE_HASH=${clean_hash}
      "-DAPPROVED_TREE_HASHES=${clean_hash};${patched_hash}"
      -DPATCHED_TREE_HASH=${patched_hash}
      "-DPATCH_FILES=${_patch_files}"
      -P ${CMAKE_CURRENT_SOURCE_DIR}/../cmake/apply_git_patch_series.cmake
    RESULT_VARIABLE _patch_series_result
  )
  if(NOT _patch_series_result EQUAL 0)
    message(FATAL_ERROR "Failed to verify and patch ${dependency} source")
  endif()
endfunction()

set(_klogg_simdutf_options
    "SIMDUTF_TESTS OFF"
    "SIMDUTF_TOOLS OFF"
    "SIMDUTF_BENCHMARKS OFF"
)
if(ENABLE_SANITIZER_ADDRESS AND NOT MSVC)
  list(APPEND _klogg_simdutf_options "SIMDUTF_SANITIZE ON")
endif()
if(ENABLE_SANITIZER_UNDEFINED_BEHAVIOR AND NOT MSVC)
  list(APPEND _klogg_simdutf_options "SIMDUTF_SANITIZE_UNDEFINED ON")
endif()
cpmaddpackage(
  NAME simdutf
  GITHUB_REPOSITORY simdutf/simdutf
  VERSION 5.6.2
  OPTIONS ${_klogg_simdutf_options}
)
# MSVC ASan trips over simdutf's lazy multi-ISA implementation registry even
# when container annotation diagnostics are disabled. Keep simdutf and all
# first-party callers instrumented, but use its scalar implementation for this
# one sanitizer/toolchain combination so no poisoned registry is constructed.
if(TARGET simdutf AND MSVC AND ENABLE_SANITIZER_ADDRESS)
  target_compile_definitions(
    simdutf
    PUBLIC SIMDUTF_IMPLEMENTATION_ICELAKE=0
           SIMDUTF_IMPLEMENTATION_HASWELL=0
           SIMDUTF_IMPLEMENTATION_WESTMERE=0
           SIMDUTF_IMPLEMENTATION_ARM64=0
           SIMDUTF_IMPLEMENTATION_PPC64=0
           SIMDUTF_IMPLEMENTATION_RVV=0
           SIMDUTF_IMPLEMENTATION_FALLBACK=1
  )
endif()
cpmaddpackage("gh:foonathan/type_safe@0.2.4")
cpmaddpackage("gh:RoaringBitmap/CRoaring@4.2.1")

# Patched dependencies must always use their pinned source trees, never a
# find_package() result selected by CPM_USE_LOCAL_PACKAGES.
set(CPM_USE_LOCAL_PACKAGES OFF)
cpmaddpackage(
  NAME streamvbyte
  GITHUB_REPOSITORY lemire/streamvbyte
  GIT_TAG c43294a81501e0fdf14adc83818d47f7f9bc1bb6
  GIT_CONFIG core.autocrlf=false
  DOWNLOAD_ONLY YES
)
klogg_apply_pinned_patch_series(
  streamvbyte
  ${streamvbyte_SOURCE_DIR}
  c43294a81501e0fdf14adc83818d47f7f9bc1bb6
  3f4a410e6b1ad8f1f1d53c8463dc7accc7e1c94a32ca1ed1da778a181f5892b9
  d736c1c9bc9e3aca800bea90765f805e27229785e3d8e417e64103ba5fc96164
  ${CMAKE_CURRENT_SOURCE_DIR}/patches/fix_streamvbyte_unaligned_stores.patch
)
if(TARGET streamvbyte)
  get_target_property(_streamvbyte_target_source_dir streamvbyte SOURCE_DIR)
  get_filename_component(_streamvbyte_expected_source_dir "${streamvbyte_SOURCE_DIR}" REALPATH)
  get_filename_component(_streamvbyte_actual_source_dir "${_streamvbyte_target_source_dir}" REALPATH)
  if(NOT "${_streamvbyte_actual_source_dir}" STREQUAL "${_streamvbyte_expected_source_dir}")
    message(FATAL_ERROR "Existing streamvbyte target does not use the pinned patched source")
  endif()
else()
  add_subdirectory(${streamvbyte_SOURCE_DIR} ${streamvbyte_BINARY_DIR} EXCLUDE_FROM_ALL)
endif()
set(CPM_USE_LOCAL_PACKAGES ${_TMP_CPM_USE_LOCAL_PACKAGES})

if(APPLE)
  cpmaddpackage(
    NAME
    macdeployqtfix
    GITHUB_REPOSITORY
    arl/macdeployqtfix
    GIT_TAG
    df888505849d3c06d20a4338af276dfa7d11826a
    DOWNLOAD_ONLY
    YES
  )
  if(macdeployqtfix_ADDED)
    configure_file(${macdeployqtfix_SOURCE_DIR}/macdeployqtfix.py ${CMAKE_BINARY_DIR}/macdeployqtfix.py COPYONLY)
  endif()
endif()

cpmaddpackage(
  NAME
  maddy
  GITHUB_REPOSITORY
  variar/maddy
  GIT_TAG
  602e26613e624535e2de883b3f2c98a16729d1d4
  EXCLUDE_FROM_ALL
  YES
)

if(KLOGG_USE_VECTORSCAN)
  if(NOT BOOST_ROOT AND DEFINED ENV{BOOST_ROOT})
    set(BOOST_ROOT "$ENV{BOOST_ROOT}" CACHE PATH "Boost root path" FORCE)
  endif()
  if(APPLE AND (NOT BOOST_ROOT OR BOOST_ROOT STREQUAL ""))
    find_program(KLOGG_BREW_EXECUTABLE brew)
    if(KLOGG_BREW_EXECUTABLE)
      execute_process(
        COMMAND ${KLOGG_BREW_EXECUTABLE} --prefix boost
        OUTPUT_VARIABLE KLOGG_BREW_BOOST_PREFIX
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
      )
      if(KLOGG_BREW_BOOST_PREFIX AND EXISTS "${KLOGG_BREW_BOOST_PREFIX}")
        set(BOOST_ROOT "${KLOGG_BREW_BOOST_PREFIX}" CACHE PATH "Boost root path" FORCE)
      endif()
    endif()
  endif()

  if(KLOGG_GENERIC_CPU)
    set(VECTORSCAN_BUILD_AVX2 OFF)
    set(VECTORSCAN_BUILD_AVX512 OFF)
  elseif(MSVC)
    set(VECTORSCAN_BUILD_AVX2 ON)
    set(VECTORSCAN_BUILD_AVX512 OFF)
  else()
    set(VECTORSCAN_BUILD_AVX2 ON)
    set(VECTORSCAN_BUILD_AVX512 ON)
  endif()

  message(
    STATUS
      "Vectorscan SIMD config: BUILD_AVX2=${VECTORSCAN_BUILD_AVX2}, BUILD_AVX512=${VECTORSCAN_BUILD_AVX512}, BUILD_AVX512VBMI=${VECTORSCAN_BUILD_AVX512}"
  )
  message(
    STATUS
      "Vectorscan toolchain: C_COMPILER_ID=${CMAKE_C_COMPILER_ID}, C_COMPILER=${CMAKE_C_COMPILER}, MSVC=${MSVC}"
  )

  # Patched Vectorscan must use the pinned source tree, never a package
  # substituted through CPM_USE_LOCAL_PACKAGES.
  set(CPM_USE_LOCAL_PACKAGES OFF)

  # Download vectorscan source without processing its cmake files.
  # Patches must be applied BEFORE add_subdirectory so that cmake-level
  # fixes (e.g. MSVC warning flags) take effect during configuration.
  # Using DOWNLOAD_ONLY keeps UNPARSED_ARGUMENTS empty, matching the
  # prefetch project's CPM source-cache hash.
  cpmaddpackage(
    NAME
    vectorscan
    GITHUB_REPOSITORY
    VectorCamp/vectorscan
    GIT_TAG
    d29730e1cb9daaa66bda63426cdce83505d2c809
    GIT_CONFIG
    core.autocrlf=false
    DOWNLOAD_ONLY
    YES
  )
  klogg_apply_pinned_patch_series(
    vectorscan
    ${vectorscan_SOURCE_DIR}
    d29730e1cb9daaa66bda63426cdce83505d2c809
    04357c577d094b3ea8760c3635c776789fb671c1bff010c66a43f479f442eefd
    bd842a94bd5ba6ba87cc475bcd0881f5d53ab6e9b45cd964bc3d4c2ea7f1298a
    ${CMAKE_CURRENT_SOURCE_DIR}/patches/fix_vectorscan_msvc_warning_flags.patch
    ${CMAKE_CURRENT_SOURCE_DIR}/patches/fix_vectorscan_msvc_aligned_free.patch
    ${CMAKE_CURRENT_SOURCE_DIR}/patches/fix_vectorscan_null_history_ubsan.patch
  )

  # Set vectorscan build options before processing its cmake files
  set(BUILD_STATIC_LIBS ON CACHE INTERNAL "" FORCE)
  set(BUILD_UNIT OFF CACHE INTERNAL "" FORCE)
  set(BUILD_TOOLS OFF CACHE INTERNAL "" FORCE)
  set(BUILD_EXAMPLES OFF CACHE INTERNAL "" FORCE)
  set(BUILD_BENCHMARKS OFF CACHE INTERNAL "" FORCE)
  set(BUILD_DOC OFF CACHE INTERNAL "" FORCE)
  set(BUILD_CHIMERA OFF CACHE INTERNAL "" FORCE)
  set(BUILD_AVX2 ${VECTORSCAN_BUILD_AVX2} CACHE INTERNAL "" FORCE)
  set(BUILD_AVX512 ${VECTORSCAN_BUILD_AVX512} CACHE INTERNAL "" FORCE)
  set(BUILD_AVX512VBMI ${VECTORSCAN_BUILD_AVX512} CACHE INTERNAL "" FORCE)
  set(FAT_RUNTIME OFF CACHE INTERNAL "" FORCE)

  if(TARGET hs)
    message(FATAL_ERROR "hs was configured before Klogg could apply the pinned Vectorscan patches")
  endif()
  add_subdirectory(${vectorscan_SOURCE_DIR} ${vectorscan_BINARY_DIR} EXCLUDE_FROM_ALL)
  klogg_stabilize_msvc_asan_dependencies(hs_exec hs_compile)
  set(CPM_USE_LOCAL_PACKAGES ${_TMP_CPM_USE_LOCAL_PACKAGES})

  message("Adding alias for vectorscan")
  add_library(klogg_vectorscan INTERFACE)
  target_link_libraries(klogg_vectorscan INTERFACE hs)
  target_include_directories(klogg_vectorscan INTERFACE 
    ${vectorscan_SOURCE_DIR}/src
    ${vectorscan_BINARY_DIR}
  )
endif()

cpmaddpackage(
  NAME
  Uchardet
  GIT_REPOSITORY
  https://gitlab.freedesktop.org/uchardet/uchardet
  VERSION
  0.0.8
  EXCLUDE_FROM_ALL
  YES
  OPTIONS
  "BUILD_BINARY OFF"
)
if(Uchardet_ADDED)
  message("Adding alias for uchardet")
  add_library(klogg_uchardet_wrapper INTERFACE)
  target_link_libraries(klogg_uchardet_wrapper INTERFACE libuchardet)
  target_include_directories(klogg_uchardet_wrapper INTERFACE ${Uchardet_SOURCE_DIR}/src)
else()
  add_library(klogg_uchardet_wrapper INTERFACE)
  target_link_libraries(klogg_uchardet_wrapper INTERFACE ${UCHARDET_LIBRARY})
  target_include_directories(klogg_uchardet_wrapper INTERFACE ${UCHARDET_INCLUDE_DIR})
endif()

if(${QT_VERSION_MAJOR} EQUAL 6) 
set(CPM_USE_LOCAL_PACKAGES OFF)
endif()

cpmaddpackage(
  NAME
  KF5Archive
  GITHUB_REPOSITORY
  variar/klogg_karchive
  GIT_TAG
  f546bf6ae66a8d34b43da5a41afcfbf4e1a47906
  EXCLUDE_FROM_ALL
  YES
)
if(KF5Archive_ADDED)
  execute_process(
    COMMAND
      ${CMAKE_COMMAND}
      -DREPO_DIR=${KF5Archive_SOURCE_DIR}
      -DPATCH_FILE=${CMAKE_CURRENT_SOURCE_DIR}/patches/fix_karchive_qt6.patch
      -P ${CMAKE_CURRENT_SOURCE_DIR}/../cmake/apply_git_patch.cmake
    RESULT_VARIABLE _karchive_patch_result
  )
  if(NOT _karchive_patch_result EQUAL 0)
    message(FATAL_ERROR "Failed to apply KArchive Qt6 patch: ${CMAKE_CURRENT_SOURCE_DIR}/patches/fix_karchive_qt6.patch")
  endif()
else()
  # KF5Archive_ADDED is false — use system package
  find_package(KF5Archive)
  add_library(klogg_karchive INTERFACE)
  target_link_libraries(klogg_karchive INTERFACE KF5::Archive)
endif()

if(${QT_VERSION_MAJOR} EQUAL 6)
message("Resetting CPM_USE_LOCAL_PACKAGES to ${_TMP_CPM_USE_LOCAL_PACKAGES}")
set(CPM_USE_LOCAL_PACKAGES ${_TMP_CPM_USE_LOCAL_PACKAGES})
endif()

cpmaddpackage(
  NAME
  robin_hood
  GITHUB_REPOSITORY
  martinus/robin-hood-hashing
  GIT_TAG
  3.11.2
  EXCLUDE_FROM_ALL
  YES
)
if(NOT TARGET robin_hood)
  message("Adding imported target for robin_hood")
  add_library(robin_hood INTERFACE)
  target_include_directories(robin_hood INTERFACE ${ROBIN_HOOD_INCLUDE_DIRS})
endif(NOT TARGET robin_hood)

if(KLOGG_BUILD_TESTS)
  cpmaddpackage("gh:bombela/backward-cpp@1.6")
  cpmaddpackage(
    NAME
    Catch2
    GITHUB_REPOSITORY
    catchorg/Catch2
    VERSION
    2.13.8
    EXCLUDE_FROM_ALL
    YES
  )
  if(NOT TARGET Catch2)
    message("Adding imported target for catch2")
    add_library(Catch2 INTERFACE)
    target_include_directories(Catch2 INTERFACE ${CATCH2_INCLUDE_DIRS})
  endif(NOT TARGET Catch2)
endif()

cpmaddpackage(
  NAME
  KDSingleApplication
  GITHUB_REPOSITORY
  variar/KDSingleApplication
  GIT_TAG
  5b30db30266f92bc01f1439777803ce8dbf16c79
  DOWNLOAD_ONLY
  YES
)
if(KDSingleApplication_ADDED)
  add_library(
    kdsingleapp STATIC ${KDSingleApplication_SOURCE_DIR}/src/kdsingleapplication.cpp
                     ${KDSingleApplication_SOURCE_DIR}/src/kdsingleapplication_localsocket.cpp
  )

  target_include_directories(kdsingleapp PUBLIC ${KDSingleApplication_SOURCE_DIR}/src)
  target_link_libraries(kdsingleapp Qt${QT_VERSION_MAJOR}::Core)
  target_compile_definitions(kdsingleapp PUBLIC KDSINGLEAPPLICATION_STATIC_BUILD)

  set_target_properties(kdsingleapp PROPERTIES AUTOMOC ON)
endif()


cpmaddpackage(
  NAME
  xxHash
  GITHUB_REPOSITORY
  Cyan4973/xxHash
  VERSION
  0.8.1
  DOWNLOAD_ONLY
  YES
)
if(xxHash_ADDED)
  set(XXHASH_BUILD_ENABLE_INLINE_API OFF)
  set(XXHASH_BUILD_XXHSUM OFF)
  add_subdirectory(${xxHash_SOURCE_DIR}/cmake_unofficial ${CMAKE_BINARY_DIR}/xxhash EXCLUDE_FROM_ALL)
endif()

cpmaddpackage(
  NAME
  whereami
  GITHUB_REPOSITORY
  gpakosz/whereami
  GIT_TAG
  dcb52a058dc14530ba9ae05e4339bd3ddfae0e0e
  DOWNLOAD_ONLY
  YES
)
if(whereami_ADDED)
  add_library(whereami STATIC ${whereami_SOURCE_DIR}/src/whereami.h ${whereami_SOURCE_DIR}/src/whereami.c)
  target_include_directories(whereami PUBLIC ${whereami_SOURCE_DIR}/src)
endif()

cpmaddpackage(
  NAME
  exprtk
  GITHUB_REPOSITORY
  variar/klogg_exprtk
  GIT_TAG
  1f9f4cd7d2620b7b24232de9ea22908d63913459
  DOWNLOAD_ONLY
  YES
)
if(exprtk_ADDED)
  add_library(exprtk INTERFACE)
  target_link_libraries(exprtk INTERFACE robin_hood)
  target_include_directories(exprtk SYSTEM INTERFACE ${exprtk_SOURCE_DIR})
  target_compile_definitions(
    exprtk
    INTERFACE -Dexprtk_disable_caseinsensitivity
              -Dexprtk_disable_comments
              -Dexprtk_disable_break_continue
              -Dexprtk_disable_return_statement
              -Dexprtk_disable_superscalar_unroll
              -Dexprtk_disable_rtl_io_file
              -Dexprtk_disable_rtl_vecops
              -Dexprtk_disable_string_capabilities
  )
endif()

cpmaddpackage(
  NAME
  KDToolBox
  GITHUB_REPOSITORY
  KDAB/KDToolBox
  GIT_TAG
  6468867d1a46eabe1bcb2cd342f338fe66f06675
  DOWNLOAD_ONLY
  YES
)
if(KDToolBox_ADDED)
  add_library(
    kdtoolbox STATIC ${KDToolBox_SOURCE_DIR}/qt/KDSignalThrottler/src/KDSignalThrottler.cpp
                     ${KDToolBox_SOURCE_DIR}/qt/KDSignalThrottler/src/KDSignalThrottler.h
  )

  target_include_directories(kdtoolbox PUBLIC ${KDToolBox_SOURCE_DIR}/qt/KDSignalThrottler/src)
  target_link_libraries(kdtoolbox Qt${QT_VERSION_MAJOR}::Core)
  set_target_properties(kdtoolbox PROPERTIES AUTOMOC ON)
endif()

# Download before configuring so the backend race fixes can be applied
# deterministically to the pinned 1.4.1 source. Upgrading wholesale would
# raise efsw's CMake minimum beyond klogg's Ubuntu 20.04 baseline.
set(CPM_USE_LOCAL_PACKAGES OFF)
cpmaddpackage(
  NAME
  efsw
  GITHUB_REPOSITORY
  SpartanJ/efsw
  GIT_TAG
  62f785c56b7a34f035193d4cb831921347b586b8
  GIT_CONFIG
  core.autocrlf=false
  DOWNLOAD_ONLY
  YES
)
klogg_apply_pinned_patch_series(
  efsw
  ${efsw_SOURCE_DIR}
  62f785c56b7a34f035193d4cb831921347b586b8
  c9dfe877307cb578f7db72f536a8ce26f4049573ceb434d3a8736862746be677
  08283124ae155e995254ef3282fb41882c7599f30b79346764b71ec0815789ee
  ${CMAKE_CURRENT_SOURCE_DIR}/patches/fix_efsw_inotify_watcher_uaf.patch
  ${CMAKE_CURRENT_SOURCE_DIR}/patches/fix_efsw_kqueue_watcher_races.patch
)

# The target used by klogg must always come from the pinned, patched source.
# Do not let an earlier superproject package registration silently substitute
# an unpatched efsw target.

if(TARGET efsw)
  get_target_property(_efsw_target_source_dir efsw SOURCE_DIR)
  get_filename_component(_efsw_expected_source_dir "${efsw_SOURCE_DIR}" REALPATH)
  get_filename_component(_efsw_actual_source_dir "${_efsw_target_source_dir}" REALPATH)
  if(NOT "${_efsw_actual_source_dir}" STREQUAL "${_efsw_expected_source_dir}")
    message(
      FATAL_ERROR
        "Existing efsw target comes from '${_efsw_actual_source_dir}', expected pinned source '${_efsw_expected_source_dir}'"
    )
  endif()
else()
  set(BUILD_TEST_APP OFF CACHE BOOL "" FORCE)
  add_subdirectory(${efsw_SOURCE_DIR} ${efsw_BINARY_DIR} EXCLUDE_FROM_ALL)
endif()
target_compile_definitions(efsw PRIVATE EFSW_FSEVENTS_NOT_SUPPORTED)
set(CPM_USE_LOCAL_PACKAGES ${_TMP_CPM_USE_LOCAL_PACKAGES})

if(WIN32)
  set(BUILD_SHARED_LIBS ON)
endif()
set(_klogg_tbb_options
    "TBB_TEST OFF"
    "TBB_EXAMPLES OFF"
    "TBB_STRICT OFF"
)
if(ENABLE_SANITIZER_THREAD)
  list(APPEND _klogg_tbb_options "TBB_SANITIZE thread")
elseif(ENABLE_SANITIZER_ADDRESS AND NOT MSVC)
  list(APPEND _klogg_tbb_options "TBB_SANITIZE address")
endif()
cpmaddpackage(
  NAME
  tbb
  GITHUB_REPOSITORY
  variar/oneTBB
  GIT_TAG
  c9be1ac2930f02dea523003ed801b4489f3e6b6e
  EXCLUDE_FROM_ALL
  YES
  OPTIONS ${_klogg_tbb_options}
)

if(WIN32)
  set(BUILD_SHARED_LIBS OFF)
endif()

set(CPM_USE_LOCAL_PACKAGES OFF)
set(MI_BUILD_TESTS OFF)
set(MI_SECURE OFF)
set(MI_BUILD_SHARED OFF)
set(MI_BUILD_STATIC ON)
set(MI_BUILD_OBJECT OFF)
set(MI_OVERRIDE OFF)
set(MI_TRACK_ASAN OFF)
set(MI_DEBUG_TSAN OFF)
if(ENABLE_SANITIZER_ADDRESS AND NOT MSVC)
  set(MI_TRACK_ASAN ON)
endif()
if(ENABLE_SANITIZER_THREAD)
  set(MI_DEBUG_TSAN ON)
endif()
# Peeled commit for the annotated v2.1.7 tag. Download without evaluating the
# dependency's CMakeLists.txt, verify its complete source tree, then add it.
cpmaddpackage(
  NAME
  mimalloc
  GITHUB_REPOSITORY
  microsoft/mimalloc
  GIT_TAG
  8c532c32c3c96e5ba1f2283e032f69ead8add00f
  DOWNLOAD_ONLY
  YES
)
if(TARGET mimalloc-static)
  message(FATAL_ERROR "mimalloc-static exists before pinned source verification")
endif()
klogg_add_verified_subdirectory(
  mimalloc
  ${mimalloc_SOURCE_DIR}
  ${mimalloc_BINARY_DIR}
  8c532c32c3c96e5ba1f2283e032f69ead8add00f
  f08b3cb3f1dfb1e37558e9f9461913bc69b2062998a42f5365c95fd1e3807db1
)
if(NOT TARGET mimalloc-static)
  message(FATAL_ERROR "Verified mimalloc source did not define mimalloc-static")
endif()
get_target_property(_mimalloc_target_source_dir mimalloc-static SOURCE_DIR)
get_filename_component(_mimalloc_expected_source_dir "${mimalloc_SOURCE_DIR}" REALPATH)
get_filename_component(_mimalloc_actual_source_dir "${_mimalloc_target_source_dir}" REALPATH)
if(NOT "${_mimalloc_actual_source_dir}" STREQUAL "${_mimalloc_expected_source_dir}")
  message(FATAL_ERROR "mimalloc-static does not use the verified pinned source")
endif()
message("Adding alias for mimalloc")
add_library(klogg_mimalloc_wrapper INTERFACE)
target_link_libraries(klogg_mimalloc_wrapper INTERFACE mimalloc-static)
target_include_directories(klogg_mimalloc_wrapper INTERFACE ${mimalloc_SOURCE_DIR}/include)

set(CPM_USE_LOCAL_PACKAGES ${_TMP_CPM_USE_LOCAL_PACKAGES})

if(KLOGG_USE_SENTRY)
  set(SENTRY_BACKEND
     "crashpad"
     CACHE INTERNAL "" FORCE
  )
  set(SENTRY_TRANSPORT
     "none"
     CACHE INTERNAL "" FORCE
  )
  set(SENTRY_BUILD_EXAMPLES
     OFF
     CACHE INTERNAL "" FORCE
  )
  set(CRASHPAD_ENABLE_INSTALL
     OFF
     CACHE INTERNAL "" FORCE
  )
  set(CRASHPAD_ENABLE_INSTALL_DEV
     OFF
     CACHE INTERNAL "" FORCE
  )

  cpmaddpackage(
    NAME
    sentry
    GITHUB_REPOSITORY
    getsentry/sentry-native
    GIT_TAG
    a3d58622a807b9dda174cb9fc18fa0f98c89d043
    EXCLUDE_FROM_ALL
    YES
  )

  if(WIN32)
    set_target_properties(crashpad_handler PROPERTIES OUTPUT_NAME "klogg_crashpad_handler.exe")
  else()
    set_target_properties(crashpad_handler PROPERTIES OUTPUT_NAME "klogg_crashpad_handler")
  endif()
endif(KLOGG_USE_SENTRY)

set(klogg_cpm_targets
    xxhash
    Catch2
    roaring
    roaring-headers-cpp
    libuchardet
    robin_hood
    whereami
    simdutf
    efsw
    SingleApplication
    hs
    tbb
    exprtk
    type_safe
    mimalloc-static
    sentry
    crashpad_client
    crashpad_compat
    crashpad_util
    mini_chromium
    streamvbyte
    FastPFor
)
foreach(target ${klogg_cpm_targets})
  if(TARGET ${target})
    get_property(
      ${target}_include_dirs
      TARGET ${target}
      PROPERTY INTERFACE_INCLUDE_DIRECTORIES
    )
    #message("Marking ${${target}_include_dirs} as system incldue")
    set_property(TARGET ${target} PROPERTY INTERFACE_SYSTEM_INCLUDE_DIRECTORIES ${${target}_include_dirs})
  endif()
endforeach()
