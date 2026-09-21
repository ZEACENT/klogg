set(UI_TEST_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/qtests_main.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/logdata_test.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/logfiltereddata_test.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/crawlerwidget_test.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/filewatcher_lifecycle_test.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/concatenatedlogdata_test.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/foldercrawler_test.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/foldercrawler_pending_failure_test.cpp
)

LIST(APPEND UI_TEST_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/mainwindow_test.cpp)

add_executable(klogg_itests ${UI_TEST_SOURCES})

target_link_libraries(klogg_itests klogg_ui klogg_utils Catch2 Qt${QT_VERSION_MAJOR}::Test test_utils test_crash_trace)
set_target_properties(klogg_itests PROPERTIES AUTOMOC ON)
klogg_configure_test_target(klogg_itests)

klogg_add_runtime_test(klogg_itests klogg_itests)
if(UNIX)
  klogg_add_runtime_test(
    klogg_itests_filewatcher_low_fd
    klogg_itests
    "[.filewatcher-lifecycle]"
  )
  set_tests_properties(
    klogg_itests_filewatcher_low_fd
    PROPERTIES
      LABELS "ui;filewatcher;resource-limit"
      RUN_SERIAL TRUE
      SKIP_RETURN_CODE 77
  )

  klogg_add_runtime_test(
    klogg_itests_filewatcher_teardown_timeout
    klogg_itests
    "[.filewatcher-teardown-timeout]"
  )
  set_tests_properties(
    klogg_itests_filewatcher_teardown_timeout
    PROPERTIES RUN_SERIAL TRUE LABELS "ui;filewatcher;lifecycle"
  )
endif()
# Windows itests have a history of latent cross-test races (destruction-order
# UAF class) that surface under CPU contention from sibling test processes;
# keep the binary serialized against everything on both architectures.
if(WIN32)
  set_tests_properties(klogg_itests PROPERTIES RUN_SERIAL TRUE)
endif()
