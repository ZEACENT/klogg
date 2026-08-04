FROM ubuntu:jammy

ENV DEBIAN_FRONTEND=noninteractive

# Retry apt operations: CI docker builds hit archive.ubuntu.com network
# flakes; retrying the whole apt invocation makes the base-image build
# resilient instead of flaky.
RUN set -eux; \
    apt_retry() { for i in 1 2 3 4 5; do "$@" && return 0; sleep $((i * 10)); done; return 1; }; \
    apt_retry apt update -y && \
    apt_retry apt install --no-install-recommends build-essential python3 python-is-python3 \
                    qtbase5-dev qt5-qmake qtbase5-dev-tools qttools5-dev qttranslations5-l10n \
                    libqt5svg5-dev libxkbcommon-dev libxkbcommon-x11-dev \
                    libboost-dev libssl-dev libcurl4-openssl-dev \
                    ragel ninja-build zlib1g-dev git pkg-config cppcheck clang llvm-14 \
                    wget ca-certificates fuse file -y && \
    apt clean && rm -rf /var/lib/apt/lists/*

COPY cmake-3.20.2-linux-x86_64.sh /tmp/cmake-installer.sh
RUN chmod 755 /tmp/cmake-installer.sh && \
    /tmp/cmake-installer.sh --prefix=/opt/ --exclude-subdir --skip-license && \
    rm /tmp/cmake-installer.sh

ENV PATH=/opt/bin:$PATH
