#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""mkimage.py: build initramfs images for the nvsystemd / NVRC comparison.

No root needed: the cpio (newc) archive is written directly, including device
nodes, symlinks and hard links. Nothing produced here is ever committed.

  mkimage.py --variant nvrc    --nvrc-bin PATH       --out X.cpio.zst
  mkimage.py --variant nvsd                          --out X.cpio.zst
  mkimage.py --variant systemd-only                  --out X.cpio.zst

All variants share the same base skeleton and the same test fixture (the nvstub
multi-call binary hard-linked at the paths NVRC execs, plus the fixture config
files and /etc/nvstub.conf), so the fixture is byte-for-byte identical.
"""
import argparse
import os
import re
import stat
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.environ.get("NVSD_OUT", "/home/ecurtin/nvsd-work/out")

STUB_NAMES = {
    "/usr/sbin/modprobe": "modprobe",
    "/usr/bin/nvidia-smi": "nvidia-smi",
    "/usr/bin/nvidia-persistenced": "nvidia-persistenced",
    "/usr/bin/nv-hostengine": "nv-hostengine",
    "/usr/bin/dcgm-exporter": "dcgm-exporter",
    "/usr/bin/nvidia-ctk": "nvidia-ctk",
    "/usr/bin/nv-fabricmanager": "nv-fabricmanager",
    "/usr/sbin/nvlsm": "nvlsm",
    "/usr/bin/kata-agent": "kata-agent",
    "/usr/sbin/ldconfig": "ldconfig",
    "/usr/sbin/veritysetup": "veritysetup",
    "/usr/bin/nvstub": "nvstub",  # test aid (mkext); the multi-call binary under its own name
}

FIXTURE_FILES = {
    "/etc/dcgm-exporter/default-counters.csv": "# SIMULATED collectors file (nvsystemd fixture)\nDCGM_FI_DEV_GPU_UTIL, gauge, GPU utilization\n",
    "/usr/share/nvidia/nvswitch/fabricmanager.cfg": "# SIMULATED fabricmanager.cfg (nvsystemd fixture)\nLOG_LEVEL=4\nDAEMONIZE=1\nFABRIC_MODE=0\nFABRIC_MODE_RESTART=0\nPARTITION_RAIL_POLICY=greedy\n",
    "/usr/share/nvidia/nvlsm/nvlsm.conf": "# SIMULATED nvlsm.conf (nvsystemd fixture)\n",
}

BASE_DIRS = ["/proc", "/sys", "/dev", "/run", "/tmp", "/etc", "/var", "/root", "/usr", "/usr/bin", "/usr/sbin",
             "/usr/lib", "/usr/lib64", "/usr/share", "/etc/dcgm-exporter", "/usr/share/nvidia",
             "/usr/share/nvidia/nvswitch", "/usr/share/nvidia/nvlsm", "/lib/firmware/nvidia"]
BASE_LINKS = {"/bin": "usr/bin", "/sbin": "usr/sbin", "/lib": "usr/lib", "/lib64": "usr/lib64", "/var/run": "../run"}
# /lib/firmware must exist for NVRC's gpu_extension bind target; create under usr/lib and reach via /lib symlink
BASE_DIRS.remove("/lib/firmware/nvidia")
BASE_DIRS += ["/usr/lib/firmware", "/usr/lib/firmware/nvidia"]
DLOPEN_LIBS = ["/lib64/libmount.so.1"]
# systemd's .mount units fork /usr/bin/mount (util-linux), they do not call mount(2) themselves.
EXTRA_BINS = ["/usr/bin/mount"]
DEV_NODES = {"/dev/console": (5, 1), "/dev/null": (1, 3), "/dev/kmsg": (1, 11), "/dev/zero": (1, 5)}


class Cpio:
    def __init__(self):
        self.entries = []  # (path, mode, data, rdev, nlink_group)
        self.ino = 1
        self.seen = set()
        self.hl = {}  # src -> ino

    def _add(self, path, mode, data=b"", rdev=(0, 0), ino=None, nlink=1):
        path = path.lstrip("/")
        if path in self.seen:
            raise SystemExit("duplicate cpio entry: " + path)
        self.seen.add(path)
        if ino is None:
            ino = self.ino
            self.ino += 1
        self.entries.append((path, mode, data, rdev, ino, nlink))

    def dir(self, path, mode=0o755):
        self._add(path, stat.S_IFDIR | mode)

    def file(self, path, data, mode=0o644):
        self._add(path, stat.S_IFREG | mode, data)

    def symlink(self, path, target):
        self._add(path, stat.S_IFLNK | 0o777, target.encode())

    def node(self, path, major, minor, mode=0o600):
        self._add(path, stat.S_IFCHR | mode, rdev=(major, minor))

    def hardlinks(self, paths, data, mode=0o755):
        """paths share one inode; newc stores the data with the last link only."""
        ino = self.ino
        self.ino += 1
        for i, p in enumerate(paths):
            last = i == len(paths) - 1
            self._add(p, stat.S_IFREG | mode, data if last else b"", ino=ino, nlink=len(paths))

    def write(self, fobj):
        def hdr(ino, mode, nlink, size, rdev, namelen):
            f = "%08X" * 13
            return ("070701" + f % (ino, mode, 0, 0, nlink, 0, size, 0, 0, rdev[0], rdev[1], namelen, 0)).encode()
        out = bytearray()
        for path, mode, data, rdev, ino, nlink in self.entries:
            name = path.encode() + b"\0"
            out += hdr(ino, mode, nlink, len(data), rdev, len(name))
            out += name
            out += b"\0" * ((4 - len(out) % 4) % 4)
            out += data
            out += b"\0" * ((4 - len(out) % 4) % 4)
        out += hdr(0, 0, 1, 0, (0, 0), 11) + b"TRAILER!!!\0"
        out += b"\0" * ((4 - len(out) % 4) % 4)
        fobj.write(bytes(out))


def read(path):
    with open(path, "rb") as f:
        return f.read()


def ldd_libs(binary):
    out = subprocess.run(["ldd", binary], capture_output=True, text=True, check=True).stdout
    libs = []
    for line in out.splitlines():
        m = re.search(r"=> (/\S+)", line) or re.match(r"\s*(/\S+) \(", line)
        if m:
            libs.append(m.group(1))
    return libs


def add_base(c, latency_conf):
    for d in BASE_DIRS:
        c.dir(d, 0o1777 if d == "/tmp" else 0o755)
    for l, t in BASE_LINKS.items():
        c.symlink(l, t)
    for p, (ma, mi) in DEV_NODES.items():
        c.node(p, ma, mi, 0o666 if p != "/dev/kmsg" else 0o600)
    # --- fixture (identical across variants) ---
    stub = read(os.path.join(OUT, "nvstub"))
    c.hardlinks(sorted(STUB_NAMES), stub)
    for p, text in FIXTURE_FILES.items():
        c.file(p, text.encode())
    c.file("/etc/nvstub.conf", latency_conf.encode())
    c.file("/etc/ld.so.cache", b"")  # bind target for the gpu-extension loader cache (baked into real rootfs images)


def add_systemd(c, with_nvsd, probe=False):
    """Fedora's stock systemd, unmodified, copied from the build host (never committed)."""
    sd = "/usr/lib/systemd/systemd"
    execu = "/usr/lib/systemd/systemd-executor"
    c.dir("/usr/lib/systemd")
    c.file(sd, read(sd), 0o755)
    c.file(execu, read(execu), 0o755)
    libs = set(ldd_libs(sd)) | set(ldd_libs(execu))
    # systemd dlopen()s libmount from mount_option_mangle(); without it even the early
    # API filesystem mounts fail ("Failed to mount early API filesystems").
    for b in EXTRA_BINS:
        c.file(b, read(b), 0o755)  # no setuid bit: we are root anyway
        libs |= set(ldd_libs(b))
    for extra in DLOPEN_LIBS:
        libs.add(extra)
        libs |= set(ldd_libs(extra))
    placed = set()
    for lib in sorted(libs):
        real = os.path.realpath(lib)  # e.g. /lib64/libc.so.6 -> /usr/lib64/libc.so.6
        target = os.path.join(os.path.dirname(real), os.path.basename(lib))
        if target in placed:
            continue
        placed.add(target)
        d = os.path.dirname(target)
        if d not in ("/usr/lib64", "/usr/lib") and ("/" + d.strip("/")).lstrip("/") not in c.seen:
            c.dir(d)
        c.file(target, read(real), 0o755)
    if probe:
        c.file("/init", read(os.path.join(OUT, "nvstub")), 0o755)
    else:
        c.symlink("/init", "usr/lib/systemd/systemd")


