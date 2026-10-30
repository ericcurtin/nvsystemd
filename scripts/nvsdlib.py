# SPDX-License-Identifier: Apache-2.0
"""Shared helpers for boot tests and benchmarks: build images, boot them, parse the
report that the kata-agent stub prints on the serial console."""
import os
import re
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.environ.get("NVSD_OUT", "/home/ecurtin/nvsd-work/out")
NVRC_BIN = os.environ.get("NVRC_BIN", "/home/ecurtin/nvsd-work/target/x86_64-unknown-linux-musl/release/NVRC")
sys.path.insert(0, os.path.join(REPO, "scripts"))
import vmrun  # noqa: E402


def sh(cmd, **kw):
    return subprocess.run(cmd, check=True, **kw)


def build_tools():
    targets = []
    if os.path.exists(os.path.join(REPO, "fixture", "stub", "nvstub.c")):
        targets.append("stub")
    if os.path.exists(os.path.join(REPO, "helper", "nvsd-helper.c")):
        targets.append("helper")
    if targets:
        sh(["make", "-C", REPO, "-s"] + targets, env=dict(os.environ, OUT=OUT))


def image_path(variant, fixture_name, compress):
    return os.path.join(OUT, "images", "%s-%s-%s.cpio%s" % (variant, fixture_name, compress, {"none": "", "zstd": ".zst", "zstd3": ".zst", "gzip": ".gz", "xz": ".xz"}[compress]))


def build_image(variant, fixture_text="", fixture_name="zero", compress="zstd3", force=False, extra=None):
    """Build (or reuse) an image. fixture_text is the content of /etc/nvstub.conf. A variant name ending in
    -probe builds the calibration image (same contents, /init = kata-agent stub)."""
    probe = variant.endswith("-probe")
    variant = variant[:-6] if probe else variant
    os.makedirs(os.path.join(OUT, "images"), exist_ok=True)
    out = image_path(variant + ("-probe" if probe else ""), fixture_name, compress)
    conf = os.path.join(OUT, "images", "fixture-%s.conf" % fixture_name)
    with open(conf, "w") as f:
        f.write(fixture_text)
    args = [sys.executable, os.path.join(REPO, "scripts", "mkimage.py"), "--variant", variant, "--out", out,
            "--compress", compress, "--fixture-conf", conf]
    for dest, text in (extra or {}).items():  # test-only files, e.g. systemd drop-ins
        src = os.path.join(OUT, "images", "extra-%s-%s" % (fixture_name, dest.strip("/").replace("/", "_")))
        with open(src, "w") as f:
            f.write(text)
        args += ["--extra", "%s=%s" % (dest, src)]
    if probe:
        args += ["--probe"]
    if variant in ("nvrc", "probe"):
        args += ["--nvrc-bin", NVRC_BIN]
    env = dict(os.environ, NVSD_OUT=OUT)
    subprocess.run(args, check=True, env=env, stdout=subprocess.DEVNULL)
    return out


KV = re.compile(r"(\w+)=(\S*)")


def kv(line):
    return {k: v for k, v in KV.findall(line)}


class Report:
    def __init__(self, lines):
        self.raw = lines
        self.kata = None
        self.env = None
        self.mem = {}
        self.procs = {}
        self.census = {}
        self.mounts = {}
        self.facts = {}
        self.kmsg = []
        self.log = []
        self.complete = False
        self.kata_host_t = None
        in_log = False
        echo = []
        for t, ln in lines:
            if ln.startswith("NVSD-KATA-ENV "):
                self.env = kv(ln)
            elif ln.startswith("NVSD-KATA "):
                self.kata = {k: int(v) if v.isdigit() else v for k, v in kv(ln).items()}
                self.kata_host_t = t
            elif ln.startswith("NVSD-MEM "):
                d = kv(ln)
                self.mem[ln.split()[1]] = {k: int(v) for k, v in d.items()}
            elif ln.startswith("NVSD-PROC "):
                d = kv(ln)
                d["_label"] = ln.split()[1]
                self.procs.setdefault(d["_label"], []).append(d)
            elif ln.startswith("NVSD-MOUNT "):
                _, mp, fs, opts = ln.split(" ", 3)
                self.mounts[mp] = (fs, opts.split(","))
            elif ln.startswith("NVSD-KMSG "):
                self.kmsg.append(ln[10:])
            elif ln.startswith("NVSD-FACT "):
                self.facts = kv(ln)
            elif ln.startswith("NVSD-CENSUS "):
                self.census[ln.split()[1]] = {k: int(v) for k, v in kv(ln).items()}
            elif ln == "NVSD-LOG-BEGIN":
                in_log = True
            elif ln == "NVSD-LOG-END":
                in_log = False
            elif ln == "NVSD-REPORT-END":
                self.complete = True
            elif in_log:
                self._add(self.log, ln)
            elif ln.startswith("NVSTUB "):
                self._add(echo, ln[7:])
        if not self.log:  # failure paths never print the report: fall back to the echoed lines
            self.log = echo

    @staticmethod
    def _add(dst, ln):
        parts = ln.split(" ", 4)
        if len(parts) >= 4 and parts[0].isdigit():
            dst.append({"t": int(parts[0]), "pid": int(parts[1]), "name": parts[2], "event": parts[3],
                        "detail": parts[4] if len(parts) > 4 else ""})

    def events(self, name, event=None):
        return [e for e in self.log if e["name"] == name and (event is None or e["event"] == event)]

    def first(self, name, event):
        ev = self.events(name, event)
        return ev[0] if ev else None

    def names_in_order(self):
        return [(e["name"], e["event"]) for e in self.log]

    def powered_off(self):
        return any("reboot: Power down" in ln or "Power down" in ln for _t, ln in self.raw)


def boot(variant, fixture_text="", fixture_name="zero", pci=(), append="", timeout=30, compress="zstd3", kernel=None,
         image=None, out=None, extra=None, qemu_extra=()):
    img = image or build_image(variant, fixture_text, fixture_name, compress, extra=extra)
    r = vmrun.run(kernel or vmrun.DEFAULT_KERNEL, img, (vmrun.BASE_CMDLINE + " " + append).strip(), pci, timeout, out=out,
                 qemu_extra=qemu_extra)
    rep = Report(r["lines"])
    rep.rc, rep.wall, rep.timed_out = r["rc"], r["wall"], r["timed_out"]
    rep.text = "\n".join(ln for _t, ln in r["lines"])
    return rep


def extension_disk(name="gpu", path=None):
    """A tiny raw disk with an MBR and two partitions (data, hash) for the composable-extension tests, plus the
    QEMU arguments attaching it as virtio-blk with serial extension-<name>. Contents are irrelevant (the
    veritysetup stub never reads them)."""
    import struct
    path = path or os.path.join(OUT, "images", "extdisk-%s.raw" % name)
    mbr = bytearray(512)
    for i, (start, size) in enumerate(((2048, 1024), (4096, 1024))):
        mbr[446 + 16 * i: 446 + 16 * i + 16] = struct.pack("<B3sB3sII", 0, b"\0\0\0", 0x83, b"\0\0\0", start, size)
    mbr[510:512] = b"\x55\xaa"
    with open(path, "wb") as f:
        f.write(mbr)
        f.truncate(3 * 1024 * 1024)
    return ["-drive", "if=none,id=ext0,format=raw,file=%s" % path, "-device", "virtio-blk-pci,drive=ext0,serial=extension-%s" % name]
