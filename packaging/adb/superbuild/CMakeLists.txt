cmake_minimum_required(VERSION 3.20)
project(klogg_adb_helper_superbuild LANGUAGES NONE)

include(ExternalProject)

set(KLOGG_ADB_SOURCE_ROOT "" CACHE PATH "Verified, extracted ADB source closure")
set(KLOGG_ADB_INSTALL_PREFIX "" CACHE PATH "Private ADB helper installation prefix")
set(KLOGG_ADB_TARGET "" CACHE STRING "Locked ADB helper target identifier")

if(NOT KLOGG_ADB_SOURCE_ROOT OR NOT IS_DIRECTORY "${KLOGG_ADB_SOURCE_ROOT}")
  message(FATAL_ERROR "verified ADB source root is missing: ${KLOGG_ADB_SOURCE_ROOT}")
endif()
if(NOT KLOGG_ADB_INSTALL_PREFIX)
  message(FATAL_ERROR "ADB helper install prefix is missing")
endif()

set(_common_args
  -DCMAKE_BUILD_TYPE=Release
  -DCMAKE_INSTALL_PREFIX=${KLOGG_ADB_INSTALL_PREFIX}
  -DCMAKE_POSITION_INDEPENDENT_CODE=ON
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5
  -DBUILD_SHARED_LIBS=OFF
  -DFETCHCONTENT_FULLY_DISCONNECTED=ON
  -DFETCHCONTENT_UPDATES_DISCONNECTED=ON
)
set(_platform_args)
if(APPLE)
  execute_process(
    COMMAND xcrun --sdk macosx --show-sdk-path
    OUTPUT_VARIABLE _klogg_adb_macos_sdk
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE _klogg_adb_xcrun_result
  )
  if(NOT _klogg_adb_xcrun_result EQUAL 0 OR NOT IS_DIRECTORY "${_klogg_adb_macos_sdk}")
    message(FATAL_ERROR "unable to resolve the locked macOS SDK sysroot")
  endif()
  list(APPEND _platform_args -DCMAKE_OSX_SYSROOT=${_klogg_adb_macos_sdk})
  if(KLOGG_ADB_TARGET STREQUAL "macos-x86_64")
    list(APPEND _platform_args -DCMAKE_OSX_ARCHITECTURES=x86_64 -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0)
  elseif(KLOGG_ADB_TARGET STREQUAL "macos-arm64")
    list(APPEND _platform_args -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=14.0)
  else()
    message(FATAL_ERROR "unsupported macOS ADB helper target: ${KLOGG_ADB_TARGET}")
  endif()
elseif(WIN32)
  if(NOT KLOGG_ADB_TARGET STREQUAL "windows-x86_64")
    message(FATAL_ERROR "unsupported Windows ADB helper target: ${KLOGG_ADB_TARGET}")
  endif()
  find_program(KLOGG_ADB_GCC_EXECUTABLE NAMES gcc.exe REQUIRED)
  find_program(KLOGG_ADB_GXX_EXECUTABLE NAMES g++.exe REQUIRED)
  find_program(KLOGG_ADB_NASM_EXECUTABLE NAMES nasm.exe REQUIRED)
  list(APPEND _platform_args
    -DCMAKE_C_COMPILER=${KLOGG_ADB_GCC_EXECUTABLE}
    -DCMAKE_CXX_COMPILER=${KLOGG_ADB_GXX_EXECUTABLE}
    -DCMAKE_ASM_NASM_COMPILER=${KLOGG_ADB_NASM_EXECUTABLE}
    -DCMAKE_EXE_LINKER_FLAGS=-static-libgcc\ -static-libstdc++
  )
elseif(UNIX)
  if(NOT KLOGG_ADB_TARGET MATCHES "^linux-(x86_64|arm64)$")
    message(FATAL_ERROR "unsupported Linux ADB helper target: ${KLOGG_ADB_TARGET}")
  endif()
  # Keep the helper's C++ runtime ABI inside the source-built executable. The
  # package contract then depends only on the locked GLIBC baseline and private
  # libusb closure, not the host distribution's libstdc++/libgcc versions.
  list(APPEND _platform_args
    -DCMAKE_EXE_LINKER_FLAGS=-static-libgcc\ -static-libstdc++\ -Wl,--enable-new-dtags
  )
else()
  message(FATAL_ERROR "unsupported ADB helper host for ${KLOGG_ADB_TARGET}")
endif()
list(APPEND _common_args ${_platform_args})

set(_zlib_static_library "${KLOGG_ADB_INSTALL_PREFIX}/lib/libz.a")
if(WIN32)
  set(_zlib_static_library "${KLOGG_ADB_INSTALL_PREFIX}/lib/libzlibstatic.a")
