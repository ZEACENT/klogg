function(klogg_verify_tsan_qt)
  if(NOT ENABLE_SANITIZER_THREAD)
    return()
  endif()

  if(NOT KLOGG_TSAN_QT_PREFIX)
    message(FATAL_ERROR "Linux TSan builds require KLOGG_TSAN_QT_PREFIX")
  endif()
  if(NOT KLOGG_TSAN_QT_VERSION)
    message(FATAL_ERROR "Linux TSan builds require KLOGG_TSAN_QT_VERSION")
  endif()
  if(NOT QT_VERSION_MAJOR EQUAL 5)
    message(FATAL_ERROR "Linux TSan builds require Qt 5, found Qt ${QT_VERSION_MAJOR}")
  endif()
  if(NOT KLOGG_QT_VERSION VERSION_EQUAL KLOGG_TSAN_QT_VERSION)
    message(
      FATAL_ERROR
        "Linux TSan requires Qt ${KLOGG_TSAN_QT_VERSION}, found ${KLOGG_QT_VERSION}"
    )
  endif()

  get_filename_component(_klogg_tsan_qt_prefix "${KLOGG_TSAN_QT_PREFIX}" REALPATH)
  if(NOT IS_DIRECTORY "${_klogg_tsan_qt_prefix}")
    message(FATAL_ERROR "KLOGG_TSAN_QT_PREFIX does not exist: ${KLOGG_TSAN_QT_PREFIX}")
  endif()

  if(NOT CMAKE_NM OR NOT EXISTS "${CMAKE_NM}")
    message(FATAL_ERROR "CMAKE_NM is required to verify the TSan Qt runtime")
  endif()

  set(_klogg_saw_qt_core FALSE)
  foreach(_klogg_qt_target ${ARGN})
    if(NOT TARGET ${_klogg_qt_target})
      message(FATAL_ERROR "Required TSan Qt target does not exist: ${_klogg_qt_target}")
    endif()

    set(_klogg_location_properties IMPORTED_LOCATION)
    get_target_property(
      _klogg_imported_configurations
      ${_klogg_qt_target}
      IMPORTED_CONFIGURATIONS
    )
    if(_klogg_imported_configurations
       AND NOT _klogg_imported_configurations MATCHES "-NOTFOUND$")
      foreach(_klogg_imported_configuration ${_klogg_imported_configurations})
        string(TOUPPER "${_klogg_imported_configuration}" _klogg_imported_configuration)
        list(APPEND
             _klogg_location_properties
             "IMPORTED_LOCATION_${_klogg_imported_configuration}"
        )
      endforeach()
    endif()
    foreach(_klogg_standard_configuration
            DEBUG
            RELEASE
            RELWITHDEBINFO
            MINSIZEREL
            NOCONFIG)
      list(APPEND
           _klogg_location_properties
           "IMPORTED_LOCATION_${_klogg_standard_configuration}"
      )
    endforeach()
    list(REMOVE_DUPLICATES _klogg_location_properties)

    set(_klogg_qt_locations)
    foreach(_klogg_location_property ${_klogg_location_properties})
      get_target_property(
        _klogg_candidate_location
        ${_klogg_qt_target}
        ${_klogg_location_property}
      )
      if(_klogg_candidate_location
         AND NOT _klogg_candidate_location MATCHES "-NOTFOUND$")
        list(APPEND _klogg_qt_locations "${_klogg_candidate_location}")
      endif()
    endforeach()
    list(REMOVE_DUPLICATES _klogg_qt_locations)

    if(NOT _klogg_qt_locations)
      message(
        FATAL_ERROR
          "Required TSan Qt target ${_klogg_qt_target} has no imported library"
      )
    endif()

    if(_klogg_qt_target STREQUAL "Qt5::Core")
      set(_klogg_saw_qt_core TRUE)
    endif()

    foreach(_klogg_qt_location ${_klogg_qt_locations})
      if(NOT EXISTS "${_klogg_qt_location}")
        message(
          FATAL_ERROR
            "Required TSan Qt target ${_klogg_qt_target} has a missing imported library: ${_klogg_qt_location}"
        )
      endif()

      get_filename_component(_klogg_qt_location "${_klogg_qt_location}" REALPATH)
      string(
        FIND
        "${_klogg_qt_location}"
        "${_klogg_tsan_qt_prefix}/"
        _klogg_prefix_position
      )
      if(NOT _klogg_prefix_position EQUAL 0)
        message(
          FATAL_ERROR
            "Qt target ${_klogg_qt_target} resolves outside KLOGG_TSAN_QT_PREFIX: ${_klogg_qt_location}"
        )
      endif()

      execute_process(
        COMMAND "${CMAKE_NM}" -D --undefined-only "${_klogg_qt_location}"
        RESULT_VARIABLE _klogg_nm_result
        OUTPUT_VARIABLE _klogg_qt_symbols
        ERROR_VARIABLE _klogg_nm_error
      )
      if(NOT _klogg_nm_result EQUAL 0)
        message(
          FATAL_ERROR
            "Failed to inspect ${_klogg_qt_target} with ${CMAKE_NM}: ${_klogg_nm_error}"
        )
      endif()

      string(
        REGEX MATCH
        "__tsan_(func_entry|read|write)"
        _klogg_tsan_compiler_marker
        "${_klogg_qt_symbols}"
      )
      if(NOT _klogg_tsan_compiler_marker)
        message(
          FATAL_ERROR
            "${_klogg_qt_target} is not compiler-instrumented for TSan: ${_klogg_qt_location}"
        )
      endif()

      if(_klogg_qt_target STREQUAL "Qt5::Core")
        foreach(_klogg_tsan_annotation __tsan_acquire __tsan_release)
          string(
            FIND
            "${_klogg_qt_symbols}"
            "${_klogg_tsan_annotation}"
            _klogg_symbol_position
          )
          if(_klogg_symbol_position EQUAL -1)
            message(
              FATAL_ERROR
                "Qt5::Core is not TSan-aware: missing ${_klogg_tsan_annotation} in ${_klogg_qt_location}"
            )
          endif()
        endforeach()
      endif()
    endforeach()
  endforeach()

  if(NOT _klogg_saw_qt_core)
    message(FATAL_ERROR "Qt5::Core must be included in the TSan Qt verification set")
  endif()
endfunction()
