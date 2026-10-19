# Ordinary CI opt-in, supplied through CMAKE_PROJECT_INCLUDE. This deliberately
# does not change the developer minimum or the qualified producer recipes.
# Always run scripts/verify_macos_tsan_profile.py on the generated compilation
# database before building: source-local and transitive options can follow a
# target's options, so configure-time intent alone is not sufficient evidence.
if(NOT KLOGG_CI_MACOS_TSAN_O1)
  return()
endif()

if(CMAKE_VERSION VERSION_LESS 3.19)
  message(FATAL_ERROR "KLOGG_CI_MACOS_TSAN_O1 requires CMake 3.19 or newer")
endif()
include_guard(GLOBAL)

function(_klogg_ci_macos_tsan_targets directory)
  get_property(_targets DIRECTORY "${directory}" PROPERTY BUILDSYSTEM_TARGETS)
  foreach(_target IN LISTS _targets)
    get_target_property(_type "${_target}" TYPE)
    if(_type MATCHES "^(EXECUTABLE|STATIC_LIBRARY|SHARED_LIBRARY|MODULE_LIBRARY|OBJECT_LIBRARY)$")
      get_target_property(_ipo "${_target}" INTERPROCEDURAL_OPTIMIZATION)
      get_target_property(_ipo_config "${_target}" INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO)
      if(_ipo OR _ipo_config)
        message(FATAL_ERROR "KLOGG_CI_MACOS_TSAN_O1 forbids LTO on ${_target}")
      endif()
      # DEFER runs after dependency targets have supplied their own -O3 flags.
      # Grouping keeps CMake's option de-duplication from retaining an earlier
      # standalone -O1 in place of this final override. Assembly is unaffected.
      # Do not append debug flags: mimalloc intentionally adds -g after the
      # project's -g1, and an optimization profile must retain its diagnostics.
      set_property(TARGET "${_target}" APPEND PROPERTY COMPILE_OPTIONS
        "$<$<COMPILE_LANGUAGE:C,CXX>:SHELL:-O1>")
    endif()
  endforeach()
  get_property(_children DIRECTORY "${directory}" PROPERTY SUBDIRECTORIES)
  foreach(_child IN LISTS _children)
    _klogg_ci_macos_tsan_targets("${_child}")
  endforeach()
endfunction()

function(_klogg_ci_macos_tsan_finalize)
  if(NOT CMAKE_SYSTEM_NAME STREQUAL "Darwin"
     OR NOT CMAKE_C_COMPILER_ID STREQUAL "AppleClang"
     OR NOT CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang"
     OR NOT CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64"
     OR (CMAKE_OSX_ARCHITECTURES AND NOT CMAKE_OSX_ARCHITECTURES STREQUAL "x86_64")
     OR CMAKE_CROSSCOMPILING
     OR NOT CMAKE_GENERATOR STREQUAL "Ninja"
     OR NOT CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo"
     OR NOT ENABLE_SANITIZER_THREAD
     OR ENABLE_SANITIZER_ADDRESS
     OR ENABLE_SANITIZER_MEMORY
     OR ENABLE_SANITIZER_UNDEFINED_BEHAVIOR
     OR ENABLE_COVERAGE
     OR NOT KLOGG_CI_LIGHT_DEBUG
     OR NOT DEFINED KLOGG_USE_LTO
     OR KLOGG_USE_LTO
     OR CMAKE_INTERPROCEDURAL_OPTIMIZATION
     OR CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO
     OR NOT CMAKE_EXPORT_COMPILE_COMMANDS)
    message(FATAL_ERROR
      "KLOGG_CI_MACOS_TSAN_O1 requires native Intel macOS AppleClang, Ninja, RelWithDebInfo, TSan only, KLOGG_CI_LIGHT_DEBUG=ON, KLOGG_USE_LTO=OFF, no coverage/IPO, and CMAKE_EXPORT_COMPILE_COMMANDS=ON")
  endif()
  _klogg_ci_macos_tsan_targets("${CMAKE_SOURCE_DIR}")
endfunction()

# CMAKE_PROJECT_INCLUDE also runs for nested project() calls. Register once at
# the root, after compiler identification, and inspect final settings/targets.
cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL _klogg_ci_macos_tsan_finalize)
