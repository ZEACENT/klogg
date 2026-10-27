add_executable(adb_server_test_helper adb_server_test_helper.cpp)
target_link_libraries(adb_server_test_helper PRIVATE project_options project_warnings)

# The native iOS protocol core is deliberately tested without Qt, a device, or
# libimobiledevice. The production adapter owns that integration boundary.
add_executable(
  klogg_ios_protocol_tests
  ios_native_error_mapping_test.cpp
  ios_ostrace_protocol_test.cpp
  pure_catch_main.cpp
)
target_link_libraries(
  klogg_ios_protocol_tests PRIVATE klogg_ios_protocol Catch2 project_options project_warnings
)
klogg_configure_test_target(klogg_ios_protocol_tests)
add_test(NAME klogg_ios_protocol_tests COMMAND klogg_ios_protocol_tests)
set_tests_properties(klogg_ios_protocol_tests PROPERTIES TIMEOUT 120)
klogg_set_test_isolation(klogg_ios_protocol_tests)

# The private crash collector shares protocol and reap ordering with these
# deterministic fakes; this target runs without Windows or intentional faults.
add_executable(
  klogg_crash_collector_protocol_tests
  crash_collector_protocol_test.cpp
  pure_catch_main.cpp
)
target_link_libraries(
  klogg_crash_collector_protocol_tests PRIVATE test_utils Catch2 project_options project_warnings
)
klogg_configure_test_target(klogg_crash_collector_protocol_tests)
add_test(NAME klogg_crash_collector_protocol_tests COMMAND klogg_crash_collector_protocol_tests)
set_tests_properties(klogg_crash_collector_protocol_tests PROPERTIES TIMEOUT 120)
klogg_set_test_isolation(klogg_crash_collector_protocol_tests)

# The Win32 backend shares this operation-lifetime coordinator with the
# deterministic native-I/O fake, so cancellation needs no kernel or timing.
get_target_property(_klogg_efsw_source_dir efsw SOURCE_DIR)
# efsw declares C++11. The runnable test uses project_options (C++17), so
# compile the same coverage separately at the dependency's language boundary.
add_library(
  klogg_directory_io_lifetime_cxx11 OBJECT win32_directory_io_lifetime_test.cpp
)
set_target_properties(
  klogg_directory_io_lifetime_cxx11
  PROPERTIES CXX_STANDARD 11 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO
)
target_include_directories(
  klogg_directory_io_lifetime_cxx11 PRIVATE "${_klogg_efsw_source_dir}/src"
)
target_link_libraries(
  klogg_directory_io_lifetime_cxx11 PRIVATE Catch2 project_warnings
)
add_executable(
  klogg_directory_io_lifetime_tests
  win32_directory_io_lifetime_test.cpp
  pure_catch_main.cpp
)
add_dependencies(klogg_directory_io_lifetime_tests klogg_directory_io_lifetime_cxx11)
target_include_directories(
  klogg_directory_io_lifetime_tests PRIVATE "${_klogg_efsw_source_dir}/src"
)
target_link_libraries(
  klogg_directory_io_lifetime_tests PRIVATE Catch2 project_options project_warnings
)
klogg_configure_test_target(klogg_directory_io_lifetime_tests)
add_test(NAME klogg_directory_io_lifetime_tests COMMAND klogg_directory_io_lifetime_tests)
set_tests_properties(klogg_directory_io_lifetime_tests PROPERTIES TIMEOUT 120)
klogg_set_test_isolation(klogg_directory_io_lifetime_tests)

# Configuration loads before KloggApp in production, so migrations must not
# require a QGuiApplication or a platform plugin.
add_executable(
  klogg_configuration_preapplication_tests
  configuration_preapplication_test.cpp
  pure_catch_main.cpp
)
target_link_libraries(
  klogg_configuration_preapplication_tests
  PRIVATE klogg_settings Catch2 Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui
          project_options project_warnings
)
klogg_configure_test_target(klogg_configuration_preapplication_tests)
add_test(
  NAME klogg_configuration_preapplication_tests
  COMMAND klogg_configuration_preapplication_tests
)
set_tests_properties(klogg_configuration_preapplication_tests PROPERTIES TIMEOUT 120)
klogg_set_test_isolation(klogg_configuration_preapplication_tests)

