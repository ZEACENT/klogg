find_package(Python3 COMPONENTS Interpreter REQUIRED)

option(KLOGG_ADB_HELPER_REQUIRED "Fail configure when a verified source-built ADB helper artifact is unavailable" OFF)
set(KLOGG_ADB_HELPER_ARTIFACT_ROOT "" CACHE PATH "Disconnected source-built ADB helper artifact root")
set(KLOGG_ADB_HELPER_TARGET "" CACHE STRING "Locked ADB helper target identifier")
set(FETCHCONTENT_FULLY_DISCONNECTED ON CACHE BOOL "ADB helper application/package configuration is always disconnected" FORCE)

set(KLOGG_ADB_HELPER_LOCK "${CMAKE_CURRENT_SOURCE_DIR}/adb-helper.lock.json")
set(_klogg_adb_verify "${CMAKE_SOURCE_DIR}/scripts/verify_adb_helper_artifact.py")
set(_klogg_adb_smoke "${CMAKE_SOURCE_DIR}/scripts/smoke_adb_helper.py")
set(_klogg_adb_stage "${CMAKE_BINARY_DIR}/adb-helper-staged")
set(_klogg_adb_release_stage "${CMAKE_BINARY_DIR}/adb-helper-release")

if(WIN32)
  set(_klogg_adb_name adb.exe)
  set(_klogg_adb_default_target windows-x86_64)
elseif(APPLE)
  set(_klogg_adb_name adb)
  if(CMAKE_OSX_ARCHITECTURES MATCHES "arm64" OR CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "arm64")
    set(_klogg_adb_default_target macos-arm64)
  else()
    set(_klogg_adb_default_target macos-x86_64)
  endif()
else()
  set(_klogg_adb_name adb)
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
    set(_klogg_adb_default_target linux-arm64)
  else()
    set(_klogg_adb_default_target linux-x86_64)
  endif()
endif()
if(NOT KLOGG_ADB_HELPER_TARGET)
  set(KLOGG_ADB_HELPER_TARGET "${_klogg_adb_default_target}")
endif()

set(_klogg_adb_artifact_helper "${KLOGG_ADB_HELPER_ARTIFACT_ROOT}/helpers/${_klogg_adb_name}")
set(_klogg_adb_artifact_receipt "${KLOGG_ADB_HELPER_ARTIFACT_ROOT}/receipt.json")
set(_klogg_adb_artifact_smoke "${KLOGG_ADB_HELPER_ARTIFACT_ROOT}/smoke.json")
set(_klogg_adb_artifact_package_smoke "${KLOGG_ADB_HELPER_ARTIFACT_ROOT}/package-smoke.json")
set(_klogg_adb_artifact_package_verification "${KLOGG_ADB_HELPER_ARTIFACT_ROOT}/package-verification.json")
set(_klogg_adb_artifact_envelope "${KLOGG_ADB_HELPER_ARTIFACT_ROOT}/SHA256SUMS")
set(_klogg_adb_artifact_release "${KLOGG_ADB_HELPER_ARTIFACT_ROOT}/release")
set(KLOGG_ADB_HELPER_AVAILABLE OFF)
if(KLOGG_ADB_HELPER_ARTIFACT_ROOT)
  foreach(_required_path
      "${_klogg_adb_artifact_helper}"
      "${_klogg_adb_artifact_receipt}"
      "${_klogg_adb_artifact_smoke}"
      "${_klogg_adb_artifact_package_smoke}"
      "${_klogg_adb_artifact_package_verification}"
      "${_klogg_adb_artifact_envelope}"
      "${_klogg_adb_artifact_release}")
    if(NOT EXISTS "${_required_path}")
      message(FATAL_ERROR "source-built ADB helper artifact is missing or not found: ${_required_path}")
    endif()
  endforeach()
  execute_process(
    COMMAND "${Python3_EXECUTABLE}" "${_klogg_adb_verify}"
      --lock "${KLOGG_ADB_HELPER_LOCK}"
      --receipt "${_klogg_adb_artifact_receipt}"
      --package-root "${KLOGG_ADB_HELPER_ARTIFACT_ROOT}"
      --release-root "${_klogg_adb_artifact_release}"
      --helper-path "helpers/${_klogg_adb_name}"
      --expected-target "${KLOGG_ADB_HELPER_TARGET}"
      --checksum-envelope "${_klogg_adb_artifact_envelope}"
      --require-lock-binding
    RESULT_VARIABLE _klogg_adb_verify_result
    OUTPUT_VARIABLE _klogg_adb_verify_output
    ERROR_VARIABLE _klogg_adb_verify_error
  )
  if(NOT _klogg_adb_verify_result EQUAL 0)
    message(FATAL_ERROR "source-built ADB helper verification failed: ${_klogg_adb_verify_output}${_klogg_adb_verify_error}")
  endif()
  set(KLOGG_ADB_HELPER_AVAILABLE ON)
elseif(KLOGG_ADB_HELPER_REQUIRED)
  message(FATAL_ERROR "required source-built ADB helper artifact is missing; package builds must consume the verified prefetch artifact")
