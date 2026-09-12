#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Disposable, non-privileged container only. No devices or network required.
set -eu
arch=$(dpkg --print-architecture)
dpkg -i "/packages/ovmeshdrpp_0.4.1-1_${arch}.deb"
command -v xdg-open
dpkg-query -W ovmeshdrpp
dpkg --verify ovmeshdrpp
for binary in /usr/bin/OVMeshDRpp /usr/bin/ovmesh-cli /usr/bin/ovmesh-rak-worker \
    /usr/lib/ovmeshdrpp/libusb-1.0.so.0; do
    closure=$(ldd "$binary")
    printf '%s\n%s\n' "$binary" "$closure"
    if printf '%s\n' "$closure" | grep -q 'not found'; then exit 1; fi
done
mkdir -m 777 /tmp/ovmesh-test-home /tmp/ovmesh-replacement
cp -L /usr/lib/ovmeshdrpp/libusb-1.0.so.0 /tmp/ovmesh-replacement/
runuser -u nobody -- env HOME=/tmp/ovmesh-test-home ovmesh-cli --help
runuser -u nobody -- env HOME=/tmp/ovmesh-test-home ovmesh-cli --headless-demo --seconds 1
runuser -u nobody -- env HOME=/tmp/ovmesh-test-home \
    LD_LIBRARY_PATH=/tmp/ovmesh-replacement LD_DEBUG=libs \
    ovmesh-cli --help > /tmp/replacement.stdout 2> /tmp/replacement.stderr
grep 'calling init: /tmp/ovmesh-replacement/libusb-1.0.so.0' /tmp/replacement.stderr
if [ "${1:-}" = gui ]; then
    runuser -u nobody -- env HOME=/tmp/ovmesh-test-home LIBGL_ALWAYS_SOFTWARE=1 \
        xvfb-run -a glxinfo -B
    runuser -u nobody -- env HOME=/tmp/ovmesh-test-home LIBGL_ALWAYS_SOFTWARE=1 \
        xvfb-run -a OVMeshDRpp --ui-smoke 10 --demo
fi
dpkg -r ovmeshdrpp
test ! -e /usr/bin/OVMeshDRpp
test ! -e /usr/bin/ovmesh-cli
test ! -e /usr/bin/ovmesh-rak-worker
test ! -e /usr/lib/ovmeshdrpp/libusb-1.0.so.0
test ! -e /usr/share/applications/org.ovmesh.OVMeshDRpp.desktop
printf 'PASS: package install, dependency closure, non-root synthetic launch, libusb override and removal\n'
