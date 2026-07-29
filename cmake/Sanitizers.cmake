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
      # MSVC ASan stamps every TU compiled with /fsanitize=address with
      # container-annotation metadata (annotate_vector / annotate_string).
      # The linker raises LNK2038 when objects with mismatched values are
      # mixed: instrumented klogg TUs (value 1) cannot link with
      # uninstrumented vendored static libs such as efsw/kdtoolbox/simdutf
      # (value 0). Every object in the binary must therefore carry the
      # flag, not just the klogg targets. target_compile_options on
      # project_options only reaches targets that link that INTERFACE
      # library, so the flag is applied GLOBALLY (current directory and
      # below) so the vendored deps inherit it. This works because
      # enable_sanitizers() is called from the top-level CMakeLists.txt
      # before add_subdirectory(3rdparty) pulls in those dependencies.
      # add_compile_options covers both C and CXX, which is required since
      # some deps (whereami.c, uchardet) are C.
      add_compile_options(/fsanitize=address)
      add_link_options(/INCREMENTAL:NO)
    endif()
  endif()

endfunction()
