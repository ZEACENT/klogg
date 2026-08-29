if(APPLE)
  set(OS_BUNDLE MACOSX_BUNDLE)
elseif(WIN32)
  set(OS_BUNDLE WIN32)
endif()

configure_file(${CMAKE_CURRENT_SOURCE_DIR}/documentation.qrc.in ${DOCUMENTATION_RESOURCE} @ONLY)

add_custom_command(
  OUTPUT ${DOCUMENTATION_HTML}
  COMMAND $<TARGET_FILE:maddy> ${DOCUMENTATION_FILE} ${DOCUMENTATION_HTML}
  DEPENDS maddy ${DOCUMENTATION_FILE}
)

set(TS_FILES
  ${CMAKE_CURRENT_SOURCE_DIR}/i18n/en.ts
  ${CMAKE_CURRENT_SOURCE_DIR}/i18n/zh_CN.ts
  ${CMAKE_CURRENT_SOURCE_DIR}/i18n/zh_TW.ts
)

find_package(Qt${QT_VERSION_MAJOR}LinguistTools REQUIRED)
if (${QT_VERSION_MAJOR} EQUAL 5)
  qt5_add_translation(QM_FILES ${TS_FILES})
else (${QT_VERSION_MAJOR} EQUAL 6)
  qt6_add_translation(QM_FILES ${TS_FILES})
endif()

if (NOT TARGET Qt${QT_VERSION_MAJOR}::lupdate)
    message(FATAL_ERROR "The package \"Qt${QT_VERSION_MAJOR}LinguistTools\" is required.")
endif()
set(Qt_LUPDATE_EXECUTABLE Qt${QT_VERSION_MAJOR}::lupdate)
set(Qt_LCONVERT_EXECUTABLE Qt${QT_VERSION_MAJOR}::lconvert)
add_custom_target(lupdate
    COMMAND ${Qt_LUPDATE_EXECUTABLE} -recursive -no-obsolete ${CMAKE_SOURCE_DIR}/src -ts ${TS_FILES}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/src
    COMMENT "Updating translations"
)

include(TranslationUtils)
add_app_translations_resource(KLOGG_I18N_RES ${QM_FILES})
add_qt_translations_resource(KLOGG_QT_TRANSLATION_RES zh_CN zh_TW)

set(MAIN_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/cli.h
                 ${CMAKE_CURRENT_SOURCE_DIR}/klogg.qrc 
                 ${KLOGG_I18N_RES}
                 ${KLOGG_QT_TRANSLATION_RES})

set(KLOGG_UI_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/messagereceiver.h
    ${CMAKE_CURRENT_SOURCE_DIR}/kloggapp.h
    ${CMAKE_CURRENT_SOURCE_DIR}/main.cpp
    ${DOCUMENTATION_RESOURCE}
    ${ICON_FILE}
    ${COPYING_FILE}
    ${NOTICE_FILE}
)

set(KLOGG_GREP_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/klogg_grep.cpp)

set(MAIN_LIBS
    # klogg_logdata removed - provided by klogg_ui
    # klogg_crash_handler removed - provided by klogg_logdata (transitive)
    # roaring removed - provided by klogg_logdata (transitive)
    kdsingleapp
)

add_executable(klogg ${OS_BUNDLE} ${MAIN_SOURCES} ${KLOGG_UI_SOURCES})
add_executable(klogg_portable ${OS_BUNDLE} ${MAIN_SOURCES} ${KLOGG_UI_SOURCES})
add_executable(klogg_grep ${MAIN_SOURCES} ${KLOGG_GREP_SOURCES})