find_package(Threads REQUIRED)

# Native iOS discovery and stream-worker lifecycle are specified against an
# injected C-ABI table. Tests include no Qt or libimobiledevice APIs directly:
# catalog snapshots, passive pairing, callbacks, ownership, event races,
# queueing, and shutdown remain source-neutral and device-free.
add_executable(
  klogg_ios_native_contract_tests
  ios_device_catalog_test.cpp
  ios_native_api_contract_test.cpp
  ios_native_stream_worker_test.cpp
  pure_catch_main.cpp
)
target_link_libraries(
  klogg_ios_native_contract_tests
  PRIVATE klogg_livecapture Catch2 Threads::Threads project_options project_warnings
)
klogg_configure_test_target(klogg_ios_native_contract_tests)
add_test(NAME klogg_ios_native_contract_tests COMMAND klogg_ios_native_contract_tests)
set_tests_properties(klogg_ios_native_contract_tests PROPERTIES TIMEOUT 120)
klogg_set_test_isolation(klogg_ios_native_contract_tests)

# Task 6 cycle 1 RED: versioned, typed, source-neutral live-log session spec
# contracts (livelogsession.h). Pure data plumbing only — no devices,
# processes, or toolchain environment — so the target stays deterministic and
# narrow until the production module lands.
add_executable(
  klogg_livelog_session_spec_tests
  livelog_session_spec_test.cpp
  pure_catch_main.cpp
)
target_link_libraries(
  klogg_livelog_session_spec_tests
  PRIVATE klogg_ui klogg_livecapture Catch2 Qt${QT_VERSION_MAJOR}::Core project_options
          project_warnings
)
klogg_configure_test_target(klogg_livelog_session_spec_tests)
add_test(NAME klogg_livelog_session_spec_tests COMMAND klogg_livelog_session_spec_tests)
set_tests_properties(klogg_livelog_session_spec_tests PROPERTIES TIMEOUT 120)
klogg_set_test_isolation(klogg_livelog_session_spec_tests)

# Task 6 cycle 2 RED: restore arming contracts. Restored tabs must route
# through the typed accept gate before anything arms a transport; valid
# Running intents drive the state reducer into a waiting state; rejected
# specs surface structured rejections without arming. Drives the real
# Session/WindowSession restore path through a recording transport factory
# and fake views — no devices, no processes, no waits.
add_executable(
  klogg_livelog_restore_arming_tests
  livelog_restore_arming_test.cpp
  livelog_restore_arming_main.cpp
)
target_link_libraries(
  klogg_livelog_restore_arming_tests
  PRIVATE klogg_ui klogg_livecapture Catch2 Qt${QT_VERSION_MAJOR}::Core project_options
          project_warnings
)
klogg_configure_test_target(klogg_livelog_restore_arming_tests)
add_test(NAME klogg_livelog_restore_arming_tests COMMAND klogg_livelog_restore_arming_tests)
# Session restore exercises the process-global default CaptureStore root with
# fixed persisted capture ids. Keep it isolated from parallel test executables
# that may create/remove the same root while identity checks are in flight.
set_tests_properties(klogg_livelog_restore_arming_tests PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE)
klogg_set_test_isolation(klogg_livelog_restore_arming_tests)

# Task 6 cycle 2 RED: once-per-capture-id migration notices. Intentionally
# NOT compilable yet — Session has lastRestoreRejections() but no notice
# channel, so this target fails to build until lastRestoreNotices() exists.
add_executable(
  klogg_livelog_migration_notice_tests
  livelog_migration_notice_test.cpp
  livelog_restore_arming_main.cpp
)
target_link_libraries(
  klogg_livelog_migration_notice_tests
  PRIVATE klogg_ui klogg_livecapture Catch2 Qt${QT_VERSION_MAJOR}::Core project_options
          project_warnings
)
klogg_configure_test_target(klogg_livelog_migration_notice_tests)
add_test(NAME klogg_livelog_migration_notice_tests COMMAND klogg_livelog_migration_notice_tests)
set_tests_properties(klogg_livelog_migration_notice_tests PROPERTIES TIMEOUT 120)
klogg_set_test_isolation(klogg_livelog_migration_notice_tests)

