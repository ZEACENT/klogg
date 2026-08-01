find_package(QT NAMES Qt6 Qt5 COMPONENTS Test REQUIRED)
find_package(Qt${QT_VERSION_MAJOR} COMPONENTS Test REQUIRED)

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
add_dependencies(ci_build klogg_tests klogg_itests)






