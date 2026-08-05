FROM ubuntu:jammy

# CCACHE_DIR lives under the workspace mount (/usr/local) so the cache can be
# persisted across CI runs via actions/cache; keep it bounded and compressed.
ENV DEBIAN_FRONTEND=noninteractive \
    CCACHE_DIR=/usr/local/.ccache \
    CCACHE_MAXSIZE=2G \
    CCACHE_COMPRESS=1

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
                qtbase5-dev qt5-qmake qtbase5-dev-tools qttools5-dev qttranslations5-l10n \
                libqt5svg5-dev libxkbcommon-dev libxkbcommon-x11-dev \
                libboost-dev libssl-dev libcurl4-openssl-dev \
                ragel ninja-build zlib1g-dev git pkg-config cppcheck clang llvm-14 \
                wget ca-certificates fuse file && \
    apt-get clean && rm -rf /var/lib/apt/lists/*

COPY cmake-3.20.2-linux-x86_64.sh /tmp/cmake-installer.sh
RUN chmod 755 /tmp/cmake-installer.sh && \
    /tmp/cmake-installer.sh --prefix=/opt/ --exclude-subdir --skip-license && \
    rm /tmp/cmake-installer.sh

ENV PATH=/opt/bin:$PATH