# Task 6 cycle 3 RED: one source-neutral controller per live tab owns the
# reducer snapshot, generation-aware effects, and retry scheduler. The test
# uses a manual monotonic clock/scheduler and recording effect sink: no Qt
# event-loop waits, wall clock, processes, or devices.
add_executable(
  klogg_livelog_controller_tests
  livelog_controller_test.cpp
  pure_catch_main.cpp
)
target_link_libraries(
  klogg_livelog_controller_tests
  PRIVATE klogg_ui klogg_livecapture klogg_adb_smartsocket Catch2
          Qt${QT_VERSION_MAJOR}::Core project_options project_warnings
)
klogg_configure_test_target(klogg_livelog_controller_tests)
add_test(NAME klogg_livelog_controller_tests COMMAND klogg_livelog_controller_tests)
set_tests_properties(klogg_livelog_controller_tests PROPERTIES TIMEOUT 120)
klogg_set_test_isolation(klogg_livelog_controller_tests)

# Task 7 cycle 2: deterministic, device-free contracts for the synthetic
# process-vs-integrated live-capture benchmark. The process child and benchmark
# core remain benchmark-only; the focused contract target is built by ci_build.
if(KLOGG_BUILD_BENCHMARKS)
  add_executable(
    klogg_live_capture_benchmark_contract_tests
    live_capture_process_integrated_benchmark_test.cpp
    live_capture_benchmark_main.cpp
  )
  target_link_libraries(
    klogg_live_capture_benchmark_contract_tests
    PRIVATE klogg_live_capture_benchmark_core klogg_livecapture klogg_logdata Catch2
            Qt${QT_VERSION_MAJOR}::Core project_options project_warnings
  )
  add_dependencies(klogg_live_capture_benchmark_contract_tests live_capture_fixture_producer)
  target_compile_definitions(
    klogg_live_capture_benchmark_contract_tests
    PRIVATE KLOGG_LIVE_CAPTURE_FIXTURE_PRODUCER_PATH="$<TARGET_FILE:live_capture_fixture_producer>"
  )
  klogg_configure_test_target(klogg_live_capture_benchmark_contract_tests)
  add_test(
    NAME klogg_live_capture_benchmark_contract_tests
    COMMAND klogg_live_capture_benchmark_contract_tests
  )
  set_tests_properties(klogg_live_capture_benchmark_contract_tests PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE)
klogg_set_test_isolation(klogg_live_capture_benchmark_contract_tests)
endif()

# Add test cpp file
add_executable(klogg_tests
    adb_device_tracker_manager_test.cpp
    adb_live_services_composition_test.cpp
    adb_protocol_test.cpp
    adb_server_supervisor_test.cpp
    adb_smart_socket_client_test.cpp
    adb_smart_socket_transport_test.cpp
    adb_ui_transport_test.cpp
    ansicolorprocessor_test.cpp
    containers_test.cpp
    wrappedstring_test.cpp
    iosdeviceparser_test.cpp
    ios_live_services_composition_test.cpp
    ios_native_transport_test.cpp
    capturestore_test.cpp
    cli_test.cpp
    colorlabelsmanager_test.cpp
    configuration_test.cpp
    encodingdetector_test.cpp
    droppathclassification_test.cpp
    device_discovery_test.cpp
    filterfavoritesmodel_test.cpp
    recentfolders_test.cpp
    highlighter_vectorscan_test.cpp
    linehighlight_compose_test.cpp
    match_projection_test.cpp
    match_selection_test.cpp
    quickfind_snapshot_test.cpp
    indexedcontent_test.cpp
    linepositionarray_test.cpp
    livecapture_state_test.cpp
    live_data_queue_test.cpp
    live_source_transport_contract_test.cpp
    mergefileorder_test.cpp
    overview_test.cpp
    optionsdialog_test.cpp
    pathutils_test.cpp
    patternmatcher_test.cpp
    quicklabelpattern_test.cpp
    qt_event_queue_test.cpp
    searchgeneration_test.cpp
    searchtoolbar_test.cpp
    selection_test.cpp
    logfiltereddataworker_test.cpp
    sessioninfo_test.cpp
    shortcut_bindings_test.cpp
    styles_test.cpp
    streaminglogdata_test.cpp
    coalescingtimer_test.cpp
    tabgroupdropresolver_test.cpp
    tabgroupmanager_test.cpp
    tabbedcrawlerwidget_test.cpp
    filewatcher_test.cpp
    efsw_inotify_test.cpp
    folderenumeration_test.cpp
    foldersearchengine_test.cpp
    foldersearchengine_parallel_test.cpp
    foldersearchresults_test.cpp
    foldersearchresults_streaming_test.cpp
    versionchecker_test.cpp
    newversiondialog_test.cpp
    updatedownload_test.cpp
    live_save_split_test.cpp
    live_save_crash_repro_test.cpp
    tests_main.cpp
)