add_dependencies(klogg klogg_adb_helper)
add_dependencies(klogg_portable klogg_adb_helper)
if(KLOGG_ADB_HELPER_AVAILABLE)
  function(klogg_stage_adb_helper target_name)
    add_custom_command(
      TARGET ${target_name}
      POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E rm -rf "$<TARGET_FILE_DIR:${target_name}>/helpers"
      COMMAND ${CMAKE_COMMAND} -E copy_directory "${KLOGG_ADB_HELPER_STAGE}/helpers" "$<TARGET_FILE_DIR:${target_name}>/helpers"
      VERBATIM
    )
    if(APPLE)
      add_custom_command(
        TARGET ${target_name}
        POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E rm -rf "$<TARGET_BUNDLE_CONTENT_DIR:${target_name}>/SharedSupport/adb-helper"
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_BUNDLE_CONTENT_DIR:${target_name}>/SharedSupport/adb-helper"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${KLOGG_ADB_HELPER_PACKAGE_STAGE}" "$<TARGET_BUNDLE_CONTENT_DIR:${target_name}>/SharedSupport/adb-helper"
        VERBATIM
      )
    endif()
  endfunction()
  klogg_stage_adb_helper(klogg)
  klogg_stage_adb_helper(klogg_portable)

  add_custom_target(klogg_stage_adb_helpers ALL
    COMMAND ${CMAKE_COMMAND} -E rm -rf "$<TARGET_FILE_DIR:klogg>/helpers"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${KLOGG_ADB_HELPER_STAGE}/helpers" "$<TARGET_FILE_DIR:klogg>/helpers"
    DEPENDS klogg klogg_adb_helper
    VERBATIM
  )
  if(APPLE)
    add_custom_command(
      TARGET klogg_stage_adb_helpers
      POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E rm -rf "$<TARGET_BUNDLE_CONTENT_DIR:klogg>/SharedSupport/adb-helper"
      COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_BUNDLE_CONTENT_DIR:klogg>/SharedSupport/adb-helper"
      COMMAND ${CMAKE_COMMAND} -E copy_directory "${KLOGG_ADB_HELPER_PACKAGE_STAGE}" "$<TARGET_BUNDLE_CONTENT_DIR:klogg>/SharedSupport/adb-helper"
      VERBATIM
    )
  elseif(WIN32)
    add_dependencies(klogg_stage_adb_helpers klogg_portable)
    add_custom_command(
      TARGET klogg_stage_adb_helpers
      POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E rm -rf "$<TARGET_FILE_DIR:klogg_portable>/helpers"
      COMMAND ${CMAKE_COMMAND} -E copy_directory "${KLOGG_ADB_HELPER_STAGE}/helpers" "$<TARGET_FILE_DIR:klogg_portable>/helpers"
      VERBATIM
    )
  endif()
endif()

add_dependencies(ci_build klogg klogg_grep)
if(TARGET klogg_stage_adb_helpers)
  add_dependencies(ci_build klogg_stage_adb_helpers)
endif()

if(WIN32)
  add_dependencies(ci_build klogg_portable)
endif()

set_target_properties(klogg PROPERTIES AUTORCC ON)
set_target_properties(klogg PROPERTIES AUTOMOC ON)
set_target_properties(klogg_portable PROPERTIES AUTORCC ON)
set_target_properties(klogg_portable PROPERTIES AUTOMOC ON)
set_target_properties(klogg_grep PROPERTIES AUTORCC ON)
set_target_properties(klogg_grep PROPERTIES AUTOMOC ON)

