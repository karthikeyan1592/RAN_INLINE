#!/bin/bash
# vm_install_ndctl80.sh — build + install ndctl v80 inside the QEMU VM
#
# Ubuntu 22.04 ships ndctl 72.1 which lacks `cxl create-region`.
# This script builds v80 from source and installs to /usr/local.
#
# Usage (from GCP host):
#   ssh -i ~/.ssh/vm_key -p 2222 -o StrictHostKeyChecking=no root@localhost \
#     "bash /root/cxl/ops/gcp-cxl-lab/scripts/vm_install_ndctl80.sh"
#
# Idempotent: skips build if /usr/local/bin/cxl already reports version 80.
set -euo pipefail

if /usr/local/bin/cxl version 2>/dev/null | grep -q "^80"; then
  echo "ndctl v80 already installed at /usr/local/bin/cxl — skipping build"
  exit 0
fi

echo "=== vm_install_ndctl80.sh ==="
export DEBIAN_FRONTEND=noninteractive

# Build deps
apt-get install -y -qq \
  meson ninja-build pkg-config libudev-dev libjson-c-dev libkmod-dev \
  libsystemd-dev uuid-dev libtraceevent-dev libtracefs-dev libkeyutils-dev \
  2>&1 | tail -3

# Fix iniparser headers for /usr/local prefix
mkdir -p /usr/local/include/iniparser
cp /usr/include/iniparser/*.h /usr/local/include/iniparser/ 2>/dev/null || \
  { apt-get install -y -qq libiniparser-dev && cp /usr/include/iniparser/*.h /usr/local/include/iniparser/; }

# Clone
cd /tmp
if [ ! -d ndctl ]; then
  git clone --depth=1 --branch v80 https://github.com/pmem/ndctl.git 2>&1 | tail -2
else
  echo "ndctl source already at /tmp/ndctl"
fi

cd ndctl
rm -rf build
meson setup build --prefix=/usr/local --libdir=/usr/local/lib -Ddocs=disabled 2>&1 | tail -3
ninja -C build -j4 2>&1 | tail -3

# Install binaries + libraries with explicit symlinks
cp build/cxl/cxl                        /usr/local/bin/cxl
cp build/ndctl/ndctl                    /usr/local/bin/ndctl
cp build/daxctl/daxctl                  /usr/local/bin/daxctl
cp build/cxl/lib/libcxl.so.1.0.7        /usr/local/lib/
cp build/daxctl/lib/libdaxctl.so.1.0.6  /usr/local/lib/
cp build/ndctl/lib/libndctl.so.6.4.21   /usr/local/lib/
rm -f /usr/local/lib/libcxl.so.1 /usr/local/lib/libdaxctl.so.1 /usr/local/lib/libndctl.so.6
ln -s /usr/local/lib/libcxl.so.1.0.7      /usr/local/lib/libcxl.so.1
ln -s /usr/local/lib/libdaxctl.so.1.0.6   /usr/local/lib/libdaxctl.so.1
ln -s /usr/local/lib/libndctl.so.6.4.21   /usr/local/lib/libndctl.so.6
ldconfig

echo "ndctl version: $(/usr/local/bin/cxl version)"
echo "=== vm_install_ndctl80.sh complete ==="
