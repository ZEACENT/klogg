# Update the base reference, manifest digest, and APT snapshot together after
# rebuilding the image and running the complete Linux TSan suite.
ARG UBUNTU_IMAGE=public.ecr.aws/ubuntu/ubuntu:22.04@sha256:0199853f6d6b20b0424f3c5694a72a62764f01e6a771b1eb48a4197848986c7e
ARG KLOGG_APT_STAGE=online

FROM ${UBUNTU_IMAGE} AS tsan-base
ENV DEBIAN_FRONTEND=noninteractive

# Local and old-CI source builds remain explicit online consumers. Qualified
# producers select locked; missing bundles or locks never fall back online.
FROM tsan-base AS online-bootstrap
ARG UBUNTU_SNAPSHOT=20260731T000000Z

COPY --chmod=0755 apt_snapshot_retry.sh /usr/local/bin/apt_snapshot_retry.sh

# The minimal base has no CA bundle. Bootstrap only ca-certificates from the
# snapshot's signed APT metadata. Limit the TLS exception to the snapshot host,
# clean it up even on failure, then refresh indexes with verification enabled.
RUN set -eux; \
    printf '%s\n' \
      "deb https://snapshot.ubuntu.com/ubuntu/${UBUNTU_SNAPSHOT}/ jammy main universe" \
      "deb https://snapshot.ubuntu.com/ubuntu/${UBUNTU_SNAPSHOT}/ jammy-updates main universe" \
      "deb https://snapshot.ubuntu.com/ubuntu/${UBUNTU_SNAPSHOT}/ jammy-security main universe" \
      > /etc/apt/sources.list; \
    printf '%s\n' 'Acquire::Check-Valid-Until "false";' \
      > /etc/apt/apt.conf.d/98snapshot; \
    trap 'rm -f /etc/apt/apt.conf.d/99snapshot-bootstrap' EXIT; \
    printf '%s\n' 'Acquire::https::snapshot.ubuntu.com::Verify-Peer "false";' \
      > /etc/apt/apt.conf.d/99snapshot-bootstrap; \
    /usr/local/bin/apt_snapshot_retry.sh ca-certificates; \
    rm /etc/apt/apt.conf.d/99snapshot-bootstrap; \
    test ! -e /etc/apt/apt.conf.d/99snapshot-bootstrap; \
    trap - EXIT; \
    /usr/local/bin/apt_snapshot_retry.sh; \
    rm -rf /var/lib/apt/lists/*

FROM tsan-base AS locked-bootstrap
ARG APT_BOOTSTRAP_LOCK_SHA256
COPY ci_install_locked_apt.sh /opt/klogg-ci/ci_install_locked_apt.sh
COPY inputs/bootstrap/ /opt/klogg-ci-inputs/bootstrap/
RUN --network=none set -eu; \
    sh /opt/klogg-ci/ci_install_locked_apt.sh /opt/klogg-ci-inputs/bootstrap \
      "${APT_BOOTSTRAP_LOCK_SHA256:?a verified bootstrap APT lock is required}"; \
    mkdir -p /usr/share/klogg-ci; \
    cp /opt/klogg-ci-inputs/bootstrap/manifest.json /usr/share/klogg-ci/bootstrap-inputs.json; \
    rm -rf /opt/klogg-ci-inputs/bootstrap

FROM ${KLOGG_APT_STAGE}-bootstrap AS jammy-snapshot

FROM jammy-snapshot AS qt-source-base

ARG QT_VERSION=5.15.19
ARG QT_ARCHIVE_BASE=https://qt.mirror.constant.com/archive/qt/5.15
ARG QTBASE_SHA256=51e91c73abacab81e64efd01bf95794e79c0e605ad80947a024769f0dd620a32
ARG QTSVG_SHA256=cabc95fc12fa84c92e95f08660742a0ce297e6db3deadbefbe7fbec4769b8c13
ARG QTTOOLS_SHA256=2694af886130d63b8a42bcd138f4035fd8108273ebd16781f87300da352f1aa6
ARG QTTRANSLATIONS_SHA256=967e0d259af13f47e853f183cffc9671cc054380188fee28402f6425132a2c7b

WORKDIR /tmp/qt-src

FROM qt-source-base AS online-qt-source
RUN /usr/local/bin/apt_snapshot_retry.sh ca-certificates curl patch xz-utils && \
    rm -rf /var/lib/apt/lists/*

RUN set -eux; \
    for module in qtbase qtsvg qttools qttranslations; do \
      archive="${module}-everywhere-opensource-src-${QT_VERSION}.tar.xz"; \
      curl --fail --location --retry 5 --retry-all-errors --retry-delay 10 \
        --continue-at - --output "${archive}" \
        "${QT_ARCHIVE_BASE}/${QT_VERSION}/submodules/${archive}"; \
    done

FROM qt-source-base AS locked-qt-source
ARG APT_SOURCE_LOCK_SHA256
COPY inputs/source/ /opt/klogg-ci-inputs/source/
RUN --network=none set -eu; \
    sh /opt/klogg-ci/ci_install_locked_apt.sh /opt/klogg-ci-inputs/source \
      "${APT_SOURCE_LOCK_SHA256:?a verified source APT lock is required}"; \
    mkdir -p /usr/share/klogg-ci; \
    cp /opt/klogg-ci-inputs/source/manifest.json /usr/share/klogg-ci/source-inputs.json; \
    rm -rf /opt/klogg-ci-inputs/source
COPY inputs/qt/qtbase-everywhere-opensource-src-${QT_VERSION}.tar.xz /tmp/qt-src/
COPY inputs/qt/qtsvg-everywhere-opensource-src-${QT_VERSION}.tar.xz /tmp/qt-src/
COPY inputs/qt/qttools-everywhere-opensource-src-${QT_VERSION}.tar.xz /tmp/qt-src/
COPY inputs/qt/qttranslations-everywhere-opensource-src-${QT_VERSION}.tar.xz /tmp/qt-src/

FROM ${KLOGG_APT_STAGE}-qt-source AS qt-source
RUN --network=none set -eux; \
    printf '%s  %s\n' \
      "${QTBASE_SHA256}" "qtbase-everywhere-opensource-src-${QT_VERSION}.tar.xz" \
      "${QTSVG_SHA256}" "qtsvg-everywhere-opensource-src-${QT_VERSION}.tar.xz" \
      "${QTTOOLS_SHA256}" "qttools-everywhere-opensource-src-${QT_VERSION}.tar.xz" \
      "${QTTRANSLATIONS_SHA256}" "qttranslations-everywhere-opensource-src-${QT_VERSION}.tar.xz" \
      | sha256sum --check --strict; \
    for archive in *.tar.xz; do tar -xf "${archive}"; done; \
    mkdir -p /usr/share/klogg-ci/qt-sources/archives; \
    mv -- *.tar.xz /usr/share/klogg-ci/qt-sources/archives/

COPY Dockerfile /usr/share/klogg-ci/qt-sources/recipe/Dockerfile
COPY README.md /usr/share/klogg-ci/qt-sources/README.md
COPY apt_snapshot_retry.sh verify_elf_runtime_closure.sh /usr/share/klogg-ci/qt-sources/recipe/
RUN --network=none set -eu; \
    mkdir -p /usr/share/klogg-ci/qt-sources/apt; \
    for module in qtbase qtsvg qttools qttranslations; do \
      destination="/usr/share/klogg-ci/qt-sources/licenses/${module}"; \
      mkdir -p "${destination}"; \
      found=0; \
      for license in "${module}-everywhere-src-${QT_VERSION}"/LICENSE*; do \
        if [ -f "${license}" ] && [ ! -L "${license}" ]; then \
          cp -- "${license}" "${destination}/"; \
          found=1; \
        fi; \
      done; \
      test "${found}" = 1; \
    done; \
    if [ -f /opt/klogg-ci/ci_install_locked_apt.sh ]; then \
      cp /opt/klogg-ci/ci_install_locked_apt.sh /usr/share/klogg-ci/qt-sources/recipe/; \
    fi; \
    for manifest in bootstrap-inputs.json source-inputs.json; do \
      if [ -f "/usr/share/klogg-ci/${manifest}" ]; then \
        cp "/usr/share/klogg-ci/${manifest}" /usr/share/klogg-ci/qt-sources/apt/; \
      fi; \
    done

COPY patches/fix_qt5_qobject_tsan_publication.patch /tmp/fix_qt5_qobject_tsan_publication.patch
RUN --network=none patch --batch --forward --fuzz=0 \
      --directory="/tmp/qt-src/qtbase-everywhere-src-${QT_VERSION}" \
      --strip=1 < /tmp/fix_qt5_qobject_tsan_publication.patch && \
    mkdir -p /usr/share/klogg-ci/qt-sources/patches && \
    mv /tmp/fix_qt5_qobject_tsan_publication.patch /usr/share/klogg-ci/qt-sources/patches/

FROM jammy-snapshot AS online-qt-deps
RUN /usr/local/bin/apt_snapshot_retry.sh \
      clang-14 g++ llvm-14 make perl pkg-config python3 \
      libfontconfig1-dev libfreetype6-dev libglib2.0-dev libssl-dev \
      libx11-dev libx11-xcb-dev libxcb1-dev libxcb-glx0-dev \
      libxcb-keysyms1-dev libxcb-image0-dev libxcb-shm0-dev \
      libxcb-icccm4-dev libxcb-sync-dev libxcb-xfixes0-dev \
      libxcb-shape0-dev libxcb-randr0-dev libxcb-render-util0-dev \
      libxcb-xinerama0-dev libxcb-xkb-dev libxkbcommon-dev \
      libxkbcommon-x11-dev zlib1g-dev && \
    rm -rf /var/lib/apt/lists/*

FROM jammy-snapshot AS locked-qt-deps
ARG APT_BUILDER_LOCK_SHA256
COPY inputs/qt-builder/ /opt/klogg-ci-inputs/qt-builder/
RUN --network=none set -eu; \
    sh /opt/klogg-ci/ci_install_locked_apt.sh /opt/klogg-ci-inputs/qt-builder \
      "${APT_BUILDER_LOCK_SHA256:?a verified Qt builder APT lock is required}"; \
    mkdir -p /usr/share/klogg-ci; \
    cp /opt/klogg-ci-inputs/qt-builder/manifest.json /usr/share/klogg-ci/qt-builder-inputs.json; \
    rm -rf /opt/klogg-ci-inputs/qt-builder

FROM ${KLOGG_APT_STAGE}-qt-deps AS qtbase-builder
ENV CC=clang-14
ENV CXX=clang++-14
ENV QT_TSAN_PREFIX=/opt/qt5-tsan

ARG QT_VERSION=5.15.19
ARG QT_BUILD_JOBS=2

# Keep builder provenance separate from the source stage to preserve parallel
# dependency branches; the final image merges this small directory into sources.
RUN --network=none set -eu; \
    mkdir -p /usr/share/klogg-ci/qt-builder-provenance; \
    if [ -f /usr/share/klogg-ci/qt-builder-inputs.json ]; then \
      cp /usr/share/klogg-ci/qt-builder-inputs.json /usr/share/klogg-ci/qt-builder-provenance/; \
    fi

RUN --network=none ln -s /usr/bin/clang-14 /usr/local/bin/clang && \
    ln -s /usr/bin/clang++-14 /usr/local/bin/clang++ && \
    printf 'int main() { return 0; }\n' > /tmp/tsan-probe.cpp && \
    clang++-14 -fsanitize=thread /tmp/tsan-probe.cpp -o /tmp/tsan-probe && \
    rm /tmp/tsan-probe.cpp /tmp/tsan-probe

COPY --from=qt-source /tmp/qt-src /tmp/qt-src
WORKDIR /tmp/qt-src

RUN --network=none set -eux; \
    cd "qtbase-everywhere-src-${QT_VERSION}"; \
    ./configure \
      -prefix "${QT_TSAN_PREFIX}" \
      -opensource -confirm-license \
      -release -shared -sanitize thread \
      -platform linux-clang \
      -nomake examples -nomake tests \
      -no-opengl -no-gtk -no-cups \
      -openssl-runtime \
      -fontconfig -system-freetype; \
    make -j"${QT_BUILD_JOBS}"; \
    make install

FROM qtbase-builder AS qt-builder

ENV PATH=/opt/qt5-tsan/bin:$PATH
ENV LD_LIBRARY_PATH=/opt/qt5-tsan/lib

RUN --network=none set -eux; \
    cd "qtsvg-everywhere-src-${QT_VERSION}"; \
    "${QT_TSAN_PREFIX}/bin/qmake" qtsvg.pro; \
    make -j"${QT_BUILD_JOBS}"; \
    make install; \
    cd "../qttools-everywhere-src-${QT_VERSION}"; \
    "${QT_TSAN_PREFIX}/bin/qmake" qttools.pro; \
    cd src/linguist; \
    "${QT_TSAN_PREFIX}/bin/qmake" linguist.pro; \
    for tool in lconvert lrelease lupdate; do \
      ( \
        cd "${tool}"; \
        "${QT_TSAN_PREFIX}/bin/qmake" "${tool}.pro"; \
        make -j"${QT_BUILD_JOBS}"; \
        make install \
      ); \
    done; \
    make install_cmake_linguist_tools_files; \
    cd "/tmp/qt-src/qttranslations-everywhere-src-${QT_VERSION}"; \
    "${QT_TSAN_PREFIX}/bin/qmake" qttranslations.pro; \
    make -j"${QT_BUILD_JOBS}"; \
    make install

RUN --network=none set -eux; \
    test -x "${QT_TSAN_PREFIX}/bin/lconvert"; \
    test -x "${QT_TSAN_PREFIX}/bin/lrelease"; \
    test -x "${QT_TSAN_PREFIX}/bin/lupdate"; \
    test -f "${QT_TSAN_PREFIX}/lib/cmake/Qt5LinguistTools/Qt5LinguistToolsConfig.cmake"; \
    test -f "${QT_TSAN_PREFIX}/plugins/platforms/libqoffscreen.so"; \
    for artifact in \
      "${QT_TSAN_PREFIX}/lib/libQt5Core.so.5" \
      "${QT_TSAN_PREFIX}/lib/libQt5Gui.so.5" \
      "${QT_TSAN_PREFIX}/lib/libQt5Widgets.so.5" \
      "${QT_TSAN_PREFIX}/lib/libQt5Concurrent.so.5" \
      "${QT_TSAN_PREFIX}/lib/libQt5Network.so.5" \
      "${QT_TSAN_PREFIX}/lib/libQt5Xml.so.5" \
      "${QT_TSAN_PREFIX}/lib/libQt5Svg.so.5" \
      "${QT_TSAN_PREFIX}/lib/libQt5Test.so.5" \
      "${QT_TSAN_PREFIX}/plugins/platforms/libqoffscreen.so" \
      "${QT_TSAN_PREFIX}/bin/lconvert" \
      "${QT_TSAN_PREFIX}/bin/lrelease" \
      "${QT_TSAN_PREFIX}/bin/lupdate"; do \
      if ! llvm-nm-14 -D "${artifact}" | grep -Eq '__tsan_(func_entry|read|write)'; then \
        echo "Qt artifact is not compiler-instrumented for TSan: ${artifact}" >&2; \
        exit 1; \
      fi; \
    done; \
    llvm-nm-14 -D --undefined-only "${QT_TSAN_PREFIX}/lib/libQt5Core.so.5" \
      | grep -q '__tsan_acquire'; \
    llvm-nm-14 -D --undefined-only "${QT_TSAN_PREFIX}/lib/libQt5Core.so.5" \
      | grep -q '__tsan_release'

FROM jammy-snapshot AS runtime-base
ENV QT_TSAN_PREFIX=/opt/qt5-tsan
ENV PATH=/opt/qt5-tsan/bin:/opt/bin:$PATH
ENV LD_LIBRARY_PATH=/opt/qt5-tsan/lib
ENV PKG_CONFIG_PATH=/opt/qt5-tsan/lib/pkgconfig

ARG CMAKE_INSTALLER_URL=https://github.com/Kitware/CMake/releases/download/v3.20.2/cmake-3.20.2-linux-x86_64.sh
ARG CMAKE_INSTALLER_SHA256=ea497b4658816010e5850a3ed53845e430654640aabbe10d93fe67def9503e4d

FROM runtime-base AS online-runtime
RUN /usr/local/bin/apt_snapshot_retry.sh \
      build-essential python3 python-is-python3 \
      clang-14 llvm-14 \
      libboost-dev libssl-dev libcurl4-openssl-dev \
      libfontconfig1 libfreetype6 libglib2.0-0 libpcre2-16-0 libx11-6 libx11-xcb1 \
      libxcb1 libxcb-glx0 libxcb-keysyms1 libxcb-image0 libxcb-shm0 \
      libxcb-icccm4 libxcb-sync1 libxcb-xfixes0 libxcb-shape0 \
      libxcb-randr0 libxcb-render-util0 libxcb-xinerama0 libxcb-xkb1 \
      libxkbcommon0 libxkbcommon-x11-0 \
      ragel ninja-build zlib1g-dev git pkg-config cppcheck \
      wget ca-certificates fuse file && \
    rm -rf /var/lib/apt/lists/*

RUN wget --tries=3 --timeout=30 --output-document=/tmp/cmake-installer.sh \
      "${CMAKE_INSTALLER_URL}"

FROM runtime-base AS locked-runtime
ARG APT_RUNTIME_LOCK_SHA256
COPY inputs/runtime/ /opt/klogg-ci-inputs/runtime/
RUN --network=none set -eu; \
    sh /opt/klogg-ci/ci_install_locked_apt.sh /opt/klogg-ci-inputs/runtime \
      "${APT_RUNTIME_LOCK_SHA256:?a verified runtime APT lock is required}"; \
    mkdir -p /usr/share/klogg-ci; \
    cp /opt/klogg-ci-inputs/runtime/manifest.json /usr/share/klogg-ci/runtime-inputs.json; \
    rm -rf /opt/klogg-ci-inputs/runtime
COPY inputs/cmake-3.20.2-linux-x86_64.sh /tmp/cmake-installer.sh

FROM ${KLOGG_APT_STAGE}-runtime AS build-environment
RUN --network=none ln -s /usr/bin/clang-14 /usr/local/bin/clang && \
    ln -s /usr/bin/clang++-14 /usr/local/bin/clang++ && \
    printf 'int main() { return 0; }\n' > /tmp/tsan-probe.cpp && \
    clang++-14 -fsanitize=thread /tmp/tsan-probe.cpp -o /tmp/tsan-probe && \
    rm /tmp/tsan-probe.cpp /tmp/tsan-probe

COPY --from=qt-builder /opt/qt5-tsan /opt/qt5-tsan
COPY --from=qt-source /usr/share/klogg-ci/qt-sources/ /usr/share/klogg-ci/qt-sources/
COPY --from=qtbase-builder /usr/share/klogg-ci/qt-builder-provenance/ /usr/share/klogg-ci/qt-sources/apt/
RUN --network=none set -eu; \
    if [ -f /usr/share/klogg-ci/runtime-inputs.json ]; then \
      cp /usr/share/klogg-ci/runtime-inputs.json /usr/share/klogg-ci/qt-sources/apt/; \
    fi
COPY --chmod=0755 verify_elf_runtime_closure.sh /usr/local/bin/verify_elf_runtime_closure.sh

RUN --network=none printf '%s  %s\n' "${CMAKE_INSTALLER_SHA256}" /tmp/cmake-installer.sh \
      | sha256sum --check --strict && \
    chmod 755 /tmp/cmake-installer.sh && \
    /tmp/cmake-installer.sh --prefix=/opt/ --exclude-subdir --skip-license && \
    rm /tmp/cmake-installer.sh && \
    /usr/local/bin/verify_elf_runtime_closure.sh "${QT_TSAN_PREFIX}" && \
    test -x /usr/bin/llvm-symbolizer-14 && \
    test -f /opt/qt5-tsan/plugins/platforms/libqoffscreen.so && \
    llvm-nm-14 -D --undefined-only /opt/qt5-tsan/lib/libQt5Core.so.5 \
      | grep -q '__tsan_acquire' && \
    llvm-nm-14 -D --undefined-only /opt/qt5-tsan/lib/libQt5Core.so.5 \
      | grep -q '__tsan_release'
