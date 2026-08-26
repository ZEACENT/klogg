cmake_minimum_required(VERSION 3.14)

project(klogg_prefetch NONE)

# Keep this list in sync with 3rdparty/CMakeLists.txt. This project only warms
# the shared CPM source cache, so every dependency is fetched as DOWNLOAD_ONLY.
include(${CMAKE_CURRENT_LIST_DIR}/../CPM.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../verify_pinned_source.cmake)

set(CPM_USE_LOCAL_PACKAGES OFF)

option(
  KLOGG_FETCH_IOS_NATIVE_DEPENDENCIES
  "Prefetch the pinned native iOS dependency stack"
  OFF
)
if(KLOGG_FETCH_IOS_NATIVE_DEPENDENCIES)
  cpmaddpackage(
    NAME openssl
    GITHUB_REPOSITORY openssl/openssl
    GIT_TAG 8cf17aaeb4599f8af87fefd810b5b5fee90fe69e
    DOWNLOAD_ONLY YES
  )
  cpmaddpackage(
    NAME curl
    GITHUB_REPOSITORY curl/curl
    GIT_TAG 68720b4837284335b2d63cb358f8f6ce65f5bc55
    DOWNLOAD_ONLY YES
  )
  cpmaddpackage(
    NAME libplist
    GITHUB_REPOSITORY libimobiledevice/libplist
    GIT_TAG cf5897a71ea412ea2aeb1e2f6b5ea74d4fabfd8c
    DOWNLOAD_ONLY YES
  )
  cpmaddpackage(
    NAME libtatsu
    GITHUB_REPOSITORY libimobiledevice/libtatsu
    GIT_TAG 42329cb756682535c7c0f087987b78d1dd5b16c8
    DOWNLOAD_ONLY YES
  )
  cpmaddpackage(
    NAME libimobiledevice-glue
    GITHUB_REPOSITORY libimobiledevice/libimobiledevice-glue
    GIT_TAG aef2bf0f5bfe961ad83d224166462d87b1df2b00
    DOWNLOAD_ONLY YES
  )
  cpmaddpackage(
    NAME libusbmuxd
    GITHUB_REPOSITORY libimobiledevice/libusbmuxd
    GIT_TAG adf9c22b9010490e4b55eaeb14731991db1c172c
    DOWNLOAD_ONLY YES
  )
  cpmaddpackage(
    NAME libimobiledevice
    GITHUB_REPOSITORY libimobiledevice/libimobiledevice
    GIT_TAG 149f7623c672c1fa73122c7119a12bfc0012f2ac
    GIT_CONFIG core.autocrlf=false
    DOWNLOAD_ONLY YES
  )
endif()

