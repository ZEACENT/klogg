find_package(QT NAMES Qt6 Qt5 COMPONENTS Test REQUIRED)
find_package(Qt${QT_VERSION_MAJOR} COMPONENTS Test REQUIRED)

add_custom_target(klogg_test_build)
include(${CMAKE_SOURCE_DIR}/cmake/TestTargetOptions.cmake)

add_subdirectory(helpers)
add_subdirectory(unit)
add_subdirectory(ui)
if(KLOGG_USE_VECTORSCAN)
  add_subdirectory(vectorscan)
endif()

# KLOGG_TESTS enables test-only API surface (test accessors, counters) in
# production headers guarded by #ifdef KLOGG_TESTS / friend declarations.
# Production (non-test) targets do NOT define this macro, so test seams are
# invisible in release builds.
target_compile_definitions(klogg_tests PUBLIC -DKLOGG_TESTS)
target_compile_definitions(klogg_itests PUBLIC -DKLOGG_TESTS)

add_dependencies(klogg_itests file_write_helper)
# Every configured C++ test joins klogg_test_build through
# klogg_configure_test_target(), keeping CI and coverage build scope aligned.
add_dependencies(ci_build klogg_test_build)

find_package(Python3 COMPONENTS Interpreter REQUIRED)

add_test(
  NAME adb_helper_release_contract
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/scripts/test_adb_helper_release_contract.py
)
add_test(
  NAME adb_helper_verifier_contract
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/scripts/test_adb_helper_verifier_contract.py
)
add_test(
  NAME adb_helper_hardening_contract
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/scripts/test_adb_helper_hardening_contract.py
)
add_test(
  NAME adb_helper_cycle8_release_contract
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/scripts/test_adb_helper_cycle8_release_contract.py
)
add_test(
  NAME libimobiledevice_ostrace_patch_contract
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/scripts/test_libimobiledevice_ostrace_patch_contract.py
)
add_test(
  NAME ios_native_release_contract
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/scripts/test_ios_native_release_contract.py
)
add_test(
  NAME external_source_asset_package_contract
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/scripts/test_external_source_asset_package_contract.py
)
add_test(
  NAME live_capture_benchmark_contract
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/scripts/test_live_capture_benchmark_contract.py
)
add_test(
  NAME live_capture_results_contract
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/scripts/test_live_capture_results_contract.py
)
add_test(
  NAME live_session_ui_retirement_contract
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/scripts/test_live_session_ui_retirement_contract.py
)
add_test(
  NAME livelog_controller_retirement_contract
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/scripts/test_livelog_controller_retirement_contract.py
)
add_test(
  NAME static_link_policy_contract
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/scripts/test_static_link_policy_contract.py
)
add_test(
  NAME application_identity_contract
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/scripts/test_application_identity_contract.py
)
add_test(
  NAME adb_compiler_portability_contract
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/scripts/test_adb_compiler_portability_contract.py
)
add_test(
  NAME ios_compiler_portability_contract
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/scripts/test_ios_compiler_portability_contract.py
)
set_tests_properties(
  adb_helper_release_contract
  adb_helper_verifier_contract
  adb_helper_hardening_contract
  adb_helper_cycle8_release_contract
  libimobiledevice_ostrace_patch_contract
  ios_native_release_contract
  external_source_asset_package_contract
  live_capture_benchmark_contract
  live_capture_results_contract
  live_session_ui_retirement_contract
  livelog_controller_retirement_contract
  static_link_policy_contract
  application_identity_contract
  adb_compiler_portability_contract
  ios_compiler_portability_contract
  PROPERTIES TIMEOUT 60
)






