add_executable(file_write_helper
        file_write_helper.cpp
        file_write_helper.h
)

target_link_libraries(file_write_helper klogg_utils tbb)

add_library(test_utils INTERFACE)
target_include_directories(test_utils SYSTEM INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

# First-chance crash trace for test binaries: Catch2's Windows SEH handler
# swallows fatal exceptions for reporting, so WER never writes a dump; this
# handler prints the faulting stack (dbghelp honors _NT_SYMBOL_PATH from the
# CI runner) before Catch2's handler runs.
add_library(test_crash_trace STATIC
        crash_trace.cpp
        crash_trace.h
)
target_link_libraries(test_crash_trace PUBLIC test_utils)
if(WIN32)
  target_link_libraries(test_crash_trace PUBLIC dbghelp)
endif()