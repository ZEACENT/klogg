# add_link_options() was introduced in CMake 3.13, while klogg supports 3.12.
# Keep one directory-wide compatibility path so source-built dependencies and
# final executables receive the same sanitizer runtime flags.
macro(klogg_add_legacy_link_options)
  if(MSVC)
    foreach(_klogg_link_option ${ARGN})
      foreach(_klogg_linker_flags_var
              CMAKE_EXE_LINKER_FLAGS
              CMAKE_SHARED_LINKER_FLAGS
              CMAKE_MODULE_LINKER_FLAGS)
        string(APPEND ${_klogg_linker_flags_var} " ${_klogg_link_option}")
        set(${_klogg_linker_flags_var} "${${_klogg_linker_flags_var}}" PARENT_SCOPE)
      endforeach()
    endforeach()
  else()
    link_libraries(${ARGN})
  endif()
endmacro()

macro(klogg_add_link_options)
  if(COMMAND add_link_options)
    add_link_options(${ARGN})
  else()
    klogg_add_legacy_link_options(${ARGN})
  endif()
endmacro()

function(enable_sanitizers project_name)

  # Sanitizer options are declared once and shared across compilers; each
  # compiler branch below honors only the sanitizer subset it supports.
  option(ENABLE_SANITIZER_ADDRESS "Enable address sanitizer" FALSE)
  option(ENABLE_SANITIZER_MEMORY "Enable memory sanitizer" FALSE)
  option(ENABLE_SANITIZER_UNDEFINED_BEHAVIOR "Enable undefined behavior sanitizer" FALSE)
  option(ENABLE_SANITIZER_THREAD "Enable thread sanitizer" FALSE)

  if(ENABLE_SANITIZER_ADDRESS OR ENABLE_SANITIZER_MEMORY
     OR ENABLE_SANITIZER_UNDEFINED_BEHAVIOR OR ENABLE_SANITIZER_THREAD)
    set(KLOGG_ANY_SANITIZER ON CACHE INTERNAL "A sanitizer build is enabled" FORCE)
  else()
    set(KLOGG_ANY_SANITIZER OFF CACHE INTERNAL "A sanitizer build is enabled" FORCE)
  endif()

  if(KLOGG_ANY_SANITIZER)
    target_compile_definitions(${project_name} INTERFACE KLOGG_SANITIZER_BUILD=1)
  endif()
  if(ENABLE_SANITIZER_ADDRESS)
    target_compile_definitions(${project_name} INTERFACE KLOGG_ASAN_BUILD=1)
  endif()
  if(ENABLE_SANITIZER_MEMORY)
    target_compile_definitions(${project_name} INTERFACE KLOGG_MSAN_BUILD=1)
  endif()
  if(ENABLE_SANITIZER_UNDEFINED_BEHAVIOR)
    target_compile_definitions(${project_name} INTERFACE KLOGG_UBSAN_BUILD=1)
  endif()
  if(ENABLE_SANITIZER_THREAD)
    target_compile_definitions(${project_name} INTERFACE KLOGG_TSAN_BUILD=1)
  endif()

  if(KLOGG_ANY_SANITIZER
     AND NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU"
     AND NOT CMAKE_CXX_COMPILER_ID MATCHES ".*Clang"
     AND NOT CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    message(
      FATAL_ERROR
        "Sanitizer builds are not configured for compiler '${CMAKE_CXX_COMPILER_ID}'"
    )
  endif()

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
      if(NOT CMAKE_CXX_COMPILER_ID MATCHES ".*Clang")
        message(
          FATAL_ERROR
            "Klogg ThreadSanitizer builds require Clang because mimalloc's TSan instrumentation is Clang-only"
        )
      endif()
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
      if(ENABLE_SANITIZER_UNDEFINED_BEHAVIOR)
        # Keep the target-local vptr disable in the same shell option group as
        # -fsanitize=undefined. CMake de-duplicates standalone compile options;
        # without this group, the directory-wide -fno-sanitize=vptr is retained
        # before the target's combined -fsanitize=address,undefined flag and the
        # later flag silently re-enables vptr for first-party translation units.
        target_compile_options(
          ${project_name}
          INTERFACE "SHELL:-fsanitize=${LIST_OF_SANITIZERS} -fno-sanitize=vptr"
        )
      else()
        target_compile_options(${project_name} INTERFACE -fsanitize=${LIST_OF_SANITIZERS})
      endif()
      target_link_libraries(${project_name} INTERFACE -fsanitize=${LIST_OF_SANITIZERS})

      # Sanitizers must cover source-built dependencies too. project_options is
      # linked by first-party targets only, so also apply each sanitizer at this
      # directory before 3rdparty/ is added. Keep non-C/C++ tools (for example
      # resource compilers) out of the global compile flags.
      foreach(_klogg_sanitizer IN LISTS SANITIZERS)
        add_compile_options(
          "$<$<OR:$<COMPILE_LANGUAGE:C>,$<COMPILE_LANGUAGE:CXX>>:-fsanitize=${_klogg_sanitizer}>"
        )
        klogg_add_link_options(-fsanitize=${_klogg_sanitizer})
      endforeach()
    endif()
  endif()

  # UBSan's -fsanitize=undefined enables the 'vptr' sub-check, which flags any
  # static_cast/downcast whose target type is not the object's dynamic type.
  # The vendored TBB flow_graph implementation
  # (oneapi/tbb/detail/_flow_graph_impl.h) relies on intentional "tagged"
  # downcasts such as forward_task_bypass that legitimately violate this rule;
  # the same idiom also appears in Qt internals. The check cannot be satisfied
  # without patching TBB, and vptr is not suppressible at runtime via
  # UBSAN_OPTIONS. Disable just vptr while keeping the rest of UBSan active
  # (alignment, bool, bounds, integer, null, ...). -fno-sanitize=vptr must be
  # passed to BOTH the compiler and the linker AND appear after -fsanitize=...
  # so the later flag wins (GCC/Clang process these flags left-to-right).
  if(ENABLE_SANITIZER_UNDEFINED_BEHAVIOR
     AND (CMAKE_CXX_COMPILER_ID STREQUAL "GNU"
          OR CMAKE_CXX_COMPILER_ID MATCHES ".*Clang"))
    target_link_libraries(${project_name} INTERFACE -fno-sanitize=vptr)
    add_compile_options(
      "$<$<OR:$<COMPILE_LANGUAGE:C>,$<COMPILE_LANGUAGE:CXX>>:-fno-sanitize=vptr>"
    )
    klogg_add_link_options(-fno-sanitize=vptr)
  endif()

  if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    # MSVC supports only AddressSanitizer. Fail configuration rather than
    # silently producing an unsanitized binary when another sanitizer was
    # explicitly requested.
    if(ENABLE_SANITIZER_MEMORY)
      message(FATAL_ERROR "MemorySanitizer is not supported by MSVC")
    endif()
    if(ENABLE_SANITIZER_UNDEFINED_BEHAVIOR)
      message(FATAL_ERROR "UndefinedBehaviorSanitizer is not supported by MSVC")
    endif()
    if(ENABLE_SANITIZER_THREAD)
      message(FATAL_ERROR "ThreadSanitizer is not supported by MSVC")
    endif()

    # /INCREMENTAL:NO is required by the ASan linker instrumentation.
    if(ENABLE_SANITIZER_ADDRESS)
      if(CMAKE_GENERATOR_PLATFORM)
        set(_klogg_msvc_target_arch "${CMAKE_GENERATOR_PLATFORM}")
      else()
        set(_klogg_msvc_target_arch "${CMAKE_SYSTEM_PROCESSOR}")
      endif()
      if(CMAKE_SIZEOF_VOID_P EQUAL 4
         OR NOT _klogg_msvc_target_arch MATCHES "^(x64|X64|AMD64|amd64|x86_64|X86_64)$")
        message(
          FATAL_ERROR
            "MSVC AddressSanitizer is supported only for x64 builds, not '${_klogg_msvc_target_arch}'"
        )
      endif()

      # MSVC ASan stamps every C/C++ TU with container-annotation metadata.
      # Vendored static libraries must carry the same flag to avoid LNK2038,
      # but resource files must not receive a C/C++ compiler option.
      add_compile_options(
        "$<$<OR:$<COMPILE_LANGUAGE:C>,$<COMPILE_LANGUAGE:CXX>>:/fsanitize=address>"
      )
      target_compile_definitions(${project_name} INTERFACE KLOGG_MSVC_ASAN=1)
      klogg_add_link_options(/INCREMENTAL:NO)
    endif()
  endif()

endfunction()
