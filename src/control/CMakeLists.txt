# Local control transport and console frontend. Keep this boundary independent
# of Widgets, settings, allocators, and log processing.
add_library(klogg_control
  include/controlframing.h
  src/controlframing.cpp
  include/controlrpc.h
  src/controlrpc.cpp
  include/controlruntime.h
  include/controltransport.h
  include/controlcli.h
  src/controlruntime_pal.h
  src/controlruntime_pal.cpp
  src/controlruntime.cpp
  src/controltransport.cpp
  src/controlcli.cpp
  include/controlinstall.h
  src/controlinstall.cpp
  src/controlinstall_pal.h
  src/controlinstall_pal.cpp
  src/controlfilesystemsecurity_p.h
)
target_compile_definitions(klogg_control PUBLIC QT_NO_KEYWORDS)
target_include_directories(klogg_control
  PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
  PRIVATE "${PROJECT_SOURCE_DIR}/src/utils/include"
)
target_link_libraries(klogg_control
  PUBLIC Qt${QT_VERSION_MAJOR}::Core project_options project_warnings
  PRIVATE Qt${QT_VERSION_MAJOR}::Network
)
if(WIN32)
  target_link_libraries(klogg_control PRIVATE advapi32)
endif()
