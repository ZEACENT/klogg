#!/bin/bash
set -euo pipefail

DESTDIR=$(readlink -f appdir) ninja install

LINUXDEPLOYQT_SRC="/usr/local/tools/linuxdeployqt-continuous-x86_64.AppImage"
if [ ! -f "$LINUXDEPLOYQT_SRC" ]; then
  echo "ERROR: linuxdeployqt not found at $LINUXDEPLOYQT_SRC"
  exit 1
fi

cp "$LINUXDEPLOYQT_SRC" ./linuxdeployqt-continuous-x86_64.AppImage
chmod a+x linuxdeployqt-continuous-x86_64.AppImage

VERSION=$KLOGG_VERSION ./linuxdeployqt-continuous-x86_64.AppImage appdir/usr/share/applications/*.desktop -bundle-non-qt-libs

mkdir -p appdir/usr/lib
cp /lib/x86_64-linux-gnu/libssl* appdir/usr/lib

VERSION=$KLOGG_VERSION ./linuxdeployqt-continuous-x86_64.AppImage appdir/usr/share/applications/*.desktop -appimage

mkdir ./packages
cp "./klogg-${KLOGG_VERSION}-x86_64.AppImage" "./packages/klogg-${KLOGG_VERSION}-appimage-${KLOGG_PACKAGE_TAG}.AppImage"
