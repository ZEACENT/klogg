#!/bin/bash
set -euo pipefail

DESTDIR=$(readlink -f appdir) cmake --install . --component klogg-runtime

test -x appdir/usr/bin/helpers/adb || {
  echo "ERROR: source-built ADB helper missing from appdir/usr/bin/helpers/adb"
  exit 1
}
python3 ../scripts/smoke_adb_helper.py \
  --adb appdir/usr/bin/helpers/adb \
  --port 0 --timeout-seconds 15 \
  --json-output adb-helper-appimage-smoke.json
python3 ../scripts/verify_adb_helper_artifact.py \
  --lock ../packaging/adb/adb-helper.lock.json \
  --receipt adb-helper-staged/receipt.json \
  --binary-smoke-receipt adb-helper-appimage-smoke.json \
  --package-root appdir \
  --asset-scope package \
  --source-assets-root appdir/usr/share/doc/klogg/adb-helper \
  --layout appimage \
  --expected-target linux-x86_64 \
  --maximum-glibc-version 2.31 \
  --checksum-envelope adb-helper-staged/SHA256SUMS \
  --require-lock-binding

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
package="./packages/klogg-${KLOGG_VERSION}-appimage-${KLOGG_PACKAGE_TAG}.AppImage"
cp "./klogg-${KLOGG_VERSION}-x86_64.AppImage" "$package"

python3 ../scripts/smoke_adb_helper.py \
  --adb appdir/usr/bin/helpers/adb \
  --port 0 --timeout-seconds 15 \
  --json-output adb-helper-appimage-final-smoke.json
python3 ../scripts/verify_adb_helper_artifact.py \
  --lock ../packaging/adb/adb-helper.lock.json \
  --receipt adb-helper-staged/receipt.json \
  --binary-smoke-receipt adb-helper-appimage-final-smoke.json \
  --package-root appdir \
  --asset-scope package \
  --source-assets-root appdir/usr/share/doc/klogg/adb-helper \
  --layout appimage \
  --expected-target linux-x86_64 \
  --maximum-glibc-version 2.31 \
  --package-target linux-appimage-x86_64 \
  --package-file "$package" \
  --package-verification-receipt packages/adb-helper-appimage-package-verification.json \
  --checksum-envelope adb-helper-staged/SHA256SUMS \
  --require-lock-binding
