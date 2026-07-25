add_executable(
  klogg_vectorscan_tests
  vectorscan_tests.cpp
)

target_link_libraries(
  klogg_vectorscan_tests
  PRIVATE klogg_ui
          klogg_utils
          Catch2
          Qt${QT_VERSION_MAJOR}::Test
          test_utils
          klogg_mimalloc_wrapper
)
klogg_configure_test_target(klogg_vectorscan_tests)

target_compile_definitions(klogg_vectorscan_tests PUBLIC -DKLOGG_TESTS)

add_dependencies(ci_build klogg_vectorscan_tests)

klogg_add_runtime_test(klogg_vectorscan_tests klogg_vectorscan_tests)
