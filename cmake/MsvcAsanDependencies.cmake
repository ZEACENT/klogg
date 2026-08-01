# Under MSVC AddressSanitizer, Vectorscan's optimized force-inlined state-machine
# compiler/runtime consistently aborts in mcclellanExec16_i with an unclassified
# access violation. Keep those dependency targets fully instrumented, but compile
# them without optimization in this one diagnostic configuration so ASan can
# execute the dependency checks reliably. Release/package builds and unrelated
# targets retain their normal optimization settings.
function(klogg_stabilize_msvc_asan_dependencies)
  if(NOT MSVC OR NOT ENABLE_SANITIZER_ADDRESS)
    return()
  endif()

  foreach(_klogg_dependency_target ${ARGN})
    if(NOT TARGET ${_klogg_dependency_target})
      message(
        FATAL_ERROR
          "Required MSVC ASan dependency target '${_klogg_dependency_target}' does not exist"
      )
    endif()

    target_compile_options(${_klogg_dependency_target} PRIVATE /Od /Ob0)
  endforeach()
endfunction()
