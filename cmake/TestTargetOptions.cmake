function(klogg_configure_test_target target)
  set(_klogg_test_options KEEP_DEBUG_SYMBOLS)
  cmake_parse_arguments(
    KLOGG_TEST
    "${_klogg_test_options}"
    ""
    ""
    ${ARGN}
  )

  if(KLOGG_TEST_UNPARSED_ARGUMENTS)
    message(
      FATAL_ERROR
        "Unknown test target options: ${KLOGG_TEST_UNPARSED_ARGUMENTS}"
    )
  endif()

  if(MSVC)
    if(KLOGG_TEST_KEEP_DEBUG_SYMBOLS)
      set(_klogg_debug_option /DEBUG:FULL)
    else()
      set(_klogg_debug_option /DEBUG:NONE)
    endif()

    # target_link_options() requires CMake 3.13. Keep the equivalent
    # configuration-specific target property for the supported 3.12 baseline.
    set_property(
      TARGET ${target}
      APPEND_STRING
      PROPERTY LINK_FLAGS_RELWITHDEBINFO
               " ${_klogg_debug_option} /INCREMENTAL:NO"
    )
  endif()
endfunction()
