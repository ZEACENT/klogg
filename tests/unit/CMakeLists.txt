# Add test cpp file
add_executable(klogg_tests
    adb_ui_transport_test.cpp
    ansicolorprocessor_test.cpp
    wrappedstring_test.cpp
    iosdeviceparser_test.cpp
    capturestore_test.cpp
    cli_test.cpp
    colorlabelsmanager_test.cpp
    configuration_test.cpp
    highlighter_vectorscan_test.cpp
    linepositionarray_test.cpp
    patternmatcher_test.cpp
    quicklabelpattern_test.cpp
    searchgeneration_test.cpp
    logfiltereddataworker_test.cpp
    sessioninfo_test.cpp
    shortcut_bindings_test.cpp
    streaminglogdata_test.cpp
    tabgroupdropresolver_test.cpp
    tabgroupmanager_test.cpp
    tabbedcrawlerwidget_test.cpp
    tests_main.cpp
)

target_link_libraries(klogg_tests klogg_ui klogg_utils klogg_logging Catch2 Qt${QT_VERSION_MAJOR}::Test test_utils)
target_include_directories(klogg_tests PRIVATE "${CMAKE_SOURCE_DIR}/src/app")
target_include_directories(klogg_tests PRIVATE "${CMAKE_BINARY_DIR}/src/ui/klogg_ui_autogen/include")
set_target_properties(klogg_tests PROPERTIES AUTOMOC ON)
klogg_configure_test_target(klogg_tests)

klogg_add_runtime_test(klogg_tests klogg_tests)