target_link_libraries(klogg_tests klogg_ui klogg_utils klogg_logging klogg_versioncheck klogg_livecapture klogg_adb_smartsocket Catch2 Qt${QT_VERSION_MAJOR}::Network Qt${QT_VERSION_MAJOR}::Test test_utils test_crash_trace)
add_dependencies(klogg_tests adb_server_test_helper)
add_dependencies(klogg_tests search_start_empty_env_launcher)
target_compile_definitions(
  klogg_tests PRIVATE KLOGG_ADB_SERVER_TEST_HELPER_PATH="$<TARGET_FILE:adb_server_test_helper>"
)
target_include_directories(klogg_tests PRIVATE "${CMAKE_SOURCE_DIR}/src/app")
target_include_directories(klogg_tests PRIVATE "${CMAKE_BINARY_DIR}/src/ui/klogg_ui_autogen/include")
set_target_properties(klogg_tests PROPERTIES AUTOMOC ON)
klogg_configure_test_target(klogg_tests KEEP_DEBUG_SYMBOLS)

klogg_add_runtime_test(klogg_tests klogg_tests)

# The opt-in is cached on first use, so absence, an empty value, and "0" each
# need a fresh process. Hidden tags keep these out of the main run.
foreach(_klogg_trace_mode enabled empty absent)
  set(_klogg_trace_tag "[.search-start-enabled]")
  if(_klogg_trace_mode STREQUAL "absent")
    set(_klogg_trace_env "--unset=KLOGG_TEST_TRACE_SEARCH_STARTS")
    set(_klogg_trace_tag "[.search-start-absent]")
  elseif(_klogg_trace_mode STREQUAL "empty")
    set(_klogg_trace_tag "[.search-start-enabled],[.search-start-empty]")
  else()
    set(_klogg_trace_env "KLOGG_TEST_TRACE_SEARCH_STARTS=0")
  endif()
  set(_klogg_trace_runtime_env)
  if(WIN32)
    set(_klogg_trace_runtime_path
      "PATH=$<TARGET_FILE_DIR:klogg_tests>$<SEMICOLON>$<TARGET_FILE_DIR:Qt${QT_VERSION_MAJOR}::Core>$<SEMICOLON>$ENV{PATH}"
    )
    string(REPLACE ";" "\\;" _klogg_trace_runtime_path "${_klogg_trace_runtime_path}")
    list(APPEND _klogg_trace_runtime_env "${_klogg_trace_runtime_path}")
  endif()
  set(_klogg_trace_test "klogg_search_start_trace_${_klogg_trace_mode}")
  if(_klogg_trace_mode STREQUAL "empty")
    add_test(
      NAME ${_klogg_trace_test}
      COMMAND ${CMAKE_COMMAND} -E env ${_klogg_trace_runtime_env}
              $<TARGET_FILE:search_start_empty_env_launcher>
              $<TARGET_FILE:klogg_tests> "${_klogg_trace_tag}"
    )
  else()
    add_test(
      NAME ${_klogg_trace_test}
      COMMAND ${CMAKE_COMMAND} -E env ${_klogg_trace_runtime_env} "${_klogg_trace_env}"
              $<TARGET_FILE:klogg_tests> -platform offscreen "${_klogg_trace_tag}"
    )
  endif()
  set_tests_properties(${_klogg_trace_test} PROPERTIES TIMEOUT 120)
  klogg_set_test_isolation(${_klogg_trace_test})
endforeach()