endif()

set(_klogg_adb_stage_helper "${_klogg_adb_stage}/helpers/${_klogg_adb_name}")
if(KLOGG_ADB_HELPER_AVAILABLE)
  add_custom_command(
    OUTPUT "${_klogg_adb_stage}/.complete"
    COMMAND "${CMAKE_COMMAND}" -E rm -rf "${_klogg_adb_stage}" "${_klogg_adb_release_stage}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${_klogg_adb_stage}"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${KLOGG_ADB_HELPER_ARTIFACT_ROOT}/helpers" "${_klogg_adb_stage}/helpers"
    COMMAND "${CMAKE_COMMAND}" -E copy "${_klogg_adb_artifact_receipt}" "${_klogg_adb_stage}/receipt.json"
    COMMAND "${CMAKE_COMMAND}" -E copy "${_klogg_adb_artifact_smoke}" "${_klogg_adb_stage}/smoke.json"
    COMMAND "${CMAKE_COMMAND}" -E copy "${_klogg_adb_artifact_package_smoke}" "${_klogg_adb_stage}/package-smoke.json"
    COMMAND "${CMAKE_COMMAND}" -E copy "${_klogg_adb_artifact_package_verification}" "${_klogg_adb_stage}/package-verification.json"
    COMMAND "${CMAKE_COMMAND}" -E copy "${_klogg_adb_artifact_envelope}" "${_klogg_adb_stage}/SHA256SUMS"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${_klogg_adb_artifact_release}" "${_klogg_adb_release_stage}"
    COMMAND "${CMAKE_COMMAND}" -E touch "${_klogg_adb_stage}/.complete"
    DEPENDS
      "${_klogg_adb_artifact_helper}"
      "${_klogg_adb_artifact_receipt}"
      "${_klogg_adb_artifact_smoke}"
      "${_klogg_adb_artifact_package_smoke}"
      "${_klogg_adb_artifact_package_verification}"
      "${_klogg_adb_artifact_envelope}"
      "${KLOGG_ADB_HELPER_LOCK}"
    VERBATIM
  )
  add_custom_target(klogg_adb_helper DEPENDS "${_klogg_adb_stage}/.complete")
  add_custom_target(klogg_adb_helper_verify
    COMMAND "${Python3_EXECUTABLE}" "${_klogg_adb_verify}"
      --lock "${KLOGG_ADB_HELPER_LOCK}"
      --receipt "${_klogg_adb_stage}/receipt.json"
      --package-root "${_klogg_adb_stage}"
      --release-root "${_klogg_adb_release_stage}"
      --helper-path "helpers/${_klogg_adb_name}"
      --expected-target "${KLOGG_ADB_HELPER_TARGET}"
      --checksum-envelope "${_klogg_adb_stage}/SHA256SUMS"
      --require-lock-binding
    DEPENDS klogg_adb_helper
    VERBATIM
  )
  add_custom_target(klogg_adb_helper_smoke
    COMMAND "${Python3_EXECUTABLE}" "${_klogg_adb_smoke}"
      --adb "${_klogg_adb_stage_helper}"
      --port 0
      --timeout-seconds 15
      --json-output "${_klogg_adb_stage}/smoke.json"
    DEPENDS klogg_adb_helper_verify
    VERBATIM
  )

  if(NOT APPLE)
    install(PROGRAMS "${_klogg_adb_stage_helper}"
      DESTINATION "${CMAKE_INSTALL_BINDIR}/helpers")
    install(DIRECTORY "${_klogg_adb_stage}/helpers/"
      DESTINATION "${CMAKE_INSTALL_BINDIR}/helpers"
      FILES_MATCHING PATTERN "*.so*" PATTERN "*.dll")
    install(DIRECTORY "${_klogg_adb_release_stage}/"
      DESTINATION "${CMAKE_INSTALL_DOCDIR}/adb-helper")
  endif()
else()
  add_custom_target(klogg_adb_helper)
  add_custom_target(klogg_adb_helper_verify DEPENDS klogg_adb_helper)
  add_custom_target(klogg_adb_helper_smoke DEPENDS klogg_adb_helper_verify)
endif()

add_custom_target(klogg_adb_helper_source_archive DEPENDS klogg_adb_helper)
add_custom_target(klogg_adb_helper_licenses DEPENDS klogg_adb_helper)
add_custom_target(klogg_adb_helper_notices DEPENDS klogg_adb_helper)
add_custom_target(klogg_adb_helper_sbom DEPENDS klogg_adb_helper)
add_custom_target(klogg_adb_helper_source_offer DEPENDS klogg_adb_helper)

set(KLOGG_ADB_HELPER_AVAILABLE "${KLOGG_ADB_HELPER_AVAILABLE}" PARENT_SCOPE)
set(KLOGG_ADB_HELPER_STAGE "${_klogg_adb_stage}" PARENT_SCOPE)
set(KLOGG_ADB_HELPER_RELEASE_STAGE "${_klogg_adb_release_stage}" PARENT_SCOPE)
