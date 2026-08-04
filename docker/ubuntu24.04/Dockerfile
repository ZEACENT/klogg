FROM ubuntu:noble

ENV DEBIAN_FRONTEND=noninteractive

# Retry apt operations: CI docker builds hit archive.ubuntu.com network
# flakes; retrying the whole apt invocation makes the base-image build
# resilient instead of flaky.
RUN set -eux; \
    apt_retry() { for i in 1 2 3 4 5; do "$@" && return 0; sleep $((i * 10)); done; return 1; }; \
    apt_retry apt-get update -y && \
    apt_retry apt-get install --no-install-recommends build-essential python3 python-is-python3 \
                    qt6-base-dev qt6-5compat-dev qt6-svg-dev qt6-translations-l10n \
                    qt6-tools-dev-tools qt6-base-dev-tools qt6-tools-dev \
                    libxkbcommon-dev libxkbcommon-x11-dev \
                    libboost-dev libicu-dev libssl-dev libcurl4-openssl-dev \
                    ragel ninja-build zlib1g-dev git pkg-config cppcheck \
                    wget ca-certificates fuse file -y && \
    apt-get clean && rm -rf /var/lib/apt/lists/*

COPY cmake-3.20.2-linux-x86_64.sh /tmp/cmake-installer.sh
RUN chmod 755 /tmp/cmake-installer.sh && \
    /tmp/cmake-installer.sh --prefix=/opt/ --exclude-subdir --skip-license && \
    rm /tmp/cmake-installer.sh

ENV PATH=/opt/bin:$PATH