endif()

ExternalProject_Add(adb_zlib
  CMAKE_GENERATOR "${CMAKE_GENERATOR}"
  SOURCE_DIR "${KLOGG_ADB_SOURCE_ROOT}/zlib"
  CMAKE_ARGS ${_common_args} -DZLIB_BUILD_EXAMPLES=OFF
  BUILD_BYPRODUCTS "${_zlib_static_library}"
)

ExternalProject_Add(adb_brotli
  CMAKE_GENERATOR "${CMAKE_GENERATOR}"
  SOURCE_DIR "${KLOGG_ADB_SOURCE_ROOT}/brotli"
  CMAKE_ARGS ${_common_args} -DBROTLI_DISABLE_TESTS=ON
  BUILD_BYPRODUCTS "${KLOGG_ADB_INSTALL_PREFIX}/lib/libbrotlicommon.a"
)

ExternalProject_Add(adb_lz4
  CMAKE_GENERATOR "${CMAKE_GENERATOR}"
  SOURCE_DIR "${KLOGG_ADB_SOURCE_ROOT}/lz4"
  SOURCE_SUBDIR build/cmake
  CMAKE_ARGS ${_common_args} -DLZ4_BUILD_CLI=OFF -DLZ4_BUILD_LEGACY_LZ4C=OFF
  BUILD_BYPRODUCTS "${KLOGG_ADB_INSTALL_PREFIX}/lib/liblz4.a"
)

ExternalProject_Add(adb_zstd
  CMAKE_GENERATOR "${CMAKE_GENERATOR}"
  SOURCE_DIR "${KLOGG_ADB_SOURCE_ROOT}/zstd"
  SOURCE_SUBDIR build/cmake
  CMAKE_ARGS ${_common_args}
    -DZSTD_BUILD_PROGRAMS=OFF
    -DZSTD_BUILD_TESTS=OFF
    -DZSTD_BUILD_SHARED=OFF
    -DZSTD_BUILD_STATIC=ON
  BUILD_BYPRODUCTS "${KLOGG_ADB_INSTALL_PREFIX}/lib/libzstd.a"
)

ExternalProject_Add(adb_googletest
  CMAKE_GENERATOR "${CMAKE_GENERATOR}"
  SOURCE_DIR "${KLOGG_ADB_SOURCE_ROOT}/googletest"
  CMAKE_ARGS ${_common_args}
    -DINSTALL_GTEST=ON
    -DBUILD_GMOCK=OFF
  BUILD_BYPRODUCTS "${KLOGG_ADB_INSTALL_PREFIX}/include/gtest/gtest_prod.h"
)

ExternalProject_Add(adb_protobuf
  CMAKE_GENERATOR "${CMAKE_GENERATOR}"
  SOURCE_DIR "${KLOGG_ADB_SOURCE_ROOT}/protobuf"
  CMAKE_ARGS ${_common_args}
    -Dprotobuf_BUILD_TESTS=OFF
    -Dprotobuf_BUILD_SHARED_LIBS=OFF
    -Dprotobuf_WITH_ZLIB=ON
    -DZLIB_ROOT=${KLOGG_ADB_INSTALL_PREFIX}
    -DZLIB_LIBRARY=${_zlib_static_library}
    -DZLIB_LIBRARY_RELEASE=${_zlib_static_library}
    -DCMAKE_PREFIX_PATH=${KLOGG_ADB_INSTALL_PREFIX}
  DEPENDS adb_zlib
  BUILD_BYPRODUCTS
    "${KLOGG_ADB_INSTALL_PREFIX}/lib/libprotobuf.a"
    "${KLOGG_ADB_INSTALL_PREFIX}/bin/protoc"
)

set(_windows_api_dependency)
if(WIN32)
  ExternalProject_Add(adb_windows_api
    SOURCE_DIR "${KLOGG_ADB_SOURCE_ROOT}/windows-platform-development/klogg"
    CMAKE_GENERATOR "${CMAKE_GENERATOR}"
    CMAKE_ARGS
      -DCMAKE_BUILD_TYPE=Release
      -DCMAKE_INSTALL_PREFIX=${KLOGG_ADB_INSTALL_PREFIX}
      -DCMAKE_C_COMPILER=cl.exe
      -DCMAKE_CXX_COMPILER=cl.exe
      -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded
      -DFETCHCONTENT_FULLY_DISCONNECTED=ON
      -DFETCHCONTENT_UPDATES_DISCONNECTED=ON
    BUILD_BYPRODUCTS
      "${KLOGG_ADB_INSTALL_PREFIX}/bin/AdbWinApi.dll"
      "${KLOGG_ADB_INSTALL_PREFIX}/bin/AdbWinUsbApi.dll"
  )
  set(_windows_api_dependency adb_windows_api)
