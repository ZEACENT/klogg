add_executable(
  regex_search_benchmark
  ${CMAKE_CURRENT_SOURCE_DIR}/regex_search_benchmark.cpp
)

add_executable(
  folder_search_benchmark
  ${CMAKE_CURRENT_SOURCE_DIR}/folder_search_benchmark.cpp
)

add_executable(
  ios_ostrace_protocol_benchmark
  ${CMAKE_CURRENT_SOURCE_DIR}/ios_ostrace_protocol_benchmark.cpp
)

# Benchmark-only synthetic live-capture stack. The protocol library stays pure
# C++ so the fixture producer does not pull the UI/logdata dependency closure;
# only the comparison runner links the production transport/controller/sink.
add_library(
  klogg_live_capture_benchmark_protocol STATIC
  ${CMAKE_CURRENT_SOURCE_DIR}/live_capture_benchmark_protocol.cpp
)
target_include_directories(
  klogg_live_capture_benchmark_protocol PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(
  klogg_live_capture_benchmark_protocol PUBLIC project_options project_warnings
)

add_library(
  klogg_live_capture_benchmark_core STATIC
  ${CMAKE_CURRENT_SOURCE_DIR}/live_capture_benchmark_core.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/live_capture_real_device_benchmark.cpp
)
target_link_libraries(
  klogg_live_capture_benchmark_core
  PUBLIC klogg_live_capture_benchmark_protocol project_options project_warnings
  PRIVATE klogg_ui klogg_livecapture klogg_logdata Qt${QT_VERSION_MAJOR}::Core
)

add_executable(
  live_capture_fixture_producer
  ${CMAKE_CURRENT_SOURCE_DIR}/live_capture_fixture_producer.cpp
)
target_link_libraries(
  live_capture_fixture_producer PRIVATE klogg_live_capture_benchmark_protocol
)

add_executable(
  live_capture_process_integrated_benchmark
  ${CMAKE_CURRENT_SOURCE_DIR}/live_capture_process_integrated_benchmark.cpp
)
target_link_libraries(
  live_capture_process_integrated_benchmark
  PRIVATE klogg_live_capture_benchmark_core klogg_logdata Qt${QT_VERSION_MAJOR}::Core
)
add_dependencies(live_capture_process_integrated_benchmark live_capture_fixture_producer)
target_compile_definitions(
  live_capture_process_integrated_benchmark
  PRIVATE KLOGG_LIVE_CAPTURE_FIXTURE_PRODUCER_PATH="$<TARGET_FILE:live_capture_fixture_producer>"
)

target_link_libraries(
  regex_search_benchmark
  PRIVATE project_options
          project_warnings
          klogg_logdata
          klogg_logging
          klogg_settings
          Qt${QT_VERSION_MAJOR}::Core
)

if(KLOGG_USE_LTO)
  set_property(TARGET regex_search_benchmark PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()

target_link_libraries(
  folder_search_benchmark
  PRIVATE project_options
          project_warnings
          klogg_logdata
          klogg_logging
          klogg_settings
          Qt${QT_VERSION_MAJOR}::Core
)

if(KLOGG_USE_LTO)
  set_property(TARGET folder_search_benchmark PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()

target_link_libraries(
  ios_ostrace_protocol_benchmark
  PRIVATE project_options project_warnings klogg_livecapture
)

if(KLOGG_USE_LTO)
  set_property(TARGET ios_ostrace_protocol_benchmark PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()

add_dependencies(
  ci_build regex_search_benchmark folder_search_benchmark ios_ostrace_protocol_benchmark
)
