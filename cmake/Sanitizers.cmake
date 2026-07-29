function(enable_sanitizers project_name)

  # Sanitizer options are declared once and shared across compilers; each
  # compiler branch below honors only the sanitizer subset it supports.
  option(ENABLE_SANITIZER_ADDRESS "Enable address sanitizer" FALSE)
  option(ENABLE_SANITIZER_MEMORY "Enable memory sanitizer" FALSE)
  option(ENABLE_SANITIZER_UNDEFINED_BEHAVIOR "Enable undefined behavior sanitizer" FALSE)
  option(ENABLE_SANITIZER_THREAD "Enable thread sanitizer" FALSE)

  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID MATCHES ".*Clang")
    option(ENABLE_COVERAGE "Enable coverage reporting for gcc/clang" FALSE)

    if(ENABLE_COVERAGE)
      target_compile_options(${project_name} INTERFACE --coverage -O0 -g)
      target_link_libraries(${project_name} INTERFACE --coverage)
    endif()

    set(SANITIZERS "")

    if(ENABLE_SANITIZER_ADDRESS)
      list(APPEND SANITIZERS "address")
    endif()

    if(ENABLE_SANITIZER_MEMORY)
      list(APPEND SANITIZERS "memory")
    endif()

    if(ENABLE_SANITIZER_UNDEFINED_BEHAVIOR)
      list(APPEND SANITIZERS "undefined")
    endif()

    if(ENABLE_SANITIZER_THREAD)
      list(APPEND SANITIZERS "thread")
    endif()

    list(
      JOIN
      SANITIZERS
      ","
      LIST_OF_SANITIZERS
    )

  endif()

  if(LIST_OF_SANITIZERS)
    if(NOT
       "${LIST_OF_SANITIZERS}"
       STREQUAL
       ""
    )
      target_compile_options(${project_name} INTERFACE -fsanitize=${LIST_OF_SANITIZERS})
      target_link_libraries(${project_name} INTERFACE -fsanitize=${LIST_OF_SANITIZERS})
    endif()
  endif()

  if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    # MSVC supports only the address sanitizer, and only on x64, via
    # /fsanitize=address. LeakSanitizer, UndefinedBehaviorSanitizer and
    # ThreadSanitizer are not available on MSVC, so the other
    # ENABLE_SANITIZER_* options are intentionally ignored here.
    # /INCREMENTAL:NO is required by the ASan linker instrumentation.
    if(ENABLE_SANITIZER_ADDRESS)
      target_compile_options(${project_name} INTERFACE /fsanitize=address)
      target_link_options(${project_name} INTERFACE /INCREMENTAL:NO)
    endif()
  endif()

endfunction()
