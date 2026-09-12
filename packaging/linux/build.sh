#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
set -eu
umask 022
mkdir -p build/evidence
cp /builder-packages.txt /runtime-packages.txt build/evidence/
uname -m > build/evidence/architecture.txt
cmake --version > build/evidence/cmake.txt
c++ --version > build/evidence/compiler.txt
getconf GNU_LIBC_VERSION > build/evidence/glibc.txt
crypto_prefix="$PWD/build/deps/openssl-3.5.8-relocatable"
if [ ! -d "$crypto_prefix" ]; then
    python3 tools/bootstrap_openssl.py --archive /inputs/openssl-3.5.8.tar.gz --jobs 4 \
        --relocatable --prefix "$crypto_prefix"
fi
if [ ! -d build/deps/usb-1.0.30-local ]; then
    python3 tools/bootstrap_usb.py --libusb-archive /inputs/libusb-1.0.30.tar.bz2 \
        --hackrf-archive /inputs/hackrf-2024.02.1.tar.xz --jobs 4
fi
cmake --fresh -S . -B build/native -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DOPENSSL_ROOT_DIR="$crypto_prefix" \
    -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_INSTALL_RPATH='$ORIGIN/../lib/ovmeshdrpp' \
    -DCMAKE_INSTALL_RPATH_USE_LINK_PATH=OFF \
    -DOVMESH_ENABLE_HACKRF=ON -DOVMESH_ENABLE_RTLSDR=ON -DOVMESH_ENABLE_RAK5146=ON \
    -DOVMESH_BUILD_IQ_LAB=OFF -DOVMESH_ENGINE_THROUGHPUT_TESTS=OFF
cmake --build build/native --parallel 4
sh /packaging/test-build.sh "${1:-full}"
build/native/ovmesh-cli --help > build/evidence/cli-help.txt
LIBGL_ALWAYS_SOFTWARE=1 xvfb-run -a build/native/OVMeshDRpp --ui-smoke 10 --demo
