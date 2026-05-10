FROM oraclelinux:8

COPY epel.repo /etc/yum.repos.d

RUN yum update -y && \
    yum install -y --enablerepo=ol8_codeready_builder ragel make wget python38 rpm-build git cppcheck \
    qt5-qtbase-devel qt5-linguist qt5-qttranslations qt5-qtsvg-devel \
    libxkbcommon-devel libxkbcommon-x11-devel \
    openssl-devel boost-devel zlib-devel libcurl-devel \
    gcc gcc-c++ elfutils-devel && \
    yum clean all

COPY cmake-3.20.2-linux-x86_64.sh /tmp/cmake-installer.sh
RUN chmod 755 /tmp/cmake-installer.sh && \
    /tmp/cmake-installer.sh --prefix=/opt/ --exclude-subdir --skip-license && \
    rm /tmp/cmake-installer.sh

RUN wget https://github.com/ninja-build/ninja/releases/download/v1.10.2/ninja-linux.zip && \
    unzip ninja-linux.zip && chmod 755 ninja && mv ninja /opt/bin && rm ninja-linux.zip

ENV PATH=/opt/bin:$PATH
