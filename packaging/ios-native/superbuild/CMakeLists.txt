cmake_minimum_required(VERSION 3.14)
project(klogg_ios_native_superbuild NONE)

include(ExternalProject)

set(KLOGG_IOS_SOURCE_ROOT "" CACHE PATH "Verified and patched iOS native source root")
set(KLOGG_IOS_INSTALL_PREFIX "" CACHE PATH "Private iOS native install prefix")
set(KLOGG_IOS_ARCHITECTURE "" CACHE STRING "Thin macOS architecture")
set(KLOGG_IOS_DEPLOYMENT_TARGET "" CACHE STRING "Locked macOS deployment target")
set(KLOGG_IOS_PARALLEL 2 CACHE STRING "Parallel build jobs")
set(KLOGG_IOS_SOURCE_DATE_EPOCH "" CACHE STRING "Locked reproducible-build timestamp")

if(NOT APPLE)
  message(FATAL_ERROR "The iOS native superbuild is macOS-only")
endif()
foreach(_required KLOGG_IOS_SOURCE_ROOT KLOGG_IOS_INSTALL_PREFIX KLOGG_IOS_ARCHITECTURE KLOGG_IOS_DEPLOYMENT_TARGET KLOGG_IOS_SOURCE_DATE_EPOCH)
  if(NOT ${_required})
    message(FATAL_ERROR "Missing required disconnected superbuild setting: ${_required}")
  endif()
endforeach()
if(NOT KLOGG_IOS_ARCHITECTURE MATCHES "^(x86_64|arm64)$")
  message(FATAL_ERROR "Unsupported thin iOS native architecture: ${KLOGG_IOS_ARCHITECTURE}")
endif()

set(_env
    ${CMAKE_COMMAND} -E env
    MACOSX_DEPLOYMENT_TARGET=${KLOGG_IOS_DEPLOYMENT_TARGET}
    SOURCE_DATE_EPOCH=${KLOGG_IOS_SOURCE_DATE_EPOCH}
    ZERO_AR_DATE=1
    CC=clang
    CXX=clang++
    "CFLAGS=-arch ${KLOGG_IOS_ARCHITECTURE} -mmacosx-version-min=${KLOGG_IOS_DEPLOYMENT_TARGET} -O2"
    "CXXFLAGS=-arch ${KLOGG_IOS_ARCHITECTURE} -mmacosx-version-min=${KLOGG_IOS_DEPLOYMENT_TARGET} -O2"
    CPPFLAGS=-I${KLOGG_IOS_INSTALL_PREFIX}/include
    "LDFLAGS=-arch ${KLOGG_IOS_ARCHITECTURE} -mmacosx-version-min=${KLOGG_IOS_DEPLOYMENT_TARGET} -L${KLOGG_IOS_INSTALL_PREFIX}/lib"
    PKG_CONFIG_PATH=${KLOGG_IOS_INSTALL_PREFIX}/lib/pkgconfig
    PKG_CONFIG_LIBDIR=${KLOGG_IOS_INSTALL_PREFIX}/lib/pkgconfig
    LIBTOOLIZE=glibtoolize
    ac_cv_path_CYTHON=no
)

if(KLOGG_IOS_ARCHITECTURE STREQUAL "x86_64")
  set(_openssl_target darwin64-x86_64-cc)
else()
  set(_openssl_target darwin64-arm64-cc)
endif()

ExternalProject_Add(
  ios_openssl
  SOURCE_DIR ${KLOGG_IOS_SOURCE_ROOT}/openssl
  DOWNLOAD_COMMAND ""
  UPDATE_COMMAND ""
  PATCH_COMMAND ""
  CONFIGURE_COMMAND
    ${_env} perl <SOURCE_DIR>/Configure ${_openssl_target} shared no-tests no-apps
    --prefix=${KLOGG_IOS_INSTALL_PREFIX}
    --openssldir=${KLOGG_IOS_INSTALL_PREFIX}/ssl
    -mmacosx-version-min=${KLOGG_IOS_DEPLOYMENT_TARGET}
  BUILD_COMMAND ${_env} make -j${KLOGG_IOS_PARALLEL}
  INSTALL_COMMAND ${_env} make install_sw
  BUILD_IN_SOURCE YES
)

ExternalProject_Add(
  ios_curl
  SOURCE_DIR ${KLOGG_IOS_SOURCE_ROOT}/curl
  DOWNLOAD_COMMAND ""
  UPDATE_COMMAND ""
  PATCH_COMMAND ""
  CONFIGURE_COMMAND
    ${_env} <SOURCE_DIR>/configure --prefix=${KLOGG_IOS_INSTALL_PREFIX}
    --disable-static --enable-shared --with-openssl=${KLOGG_IOS_INSTALL_PREFIX}
    --enable-ca-native --without-ca-bundle --without-ca-path --without-ca-fallback
    --without-zlib --without-brotli --without-zstd
    --without-libpsl --without-libidn2 --without-libssh2 --without-nghttp2
    --without-ngtcp2 --without-nghttp3 --without-quiche --disable-ldap
    --disable-ldaps --disable-rtsp --disable-dict --disable-telnet --disable-tftp
    --disable-pop3 --disable-imap --disable-smb --disable-smtp --disable-gopher
    --disable-mqtt --disable-manual --disable-docs
  BUILD_COMMAND ${_env} make -j${KLOGG_IOS_PARALLEL}
  INSTALL_COMMAND ${_env} make install
  BUILD_IN_SOURCE YES
  DEPENDS ios_openssl
)

function(klogg_ios_autotools_project target source dependency)
  set(_options ${ARGN})
  ExternalProject_Add(
    ${target}
    SOURCE_DIR ${KLOGG_IOS_SOURCE_ROOT}/${source}
    DOWNLOAD_COMMAND ""
    UPDATE_COMMAND ""
    PATCH_COMMAND ""
    CONFIGURE_COMMAND
      ${_env} <SOURCE_DIR>/autogen.sh --prefix=${KLOGG_IOS_INSTALL_PREFIX}
      --disable-static --enable-shared ${_options}
    BUILD_COMMAND ${_env} make -j${KLOGG_IOS_PARALLEL}
    INSTALL_COMMAND ${_env} make install
    BUILD_IN_SOURCE YES
    DEPENDS ${dependency}
  )
endfunction()

klogg_ios_autotools_project(
  ios_libplist libplist ios_openssl --without-cython --without-tools
)
klogg_ios_autotools_project(
  ios_libtatsu libtatsu "ios_libplist;ios_curl"
)
klogg_ios_autotools_project(
  ios_libimobiledevice_glue libimobiledevice-glue ios_libplist
)
klogg_ios_autotools_project(
  ios_libusbmuxd libusbmuxd ios_libimobiledevice_glue
)
klogg_ios_autotools_project(
  ios_libimobiledevice libimobiledevice "ios_libusbmuxd;ios_libtatsu" --without-cython
)

add_custom_target(klogg_ios_native_stack DEPENDS ios_libimobiledevice)