if(KLOGG_USE_LTO)
  set_property(TARGET klogg PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
  set_property(TARGET klogg_portable PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
  set_property(TARGET klogg_grep PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()

target_link_libraries(klogg PUBLIC ${MAIN_LIBS} klogg_ui)
target_link_libraries(klogg_portable PUBLIC ${MAIN_LIBS} klogg_ui)
# klogg_grep is a CLI tool (no UI) but it still relies on core log processing code.
# It must link klogg_logdata to get required dependencies (including mimalloc headers/libs).
target_link_libraries(klogg_grep PUBLIC ${MAIN_LIBS} klogg_logdata)

target_compile_definitions(klogg_portable PUBLIC -DKLOGG_PORTABLE)

if(WIN32)

  target_sources(klogg PRIVATE ${ProductVersionResourceFiles})
  target_sources(klogg_portable PRIVATE ${ProductVersionResourceFiles})
  target_sources(klogg_grep PRIVATE ${ProductVersionResourceFiles})

elseif(APPLE)
  # Do not let CMake touch rpaths at install time.  macdeployqt handles
  # rpath adjustment; DMG creation uses hdiutil directly (not CPack),
  # so there is no install step that needs rpath manipulation.
  set_target_properties(klogg PROPERTIES SKIP_INSTALL_RPATH ON)
  set_target_properties(klogg_portable PROPERTIES SKIP_INSTALL_RPATH ON)
  if(KLOGG_ENABLE_IOS_NATIVE_STACK)
    # Keep package candidates free of build-machine rpaths. The raw smoke test
    # receives the configured Qt framework directory through its test environment.
    set_target_properties(klogg PROPERTIES SKIP_BUILD_RPATH ON)
    set(KLOGG_QT_PACKAGE_DIR_VARIABLE "Qt${QT_VERSION_MAJOR}_DIR")
    get_filename_component(
      KLOGG_QT_BUILD_RPATH "${${KLOGG_QT_PACKAGE_DIR_VARIABLE}}/../.." ABSOLUTE
    )
    set_target_properties(klogg PROPERTIES KLOGG_QT_BUILD_RPATH "${KLOGG_QT_BUILD_RPATH}")
    target_link_options(
      klogg PRIVATE "LINKER:-rpath,@executable_path/../Frameworks/ios-native/lib"
    )
  endif()
  set_source_files_properties(${ICON_FILE} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
  set_source_files_properties(${NOTICE_FILE} PROPERTIES MACOSX_PACKAGE_LOCATION SharedSupport)
  set_source_files_properties(${COPYING_FILE} PROPERTIES MACOSX_PACKAGE_LOCATION SharedSupport)

  set_source_files_properties(${README_FILE} PROPERTIES MACOSX_PACKAGE_LOCATION SharedSupport)
  set_source_files_properties(${DOCUMENTATION_FILE} PROPERTIES MACOSX_PACKAGE_LOCATION SharedSupport)
  set_source_files_properties(${DOCUMENTATION_HTML} PROPERTIES MACOSX_PACKAGE_LOCATION SharedSupport)

  set(MACOSX_BUNDLE_BUNDLE_DISPLAY_NAME ${PROJECT_DESCRIPTION})
  set(MACOSX_BUNDLE_BUNDLE_NAME ${PROJECT_NAME})
  set(MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION})
  set(MACOSX_BUNDLE_LONG_VERSION_STRING ${PROJECT_VERSION})
  set(MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}")
  set(MACOSX_BUNDLE_COPYRIGHT ${COPYRIGHT})
  set(MACOSX_BUNDLE_GUI_IDENTIFIER ${KLOGG_APPLICATION_ID})
  set(MACOSX_BUNDLE_ICON_FILE ${ICON_NAME})

endif()

install(
  TARGETS klogg
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
          COMPONENT ${KLOGG_RUNTIME_INSTALL_COMPONENT}
  BUNDLE DESTINATION .
         COMPONENT ${KLOGG_RUNTIME_INSTALL_COMPONENT}
)

if(WIN32)
  if(TARGET tbb)
    add_custom_command(
      TARGET klogg
      POST_BUILD
      COMMAND cmake -E copy $<TARGET_FILE:tbb> $<TARGET_FILE_DIR:klogg>
      WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
      COMMENT "Copy tbb"
    )
  endif()
  if(TARGET tbbmalloc_proxy)
    add_custom_command(
      TARGET klogg
      POST_BUILD
      COMMAND cmake -E copy $<TARGET_FILE:tbbmalloc> $<TARGET_FILE_DIR:klogg>
      WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
      COMMENT "Copy tbbmalloc"
    )
    add_custom_command(
      TARGET klogg
      POST_BUILD
      COMMAND cmake -E copy $<TARGET_FILE:tbbmalloc_proxy> $<TARGET_FILE_DIR:klogg>
      WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
      COMMENT "Copy tbbmalloc_proxy"
    )
  endif()
endif()

if(KLOGG_USE_SENTRY)
  if(NOT WIN32)
    if(APPLE)
      find_program(DSYMUTIL dsymutil)
      add_custom_command(
        TARGET klogg
        POST_BUILD
        COMMAND ${DSYMUTIL} --out=${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/klogg.dSym $<TARGET_FILE:klogg>
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Generating symbols for $<TARGET_FILE:klogg>"
      )

      add_custom_command(
        TARGET klogg_portable
        POST_BUILD
        COMMAND ${DSYMUTIL} --out=${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/klogg_portable.dSym $<TARGET_FILE:klogg_portable>
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Generating symbols for $<TARGET_FILE:klogg_portable>"
      )
    else()
      find_program(OBJCOPY objcopy)
      add_custom_command(
        TARGET klogg
        POST_BUILD
        COMMAND ${OBJCOPY} --only-keep-debug --compress-debug-sections=zlib $<TARGET_FILE:klogg>
                ${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/klogg.debug
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Generating symbols for $<TARGET_FILE:klogg>"
      )
      add_custom_command(
        TARGET klogg
        POST_BUILD
        COMMAND ${OBJCOPY} --strip-debug --strip-unneeded $<TARGET_FILE:klogg>
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Stripping $<TARGET_FILE:klogg>"
      )
      add_custom_command(
        TARGET klogg
        POST_BUILD
        COMMAND ${OBJCOPY} --add-gnu-debuglink=${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/klogg.debug $<TARGET_FILE:klogg>
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Linking symbols for $<TARGET_FILE:klogg>"
      )

      add_custom_command(
        TARGET klogg_portable
        POST_BUILD
        COMMAND ${OBJCOPY} --only-keep-debug $<TARGET_FILE:klogg_portable>
                ${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/klogg_portable.debug
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Generating symbols for $<TARGET_FILE:klogg>"
      )
      add_custom_command(
        TARGET klogg_portable
        POST_BUILD
        COMMAND ${OBJCOPY} --strip-debug --strip-unneeded $<TARGET_FILE:klogg_portable>
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Stripping $<TARGET_FILE:klogg_portable>"
      )
      add_custom_command(
        TARGET klogg_portable
        POST_BUILD
        COMMAND ${OBJCOPY} --add-gnu-debuglink=${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/klogg_portable.debug
                $<TARGET_FILE:klogg_portable>
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Linking symbols for $<TARGET_FILE:klogg_portable>"
      )
    endif()

    add_custom_command(
      TARGET klogg
      POST_BUILD
      COMMAND cmake -E copy $<TARGET_FILE:crashpad_handler> $<TARGET_FILE_DIR:klogg>/klogg_crashpad_handler
      WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
      COMMENT "Copy crash_handler"
    )

    # The minidump_dump stack walker was vendored under 3rdparty/dump_syms and
    # removed; crashhandler.cpp degrades gracefully when the binary is absent
    # (logs a warning and skips the pending report instead of deleting it).
  else()
    add_custom_command(
      TARGET klogg
      POST_BUILD
      COMMAND cmake -E copy $<TARGET_FILE:crashpad_handler> $<TARGET_FILE_DIR:klogg>/klogg_crashpad_handler.exe
      WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
      COMMENT "Copy crash_handler"
    )

    # The minidump_dump stack walker was vendored under 3rdparty/dump_syms and
    # removed; crashhandler.cpp degrades gracefully when the binary is absent
    # (logs a warning and skips the pending report instead of deleting it).
  endif()

  if(APPLE)
    install(PROGRAMS $<TARGET_FILE_DIR:klogg>/klogg_crashpad_handler
            DESTINATION $<TARGET_BUNDLE_CONTENT_DIR:klogg>/MacOS
            COMPONENT ${KLOGG_RUNTIME_INSTALL_COMPONENT}
    )
  elseif(NOT WIN32)
    install(PROGRAMS $<TARGET_FILE_DIR:klogg>/klogg_crashpad_handler
            DESTINATION ${CMAKE_INSTALL_BINDIR}
            COMPONENT ${KLOGG_RUNTIME_INSTALL_COMPONENT}
    )
  endif()
endif(KLOGG_USE_SENTRY)
