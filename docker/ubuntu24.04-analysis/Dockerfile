# The producer must override UBUNTU_IMAGE with the resolved linux/amd64 digest.
# Inputs are materialized and verified before this network-free image build.
ARG UBUNTU_IMAGE=ubuntu:noble
FROM ${UBUNTU_IMAGE}

ARG APT_RUNTIME_LOCK_SHA256
ARG TOOLS_ARCHIVE_SHA256

ENV DEBIAN_FRONTEND=noninteractive \
    LANG=C.UTF-8 \
    LC_ALL=C.UTF-8

COPY ci_install_locked_apt.sh /opt/klogg-ci/ci_install_locked_apt.sh
COPY inputs/apt/ /opt/klogg-ci-inputs/apt/
RUN --network=none set -eu; \
    sh /opt/klogg-ci/ci_install_locked_apt.sh /opt/klogg-ci-inputs/apt \
      "${APT_RUNTIME_LOCK_SHA256:?a verified APT lock is required}"; \
    mkdir -p /usr/share/klogg-ci; \
    cp /opt/klogg-ci-inputs/apt/manifest.json /usr/share/klogg-ci/apt-inputs.json; \
    rm -rf /opt/klogg-ci-inputs/apt

# The tools archive contains only verified, redistributable CMake, Ninja, Qt and
# Boost material. CodeQL stays in its official distribution and is fetched only
# by its qualification/analysis role, never included in this published image.
COPY extract_verified_tar.py prefetch_adb_helper_sources.py /opt/klogg-ci/
COPY inputs/tools.tar /tmp/klogg-tools.tar
RUN --network=none set -eu; \
    printf '%s  %s\n' "${TOOLS_ARCHIVE_SHA256:?a verified tool archive is required}" \
      /tmp/klogg-tools.tar | sha256sum --check --strict; \
    python3 /opt/klogg-ci/extract_verified_tar.py \
      --archive /tmp/klogg-tools.tar --destination /opt/klogg-tools; \
    rm /tmp/klogg-tools.tar

ENV PATH=/opt/klogg-tools/cmake/bin:/opt/klogg-tools/bin:/opt/klogg-tools/qt/6.9.3/gcc_64/bin:$PATH \
    CC=gcc-13 \
    CXX=g++-13 \
    BOOST_ROOT=/opt/klogg-tools/boost \
    QT_ROOT_DIR=/opt/klogg-tools/qt/6.9.3/gcc_64 \
    CMAKE_PREFIX_PATH=/opt/klogg-tools/qt/6.9.3/gcc_64 \
    LD_LIBRARY_PATH=/opt/klogg-tools/qt/6.9.3/gcc_64/lib \
    QT_PLUGIN_PATH=/opt/klogg-tools/qt/6.9.3/gcc_64/plugins

RUN --network=none set -eu; \
    cmake --version | grep -F -x 'cmake version 3.31.6'; \
    test "$(ninja --version)" = 1.12.1; \
    test "$(qmake -query QT_VERSION)" = 6.9.3; \
    test "$(qmake -query QT_INSTALL_PREFIX)" = "$QT_ROOT_DIR"; \
    test -f "$BOOST_ROOT/boost/version.hpp"; \
    gcc-13 --version; \
    g++-13 --version; \
    clang-tidy --version; \
    cppcheck --version; \
    gcovr --version
