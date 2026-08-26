add_library(
  klogg_livecapture STATIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include/adbprotocol.h
  ${CMAKE_CURRENT_SOURCE_DIR}/include/ioscatalogprovider.h
  ${CMAKE_CURRENT_SOURCE_DIR}/include/iosdevicecatalog.h
  ${CMAKE_CURRENT_SOURCE_DIR}/include/iosnativeadapter.h
  ${CMAKE_CURRENT_SOURCE_DIR}/include/iosnativeapi.h
  ${CMAKE_CURRENT_SOURCE_DIR}/include/iosnativeerrors.h
  ${CMAKE_CURRENT_SOURCE_DIR}/include/iosnativestream.h
  ${CMAKE_CURRENT_SOURCE_DIR}/include/iosostraceprotocol.h
  ${CMAKE_CURRENT_SOURCE_DIR}/include/livedataqueue.h
  ${CMAKE_CURRENT_SOURCE_DIR}/include/livedatastatistics.h
  ${CMAKE_CURRENT_SOURCE_DIR}/include/livestate.h
  ${CMAKE_CURRENT_SOURCE_DIR}/src/adbprotocol.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/iosdevicecatalog.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/iosnativeadapter.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/iosnativeapi.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/iosnativeerrors.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/iosnativestream.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/iosostraceprotocol.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/livedataqueue.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/livedatastatistics.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/livestate.cpp
)

target_include_directories(klogg_livecapture PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(klogg_livecapture PUBLIC project_options project_warnings)
# iosnativeadapter resolves the verified private closure exclusively with dlopen.
# KLOGG_ENABLE_IOS_NATIVE_STACK remains a configure-time fail-closed artifact
# check in 3rdparty/CMakeLists.txt; linking the dylibs here would make macdeployqt
# rewrite or duplicate the private closure into the general Frameworks directory.

add_library(
  klogg_adb_smartsocket STATIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include/adbdevicetracker.h
  ${CMAKE_CURRENT_SOURCE_DIR}/include/adbinfrastructuremanager.h
  ${CMAKE_CURRENT_SOURCE_DIR}/include/adbserversupervisor.h
  ${CMAKE_CURRENT_SOURCE_DIR}/include/adbsmartsocketclient.h
  ${CMAKE_CURRENT_SOURCE_DIR}/src/adbdevicetracker.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/adbinfrastructuremanager.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/adbserveradapters.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/adbserversupervisor.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/adbsmartsocketclient.cpp
)

target_include_directories(klogg_adb_smartsocket PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(
  klogg_adb_smartsocket
  PUBLIC klogg_livecapture project_options project_warnings Qt${QT_VERSION_MAJOR}::Network
)
set_target_properties(klogg_adb_smartsocket PROPERTIES AUTOMOC ON)
