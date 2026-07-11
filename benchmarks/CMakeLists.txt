add_executable(
  regex_search_benchmark
  ${CMAKE_CURRENT_SOURCE_DIR}/regex_search_benchmark.cpp
)

add_executable(
  folder_search_benchmark
  ${CMAKE_CURRENT_SOURCE_DIR}/folder_search_benchmark.cpp
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

add_dependencies(ci_build regex_search_benchmark folder_search_benchmark)
