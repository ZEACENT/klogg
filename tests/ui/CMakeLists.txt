set(UI_TEST_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/qtests_main.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/logdata_test.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/logfiltereddata_test.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/crawlerwidget_test.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/concatenatedlogdata_test.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/foldercrawler_test.cpp
)

LIST(APPEND UI_TEST_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/mainwindow_test.cpp)

add_executable(klogg_itests ${UI_TEST_SOURCES} ${BACKWARD_ENABLE})

target_link_libraries(klogg_itests klogg_ui klogg_utils Catch2 Qt${QT_VERSION_MAJOR}::Test test_utils)
set_target_properties(klogg_itests PROPERTIES AUTOMOC ON)
klogg_configure_test_target(klogg_itests)

add_backward(klogg_itests)

klogg_add_runtime_test(klogg_itests klogg_itests)
if(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8)
  set_tests_properties(klogg_itests PROPERTIES RUN_SERIAL TRUE)
endif()
