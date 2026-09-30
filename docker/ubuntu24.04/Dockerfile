ARG UBUNTU_IMAGE=ubuntu:noble
ARG KLOGG_APT_STAGE=online

# Explicit online source builds remain available locally. Qualified producers
# fix KLOGG_APT_STAGE=locked in the catalog and never fall back to this stage.
FROM ${UBUNTU_IMAGE} AS online-apt
ENV DEBIAN_FRONTEND=noninteractive

# Retry apt operations: CI docker builds hit archive.ubuntu.com network
# flakes; retrying the whole apt invocation makes the base-image build
# resilient instead of flaky.
RUN set -eux; \
    apt_retry() { for i in 1 2 3 4 5; do "$@" && return 0; sleep $((i * 15)); done; return 1; }; \
    apt_install() { for i in 1 2 3 4 5; do \
      apt-get -o Acquire::Retries=5 -o Acquire::http::Timeout=30 update -y \
        && apt-get -o Acquire::Retries=5 -o Acquire::http::Timeout=30 install --no-install-recommends "$@" -y \
        && return 0; \
      sleep $((i * 15)); \
    done; return 1; }; \
    apt_install build-essential python3 python-is-python3 ccache \
                qt6-base-dev qt6-5compat-dev qt6-svg-dev qt6-translations-l10n \
                qt6-tools-dev-tools qt6-base-dev-tools qt6-tools-dev \
                libxkbcommon-dev libxkbcommon-x11-dev \
                libboost-dev libicu-dev libssl-dev libcurl4-openssl-dev \
                ragel ninja-build zlib1g-dev git pkg-config cppcheck \
                wget ca-certificates fuse file && \
    apt-get clean && rm -rf /var/lib/apt/lists/*

FROM ${UBUNTU_IMAGE} AS locked-apt
ARG APT_RUNTIME_LOCK_SHA256
COPY ci_install_locked_apt.sh /opt/klogg-ci/ci_install_locked_apt.sh
COPY inputs/apt/ /opt/klogg-ci-inputs/apt/
RUN --network=none set -eu; \
    sh /opt/klogg-ci/ci_install_locked_apt.sh /opt/klogg-ci-inputs/apt \
      "${APT_RUNTIME_LOCK_SHA256:?a verified APT lock is required}"; \
    mkdir -p /usr/share/klogg-ci; \
    cp /opt/klogg-ci-inputs/apt/manifest.json /usr/share/klogg-ci/apt-inputs.json; \
    rm -rf /opt/klogg-ci-inputs/apt

FROM ${KLOGG_APT_STAGE}-apt AS build-environment
# The workspace mount carries the optional, bounded compiler cache.
ENV DEBIAN_FRONTEND=noninteractive \
    CCACHE_DIR=/usr/local/.ccache \
    CCACHE_MAXSIZE=2G \
    CCACHE_COMPRESS=1

COPY cmake-3.20.2-linux-x86_64.sh /tmp/cmake-installer.sh
RUN printf '%s  %s\n' \
      ea497b4658816010e5850a3ed53845e430654640aabbe10d93fe67def9503e4d \
      /tmp/cmake-installer.sh | sha256sum --check --strict && \
    chmod 755 /tmp/cmake-installer.sh && \
    /tmp/cmake-installer.sh --prefix=/opt/ --exclude-subdir --skip-license && \
    rm /tmp/cmake-installer.sh

ENV PATH=/opt/bin:$PATH
