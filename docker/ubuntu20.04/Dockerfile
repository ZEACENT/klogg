FROM ubuntu:focal

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update -y && \
    apt-get install --no-install-recommends software-properties-common \
                    build-essential python3 python-is-python3 \
                    gnupg dirmngr \
                    wget ca-certificates \
                    -y && \
    apt-key adv --keyserver keyserver.ubuntu.com --recv-keys 1E9377A2BA9EF27F && \
    echo "deb http://ppa.launchpad.net/ubuntu-toolchain-r/test/ubuntu focal main" \
        > /etc/apt/sources.list.d/ubuntu-toolchain-r-test.list && \
    apt-get update -y && \
    apt-get install --no-install-recommends \
                    qtbase5-dev qt5-qmake qtbase5-dev-tools qttools5-dev qttranslations5-l10n \
                    libqt5svg5-dev libxkbcommon-dev libxkbcommon-x11-dev \
                    libboost-dev libssl-dev libicu-dev libcurl4-openssl-dev \
                    ragel ninja-build zlib1g-dev git pkg-config cppcheck \
                    gcc-13 g++-13 \
                    fuse file -y && \
    apt-get clean && rm -rf /var/lib/apt/lists/*

COPY cmake-3.20.2-linux-x86_64.sh /tmp/cmake-installer.sh
RUN chmod 755 /tmp/cmake-installer.sh && \
    /tmp/cmake-installer.sh --prefix=/opt/ --exclude-subdir --skip-license && \
    rm /tmp/cmake-installer.sh

ENV PATH=/opt/bin:$PATH
ENV CC=/usr/bin/gcc-13
ENV CXX=/usr/bin/g++-13
