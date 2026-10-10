add_executable(file_write_helper
        file_write_helper.cpp
        file_write_helper.h
)

target_link_libraries(file_write_helper klogg_utils tbb)

add_library(test_utils INTERFACE)
target_include_directories(test_utils SYSTEM INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

# First-chance crash trace for test binaries: Catch2's Windows SEH handler
# swallows fatal exceptions for reporting, so WER never writes a dump; this
# handler requests an opt-in minimal dump from a restricted child collector
# and prints the original context before Catch2's handler runs (dbghelp honors
# the runner's _NT_SYMBOL_PATH for parent-process stack symbolization).
add_library(test_crash_trace STATIC
        crash_trace.cpp
        crash_trace.h
        crash_collector_protocol.h
)
target_link_libraries(test_crash_trace PUBLIC test_utils PRIVATE project_options project_warnings)
if(WIN32)
  target_link_libraries(test_crash_trace PUBLIC dbghelp)
  add_executable(crash_trace_collector collector.cpp)
  target_link_libraries(crash_trace_collector PRIVATE test_utils dbghelp project_options project_warnings)
  klogg_configure_test_target(crash_trace_collector)
  add_dependencies(test_crash_trace crash_trace_collector)
  add_executable(crash_trace_test_helper crash_trace_test_helper.cpp)
  target_link_libraries(crash_trace_test_helper PRIVATE test_crash_trace project_options project_warnings)
  klogg_configure_test_target(crash_trace_test_helper)
endif()