cpmaddpackage(
  NAME simdutf
  GITHUB_REPOSITORY simdutf/simdutf
  # v5.6.2
  GIT_TAG 58c12ff5d84665e26e80547e5f0d72b29ba1c5d4
  DOWNLOAD_ONLY YES
)
cpmaddpackage(
  NAME type_safe
  GITHUB_REPOSITORY foonathan/type_safe
  # v0.2.4
  GIT_TAG 1dbea7936a1e389caa692d01bb3f5ba4b6da7d82
  DOWNLOAD_ONLY YES
)
cpmaddpackage(
  NAME CRoaring
  GITHUB_REPOSITORY RoaringBitmap/CRoaring
  # v4.2.1
  GIT_TAG 3ef7b44a4f74af09fe19bb84d30bc35cab130561
  DOWNLOAD_ONLY YES
)
cpmaddpackage(
  NAME streamvbyte
  GITHUB_REPOSITORY lemire/streamvbyte
  GIT_TAG c43294a81501e0fdf14adc83818d47f7f9bc1bb6
  GIT_CONFIG core.autocrlf=false
  DOWNLOAD_ONLY YES
)
cpmaddpackage(
  NAME macdeployqtfix
  GITHUB_REPOSITORY arl/macdeployqtfix
  GIT_TAG df888505849d3c06d20a4338af276dfa7d11826a
  DOWNLOAD_ONLY YES
)
cpmaddpackage(
  NAME maddy
  GITHUB_REPOSITORY variar/maddy
  GIT_TAG 602e26613e624535e2de883b3f2c98a16729d1d4
  DOWNLOAD_ONLY YES
)
cpmaddpackage(
  NAME vectorscan
  GITHUB_REPOSITORY VectorCamp/vectorscan
  GIT_TAG d29730e1cb9daaa66bda63426cdce83505d2c809
  GIT_CONFIG core.autocrlf=false
  DOWNLOAD_ONLY YES
)
cpmaddpackage(
  NAME Uchardet
  GIT_REPOSITORY https://gitlab.freedesktop.org/uchardet/uchardet
  # v0.0.8
  GIT_TAG ae6302a016088ad07177f86d417b20010053632b
  DOWNLOAD_ONLY YES
)
cpmaddpackage(
  NAME KF5Archive
  GITHUB_REPOSITORY variar/klogg_karchive
  GIT_TAG f546bf6ae66a8d34b43da5a41afcfbf4e1a47906
  DOWNLOAD_ONLY YES
)
cpmaddpackage(
  NAME robin_hood
  GITHUB_REPOSITORY martinus/robin-hood-hashing
  # 3.11.2
  GIT_TAG f2cae2e6b8231c46fe9c324e31b0a1f64e0de8c6
  DOWNLOAD_ONLY YES
)
cpmaddpackage(
  NAME backward-cpp
  GITHUB_REPOSITORY bombela/backward-cpp
  VERSION 1.6
  DOWNLOAD_ONLY YES
)
cpmaddpackage(
  NAME Catch2
  GITHUB_REPOSITORY catchorg/Catch2
  VERSION 2.13.8
  DOWNLOAD_ONLY YES
)
cpmaddpackage(
  NAME KDSingleApplication
  GITHUB_REPOSITORY variar/KDSingleApplication
  GIT_TAG 5b30db30266f92bc01f1439777803ce8dbf16c79
  DOWNLOAD_ONLY YES
)
cpmaddpackage(
  NAME xxHash
  GITHUB_REPOSITORY Cyan4973/xxHash
  # v0.8.1
  GIT_TAG 35b0373c697b5f160d3db26b1cbb45a0d5ba788c
  DOWNLOAD_ONLY YES
)
cpmaddpackage(
  NAME whereami
  GITHUB_REPOSITORY gpakosz/whereami
  GIT_TAG dcb52a058dc14530ba9ae05e4339bd3ddfae0e0e
  DOWNLOAD_ONLY YES
)
cpmaddpackage(
  NAME exprtk
  GITHUB_REPOSITORY variar/klogg_exprtk
  GIT_TAG 1f9f4cd7d2620b7b24232de9ea22908d63913459
  DOWNLOAD_ONLY YES
)
cpmaddpackage(
  NAME KDToolBox
  GITHUB_REPOSITORY KDAB/KDToolBox
  GIT_TAG 6468867d1a46eabe1bcb2cd342f338fe66f06675
  DOWNLOAD_ONLY YES
)
cpmaddpackage(
  NAME efsw
  GITHUB_REPOSITORY SpartanJ/efsw
  GIT_TAG 62f785c56b7a34f035193d4cb831921347b586b8
  GIT_CONFIG core.autocrlf=false
  DOWNLOAD_ONLY YES
)
cpmaddpackage(
  NAME tbb
  GITHUB_REPOSITORY variar/oneTBB
  GIT_TAG c9be1ac2930f02dea523003ed801b4489f3e6b6e
  DOWNLOAD_ONLY YES
)
# Peeled commit for the annotated v2.1.7 tag; keep this aligned with 3rdparty.
cpmaddpackage(
  NAME mimalloc
  GITHUB_REPOSITORY microsoft/mimalloc
  GIT_TAG 8c532c32c3c96e5ba1f2283e032f69ead8add00f
  DOWNLOAD_ONLY YES
)
klogg_require_pinned_source(
  mimalloc
  ${mimalloc_SOURCE_DIR}
  8c532c32c3c96e5ba1f2283e032f69ead8add00f
  f08b3cb3f1dfb1e37558e9f9461913bc69b2062998a42f5365c95fd1e3807db1
)
cpmaddpackage(
  NAME sentry
  GITHUB_REPOSITORY getsentry/sentry-native
  GIT_TAG a3d58622a807b9dda174cb9fc18fa0f98c89d043
  DOWNLOAD_ONLY YES
)
