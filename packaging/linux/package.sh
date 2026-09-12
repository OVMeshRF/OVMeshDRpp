#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
set -eu
umask 022
arch=$(dpkg --print-architecture)
case "$arch" in amd64|arm64) ;; *) exit 1 ;; esac
version=0.4.2-1
stage=$(mktemp -d "$PWD/build/stage.XXXXXX")
meta=$(mktemp -d "$PWD/build/deb-metadata.XXXXXX")
chmod 755 "$stage"
mkdir -p "$stage/DEBIAN" "$stage/usr/lib/ovmeshdrpp" \
    "$stage/usr/share/applications" "$stage/usr/share/doc/ovmeshdrpp/source-manifests" \
    "$meta/debian" build/output build/evidence
DESTDIR="$stage" cmake --install build/native --strip
cp -P build/deps/usb-1.0.30-local/lib/libusb-1.0.so* "$stage/usr/lib/ovmeshdrpp/"
strip --strip-unneeded "$stage"/usr/lib/ovmeshdrpp/libusb-1.0.so.*.*.*
install -m 644 /packaging/org.ovmesh.OVMeshDRpp.desktop "$stage/usr/share/applications/"
desktop-file-validate "$stage/usr/share/applications/org.ovmesh.OVMeshDRpp.desktop"
install -m 644 /packaging/README.md "$stage/usr/share/doc/ovmeshdrpp/README.md"
install -m 644 LICENSE "$stage/usr/share/doc/ovmeshdrpp/copyright"
cp third_party/*source.json third_party/ui-sources.json "$stage/usr/share/doc/ovmeshdrpp/source-manifests/"
cat > "$meta/debian/control" <<EOF
Source: ovmeshdrpp
Section: science
Priority: optional
Maintainer: OVMeshRF <245213998+OVMeshRF@users.noreply.github.com>
Standards-Version: 4.6.2

Package: ovmeshdrpp
Architecture: any
Description: Receive-only RF survey
EOF
printf 'libusb-1.0 0 ovmeshdrpp (= %s)\n' "$version" > "$meta/shlibs.local"
set --
for binary in "$stage"/usr/bin/* "$stage"/usr/lib/ovmeshdrpp/libusb-1.0.so.*.*.*; do
    file "$binary"
    readelf -d "$binary"
    readelf --version-info "$binary"
    set -- "$@" "-e$binary"
done > build/evidence/elf-details.txt
depends=$(cd "$meta" && dpkg-shlibdeps -O -xovmeshdrpp -Lshlibs.local \
    -l"$stage/usr/lib/ovmeshdrpp" "$@")
depends=${depends#shlibs:Depends=}
size=$(du -sk "$stage/usr" | cut -f1)
cat > "$stage/DEBIAN/control" <<EOF
Package: ovmeshdrpp
Version: $version
Architecture: $arch
Maintainer: OVMeshRF <245213998+OVMeshRF@users.noreply.github.com>
Section: science
Priority: optional
Depends: $depends, libgl1, libx11-6, libxrandr2, libxinerama1, libxcursor1, libxi6, xdg-utils
Installed-Size: $size
Homepage: https://github.com/OVMeshRF/OVMeshDRpp
Description: Experimental receive-only RF spectrum survey
 Native desktop and CLI with HackRF and RTL-SDR receiver adapters and a
 separate experimental RAK5146 worker where qualified at build time.
 Hardware and native desktop acceptance are not established by container tests.
EOF
cp "$stage/DEBIAN/control" build/evidence/package-control.txt
python3 /packaging/inventory.py "$stage/usr/share/doc/ovmeshdrpp/build-inventory.json"
python3 /packaging/inspect-stage.py "$stage"
(cd "$stage" && find usr -type f -print0 | sort -z | xargs -0 md5sum > DEBIAN/md5sums)
dpkg-deb --root-owner-group --build "$stage" "build/output/ovmeshdrpp_${version}_${arch}.deb"
dpkg-deb --contents "build/output/ovmeshdrpp_${version}_${arch}.deb" > build/evidence/package-files.txt
sha256sum "build/output/ovmeshdrpp_${version}_${arch}.deb" > build/output/SHA256SUMS
printf '%s\n' "$stage" > build/evidence/stage-path.txt
