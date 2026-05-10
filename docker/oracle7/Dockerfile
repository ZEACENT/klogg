FROM oraclelinux:7

COPY epel.repo /etc/yum.repos.d

RUN yum update -y && \
    yum install -y ragel make wget rpm-build git \
    qt5-qtbase-devel qt5-qtsvg-devel \
    libxkbcommon-devel libxkbcommon-x11-devel \
    openssl-devel boost-devel zlib-devel \
    devtoolset-7-gcc devtoolset-7-gcc-c++ ninja-build && \
    yum clean all

COPY cmake-3.20.2-linux-x86_64.sh /tmp/cmake-installer.sh
RUN chmod 755 /tmp/cmake-installer.sh && \
    /tmp/cmake-installer.sh --prefix=/opt/ --exclude-subdir --skip-license && \
    rm /tmp/cmake-installer.sh

ENV PATH=/opt/rh/devtoolset-7/root/usr/bin:/opt/bin:$PATH
