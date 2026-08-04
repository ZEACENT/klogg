FROM ubuntu:focal

ENV DEBIAN_FRONTEND=noninteractive

# Retry apt operations: CI docker builds hit archive.ubuntu.com/PPA network
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
    apt_install software-properties-common && \
    apt_retry add-apt-repository ppa:beineri/opt-qt-5.15.3-focal -y && \
    apt_install build-essential python3 python-is-python3 \
                qt515base qt515svg qt515tools qt515imageformats qt515translations \
                libxkbcommon-dev libxkbcommon-x11-dev \
                mesa-common-dev libglu1-mesa-dev \
                libboost-dev libicu-dev libssl-dev libcurl4-openssl-dev \
                ragel ninja-build zlib1g-dev git pkg-config \
                wget ca-certificates fuse file && \
    apt-get clean && rm -rf /var/lib/apt/lists/*

COPY cmake-3.20.2-linux-x86_64.sh /tmp/cmake-installer.sh
RUN chmod 755 /tmp/cmake-installer.sh && \
    /tmp/cmake-installer.sh --prefix=/opt/ --exclude-subdir --skip-license && \
    rm /tmp/cmake-installer.sh

ENV PATH=/opt/bin:/opt/qt515/bin:$PATH