endif()

find_program(KLOGG_ADB_PKG_CONFIG_EXECUTABLE NAMES pkg-config pkg-config.exe REQUIRED)
if(WIN32)
  # A literal semicolon is a CMake list separator and would split the
  # `cmake -E env` argument. MinGW installs this closure's pkg-config files
  # under lib/pkgconfig, so use the single controlled prefix on Windows.
  set(_pkg_config_path "${KLOGG_ADB_INSTALL_PREFIX}/lib/pkgconfig")
else()
  set(_pkg_config_path "${KLOGG_ADB_INSTALL_PREFIX}/lib/pkgconfig:${KLOGG_ADB_INSTALL_PREFIX}/lib64/pkgconfig")
endif()
set(_android_configure
  ${CMAKE_COMMAND} -E env
  "PKG_CONFIG_PATH=${_pkg_config_path}"
  "PKG_CONFIG_LIBDIR=${_pkg_config_path}"
  "PKG_CONFIG_SYSROOT_DIR="
  ${CMAKE_COMMAND}
  -G "${CMAKE_GENERATOR}"
  -S "${KLOGG_ADB_SOURCE_ROOT}/android-tools-release"
  -B "<BINARY_DIR>"
  ${_common_args}
  -DCMAKE_PREFIX_PATH=${KLOGG_ADB_INSTALL_PREFIX}
  -DZLIB_LIBRARY=${_zlib_static_library}
  -DZLIB_LIBRARY_RELEASE=${_zlib_static_library}
  -DCMAKE_FIND_ROOT_PATH=${KLOGG_ADB_INSTALL_PREFIX}
  -DCMAKE_FIND_ROOT_PATH_MODE_PROGRAM=NEVER
  -DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=ONLY
  -DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY
  -DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=ONLY
  -DPKG_CONFIG_EXECUTABLE=${KLOGG_ADB_PKG_CONFIG_EXECUTABLE}
  -DCMAKE_C_FLAGS=-I${KLOGG_ADB_INSTALL_PREFIX}/include
  -DCMAKE_CXX_FLAGS=-I${KLOGG_ADB_INSTALL_PREFIX}/include
  -DCMAKE_BUILD_WITH_INSTALL_RPATH=ON
  -DANDROID_TOOLS_PATCH_VENDOR=OFF
  -DANDROID_TOOLS_USE_BUNDLED_FMT=ON
  -DANDROID_TOOLS_USE_BUNDLED_LIBUSB=ON
  -DANDROID_TOOLS_LIBUSB_ENABLE_UDEV=OFF
  -DKLOGG_ADB_ONLY=ON
  -DKLOGG_ADB_GTEST_INCLUDE_DIR=${KLOGG_ADB_INSTALL_PREFIX}/include
)
if(APPLE)
  list(APPEND _android_configure -DKLOGG_ADB_PRIVATE_LIBUSB=OFF)
else()
  list(APPEND _android_configure -DKLOGG_ADB_PRIVATE_LIBUSB=ON)
endif()

if(WIN32)
  set(_android_install
    ${CMAKE_COMMAND} -E make_directory "${KLOGG_ADB_INSTALL_PREFIX}/bin"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "<BINARY_DIR>/vendor/adb.exe" "${KLOGG_ADB_INSTALL_PREFIX}/bin/adb.exe"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "<BINARY_DIR>/vendor/libusb-1.0.dll" "${KLOGG_ADB_INSTALL_PREFIX}/bin/libusb-1.0.dll"
  )
  set(_adb_byproduct "${KLOGG_ADB_INSTALL_PREFIX}/bin/adb.exe")
else()
  set(_android_install ${CMAKE_COMMAND} --build <BINARY_DIR> --target install)
  set(_adb_byproduct "${KLOGG_ADB_INSTALL_PREFIX}/bin/adb")
endif()

ExternalProject_Add(adb_android_tools
  SOURCE_DIR "${KLOGG_ADB_SOURCE_ROOT}/android-tools-release"
  CONFIGURE_COMMAND ${_android_configure}
  BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --target adb
  INSTALL_COMMAND ${_android_install}
  DEPENDS adb_brotli adb_googletest adb_lz4 adb_protobuf adb_zstd adb_zlib ${_windows_api_dependency}
  BUILD_BYPRODUCTS "${_adb_byproduct}"
)

add_custom_target(klogg_adb_helper DEPENDS adb_android_tools)
