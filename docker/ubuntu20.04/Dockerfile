FROM ubuntu:focal

ENV DEBIAN_FRONTEND=noninteractive

# Retry apt operations as one update+install unit: CI docker builds hit
# archive.ubuntu.com/PPA network flakes (the ubuntu-20.04-appimage leg failed
# when apt-get update returned success with empty package lists, so install
# reported "Unable to locate package"). apt_install re-runs update inside each
# retry so a stale or empty list is refreshed, and Acquire::Retries/timeouts
# make apt itself resilient to per-connection failures.
RUN set -eux; \
    apt_retry() { for i in 1 2 3 4 5; do "$@" && return 0; sleep $((i * 15)); done; return 1; }; \
    apt_install() { for i in 1 2 3 4 5; do \
      apt-get -o Acquire::Retries=5 -o Acquire::http::Timeout=30 update -y \
        && apt-get -o Acquire::Retries=5 -o Acquire::http::Timeout=30 install --no-install-recommends "$@" -y \
        && return 0; \
      sleep $((i * 15)); \
    done; return 1; }; \
    apt_install software-properties-common build-essential python3 python-is-python3 \
                gnupg dirmngr wget ca-certificates && \
    apt_retry apt-key adv --keyserver keyserver.ubuntu.com --recv-keys 1E9377A2BA9EF27F && \
    echo "deb http://ppa.launchpad.net/ubuntu-toolchain-r/test/ubuntu focal main" \
        > /etc/apt/sources.list.d/ubuntu-toolchain-r-test.list && \
    apt_install qtbase5-dev qt5-qmake qtbase5-dev-tools qttools5-dev qttranslations5-l10n \
                libqt5svg5-dev libxkbcommon-dev libxkbcommon-x11-dev \
                libboost-dev libssl-dev libicu-dev libcurl4-openssl-dev \
                ragel ninja-build zlib1g-dev git pkg-config cppcheck \
                gcc-13 g++-13 \
                fuse file && \
    apt-get clean && rm -rf /var/lib/apt/lists/*

COPY cmake-3.20.2-linux-x86_64.sh /tmp/cmake-installer.sh
RUN chmod 755 /tmp/cmake-installer.sh && \
    /tmp/cmake-installer.sh --prefix=/opt/ --exclude-subdir --skip-license && \
    rm /tmp/cmake-installer.sh

ENV PATH=/opt/bin:$PATH
ENV CC=/usr/bin/gcc-13
ENV CXX=/usr/bin/g++-13