def compress(raw, out, how):
    if how == "none":
        os.replace(raw, out)
        return
    cmd = {"zstd": ["zstd", "-q", "-19", "-T0", "-f", "-o", out, raw],
           "zstd3": ["zstd", "-q", "-3", "-T0", "-f", "-o", out, raw],
           "gzip": ["sh", "-c", "gzip -9 -n < %s > %s" % (raw, out)],
           "xz": ["xz", "-q", "--check=crc32", "-9", "-T1", "-c", raw]}[how]
    if how == "xz":
        with open(out, "wb") as f:
            subprocess.run(cmd, check=True, stdout=f)
    else:
        subprocess.run(cmd, check=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--variant", required=True, choices=["nvrc", "nvsd", "systemd-only"])
    ap.add_argument("--out", required=True)
    ap.add_argument("--compress", default="zstd", choices=["none", "zstd", "zstd3", "gzip", "xz"])
    ap.add_argument("--nvrc-bin")
    ap.add_argument("--extra", action="append", default=[], metavar="DEST=SRCFILE",
                    help="test-only: add a file to the image (e.g. a systemd drop-in)")
    ap.add_argument("--probe", action="store_true",
                    help="benchmark calibration: same image contents, but /init is the kata-agent stub, so the "
                         "PID 1 CPU time of the kernel's own init thread (initramfs unpack, initcalls) can be measured")
    ap.add_argument("--fixture-conf", help="file for /etc/nvstub.conf (default: empty = zero latency)")
    a = ap.parse_args()

    latency = read(a.fixture_conf).decode() if a.fixture_conf else "# zero simulated latency (fixture default)\n"
    c = Cpio()
    add_base(c, latency)
    if a.variant == "nvrc":
        if not a.nvrc_bin:
            sys.exit("--nvrc-bin required")
        c.file("/init", read(a.probe and os.path.join(OUT, "nvstub") or a.nvrc_bin), 0o755)
    else:
        add_systemd(c, a.variant == "nvsd", a.probe)
        if a.variant == "nvsd":
            sys.path.insert(0, os.path.join(REPO, "scripts"))
            import nvsd_overlay
            nvsd_overlay.apply(c, REPO, OUT)
    for spec in a.extra:
        dest, src = spec.split("=", 1)
        parts = dest.strip("/").split("/")[:-1]
        cur = ""
        for part in parts:
            cur += "/" + part
            if cur.lstrip("/") not in c.seen:
                c.dir(cur)
        c.file(dest, read(src), 0o644)
    raw = a.out + ".raw"
    with open(raw, "wb") as f:
        c.write(f)
    compress(raw, a.out, a.compress)
    if a.compress != "none":
        os.unlink(raw)
    print("%s: %d bytes (%s)" % (a.out, os.path.getsize(a.out), a.compress))


if __name__ == "__main__":
    main()
