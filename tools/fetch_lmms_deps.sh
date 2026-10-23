#!/bin/sh
# Fetches headers for the native LMMS plugin into third_party/.
# The LMMS headers must match the LMMS build the plugin is loaded into:
# 1.3.0-alpha.1.102+g89fc6c9 (the nightly AppImage).
set -e
cd "$(dirname "$0")/.."
LMMS_COMMIT=89fc6c960b548018a4848346f617d5b4f79b82fa
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

git clone -q --filter=blob:none --no-checkout https://github.com/LMMS/lmms.git "$TMP/lmms"
git -C "$TMP/lmms" sparse-checkout set --no-cone /include/ /LICENSE.txt
git -C "$TMP/lmms" checkout -q "$LMMS_COMMIT"
rm -rf third_party/lmms
mkdir -p third_party/lmms
cp -r "$TMP/lmms/include" "$TMP/lmms/LICENSE.txt" third_party/lmms/

# Qt 5 and libsamplerate headers from Debian, extracted without installing (no root needed)
(cd "$TMP" && apt-get download qtbase5-dev libsamplerate0-dev >/dev/null &&
    dpkg -x qtbase5-dev_*.deb qt && dpkg -x libsamplerate0-dev_*.deb src)
cp "$TMP/src/usr/include/samplerate.h" third_party/lmms/include/
rm -rf third_party/qt5
mkdir -p third_party/qt5/include
for m in QtCore QtGui QtWidgets QtXml; do
    cp -r "$TMP/qt/usr/include/x86_64-linux-gnu/qt5/$m" third_party/qt5/include/
done
echo "LMMS, Qt and libsamplerate headers fetched into third_party/"
