cmake_minimum_required(VERSION 3.14)

project(klogg_prefetch NONE)

# Keep this list in sync with 3rdparty/CMakeLists.txt. This project only warms
# the shared CPM source cache, so every dependency is fetched as DOWNLOAD_ONLY.
include(${CMAKE_CURRENT_LIST_DIR}/../CPM.cmake)

set(CPM_USE_LOCAL_PACKAGES OFF)

cpmaddpackage(
  NAME simdutf
  GITHUB_REPOSITORY simdutf/simdutf
  VERSION 5.6.2
  DOWNLOAD_ONLY YES
)
cpmaddpackage(
  NAME type_safe
  GITHUB_REPOSITORY foonathan/type_safe
  VERSION 0.2.4
  DOWNLOAD_ONLY YES
)
cpmaddpackage(
  NAME CRoaring
  GITHUB_REPOSITORY RoaringBitmap/CRoaring
  VERSION 4.2.1
  DOWNLOAD_ONLY YES
)
cpmaddpackage(
  NAME streamvbyte
  GITHUB_REPOSITORY lemire/streamvbyte
  VERSION 1.0.0
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
  DOWNLOAD_ONLY YES
)
cpmaddpackage(
  NAME Uchardet
  GIT_REPOSITORY https://gitlab.freedesktop.org/uchardet/uchardet
  VERSION 0.0.8
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
  GIT_TAG 3.11.2
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
  VERSION 0.8.1
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
  GIT_TAG 1.4.1
  DOWNLOAD_ONLY YES
)
cpmaddpackage(
  NAME tbb
  GITHUB_REPOSITORY variar/oneTBB
  GIT_TAG c9be1ac2930f02dea523003ed801b4489f3e6b6e
  DOWNLOAD_ONLY YES
)
cpmaddpackage(
  NAME mimalloc
  GITHUB_REPOSITORY microsoft/mimalloc
  VERSION 2.1.7
  DOWNLOAD_ONLY YES
)
cpmaddpackage(
  NAME sentry
  GITHUB_REPOSITORY getsentry/sentry-native
  GIT_TAG a3d58622a807b9dda174cb9fc18fa0f98c89d043
  DOWNLOAD_ONLY YES
)
