#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Regenerates the CentOS Stream 10 systemd sysroot used by `mkimage.py --base centos10`.
#
#   scripts/fetch-centos10-systemd.sh [DIR]      (default DIR: $NVSD_WORK/centos10, i.e. /home/ecurtin/nvsd-work/centos10)
#
# Uses podman and the quay.io/centos/centos:stream10 image (the container's dnf installs the `systemd` package
# from the CentOS Stream 10 BaseOS mirror, so this needs network access from inside the container). Produces
#   DIR/sysroot/   the files the image needs, as REAL files at their real paths (no symlinks):
#                  usr/lib/systemd/systemd, usr/lib/systemd/systemd-executor, usr/bin/mount, and every shared
#                  library they need (glibc, ld.so, libsystemd-core/-shared, libmount, ...) found with the
#                  container's own ldd, so nothing is ever taken from the Fedora host
#   DIR/VERSION    the systemd package version-release
# Nothing here is committed (see .gitignore: sysroot, VERSION).
set -eu
WORK=${NVSD_WORK:-/home/ecurtin/nvsd-work}
DIR=${1:-$WORK/centos10}
IMAGE=${CENTOS10_IMAGE:-quay.io/centos/centos:stream10}
mkdir -p "$DIR"
DIR=$(cd "$DIR" && pwd)
rm -rf "$DIR/sysroot"
mkdir -p "$DIR/sysroot"
podman run --rm -v "$DIR":/out:Z "$IMAGE" sh -eu -c '
  dnf -y -q --setopt=install_weak_deps=0 install systemd util-linux-core >/dev/null
  rpm -q systemd --qf "%{VERSION}-%{RELEASE}\n" > /out/VERSION
  S=/out/sysroot
  put() {  # put SRC [DESTNAME]: copy the real file to <real dir of SRC>/<DESTNAME or basename SRC>
    real=$(readlink -f "$1"); d=$(dirname "$real"); n=${2:-$(basename "$1")}
    mkdir -p "$S$d"; cp -L "$real" "$S$d/$n"
  }
  libs() {  # every library ldd resolves for $1
    ldd "$1" | sed -n "s/.*=> \(\/[^ ]*\) .*/\1/p; s/^[[:space:]]*\(\/[^ ]*\) (.*/\1/p"
  }
  for b in /usr/lib/systemd/systemd /usr/lib/systemd/systemd-executor /usr/bin/mount; do
    put "$b"
    for l in $(libs "$b"); do put "$l"; done
  done
  # systemd dlopen()s libmount from mount_option_mangle(); take it and its dependencies too
  for l in /usr/lib64/libmount.so.1; do put "$l"; for m in $(libs "$l"); do put "$m"; done; done
  # the interpreter keeps its soname path
  put /lib64/ld-linux-x86-64.so.2
  chmod -R u+rwX /out/sysroot
'
echo "centos10 systemd $(cat "$DIR/VERSION") sysroot in $DIR/sysroot ($(du -sh "$DIR/sysroot" | cut -f1))"
