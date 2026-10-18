# Windows CI opt-in preload and compiler-rules override. Run the transformation
# AFTER Windows-MSVC initializes flags, but BEFORE CMake caches them. Replacing
# whole config flag lists before project() would drop /MD and /MDd under the
# project's legacy CMP0091 policy, changing the CRT ABI used by Qt and ASan.
set(CMAKE_USER_MAKE_RULES_OVERRIDE "${CMAKE_CURRENT_LIST_FILE}"
    CACHE FILEPATH "Windows CI per-object debug information override")

# Dependencies opting into CMP0141 NEW use this property instead of flags.
set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT Embedded CACHE STRING "Per-object debug information")

# On the initial -C preload these variables are not initialized yet. This same
# file is included again by CMake's C/CXX compiler-information modules, after
# platform/toolchain defaults exist. Preserve CRT, optimizations, custom flags,
# linker /DEBUG/PDB generation and LTO; change only the compiler debug format.
foreach(_klogg_cache_language C CXX)
  foreach(_klogg_cache_config DEBUG RELWITHDEBINFO)
    set(_klogg_cache_flags "CMAKE_${_klogg_cache_language}_FLAGS_${_klogg_cache_config}_INIT")
    if(DEFINED ${_klogg_cache_flags})
      string(REGEX REPLACE "(^|[ \t])[-/]Z[iI]($|[ \t])" "\\1/Z7\\2"
             ${_klogg_cache_flags} "${${_klogg_cache_flags}}")
    endif()
  endforeach()
endforeach()
unset(_klogg_cache_flags)
unset(_klogg_cache_config)
unset(_klogg_cache_language)
