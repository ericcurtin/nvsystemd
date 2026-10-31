#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""vmrun.py: boot one initramfs in QEMU/KVM and capture the serial console.

Used by the boot tests and the benchmark so that both init systems always get
exactly the same machine: q35 + KVM, 2 vCPU, 512 MiB (memfd-backed so fake PCI
devices can attach), no default devices, serial console only.

Every QEMU run is wrapped in `timeout -s KILL`. The output file has one line per
console line, prefixed with the host-side seconds since QEMU was spawned.
"""
import argparse
import os
import subprocess
import sys
import tempfile
import threading
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "fixture", "fakepci"))
import fakepci  # noqa: E402

DEFAULT_KERNEL = "/lib/modules/7.2.13-200.fc44.x86_64/vmlinuz"
# Identical for every variant and scenario. systemd.* items are ignored by NVRC,
# nvrc.* / kata.* items are ignored by systemd.
BASE_CMDLINE = ("console=ttyS0 quiet panic=-1 8250.nr_uarts=1 random.trust_cpu=on TERM=dumb "
                "tsc=reliable no_timer_check log_buf_len=1M")

PCI_PRESETS = {
    "none": [],
    "gpu1": ["10de:2330:030200"],
    "gpu2": ["10de:2330:030200"] * 2,
    "gpu8": ["10de:2330:030200"] * 8,
    "nvl4": ["10de:22a3:068000"] * 4,
    "gpu8-nvl4": ["10de:2330:030200"] * 8 + ["10de:22a3:068000"] * 4,
    "nvl5": ["15b3:1021:020700:vpd=SW_MNG"] * 4,
    "bad-topology": ["10de:22a3:068000"] * 2,
}


def qemu_cmd(kernel, initrd, append, pci_args, smp=2, mem=512, qemu="qemu-system-x86_64", extra=()):
    return [
        qemu, "-machine", "q35,accel=kvm,memory-backend=pc.ram", "-cpu", "host",
        "-object", "memory-backend-memfd,id=pc.ram,size=%dM,share=on" % mem,
        "-smp", str(smp), "-m", str(mem), "-nodefaults", "-no-user-config", "-display", "none",
        "-chardev", "stdio,id=con0,signal=off", "-serial", "chardev:con0", "-monitor", "none",
        "-no-reboot", "-kernel", kernel, "-initrd", initrd, "-append", append,
    ] + pci_args + list(extra)


def run(kernel, initrd, append, pci=(), timeout=30, smp=2, mem=512, out=None, qemu_extra=()):
    """Returns dict(lines=[(t, text)], rc, wall, timed_out)."""
    specs = []
    for p in pci:
        specs += PCI_PRESETS[p] if p in PCI_PRESETS else [p]
    sockdir = tempfile.mkdtemp(prefix="nvsd-pci-")
    devs, pci_args = fakepci.start_devices(specs, sockdir)
    cmd = ["timeout", "-s", "KILL", str(timeout)]
    cpus = os.environ.get("NVSD_CPUS")  # e.g. "12-15": pin QEMU (all threads) to dedicated host cores (benchmarks)
    if cpus:
        cmd += ["taskset", "-c", cpus]
    cmd += qemu_cmd(kernel, initrd, append, pci_args, smp, mem, extra=qemu_extra)
    t0 = time.monotonic()
    proc = subprocess.Popen(cmd, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    lines = []
    buf = b""
    while True:
        chunk = os.read(proc.stdout.fileno(), 65536)
        if not chunk:
            break
        now = time.monotonic() - t0
        buf += chunk
        while b"\n" in buf:
            ln, buf = buf.split(b"\n", 1)
            lines.append((now, ln.decode("utf-8", "replace").rstrip("\r")))
    if buf:
        lines.append((time.monotonic() - t0, buf.decode("utf-8", "replace")))
    rc = proc.wait()
    wall = time.monotonic() - t0
    for d in devs:
        try:
            d.sock.close()
        except OSError:
            pass
    for f in os.listdir(sockdir):
        os.unlink(os.path.join(sockdir, f))
    os.rmdir(sockdir)
    res = {"lines": lines, "rc": rc, "wall": wall, "timed_out": rc == -9 or rc == 137}
    if out:
        with open(out, "w") as f:
            for t, ln in lines:
                f.write("%.6f %s\n" % (t, ln))
    return res


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--initrd", required=True)
    ap.add_argument("--kernel", default=DEFAULT_KERNEL)
    ap.add_argument("--append", default="")
    ap.add_argument("--pci", action="append", default=[], help="preset name or vendor:device:class[:vpd=..]")
    ap.add_argument("--timeout", type=int, default=30)
    ap.add_argument("--out")
    a = ap.parse_args()
    r = run(a.kernel, a.initrd, (BASE_CMDLINE + " " + a.append).strip(), a.pci, a.timeout, out=a.out)
    for t, ln in r["lines"]:
        print("%.3f %s" % (t, ln))
    print("rc=%s wall=%.3f" % (r["rc"], r["wall"]), file=sys.stderr)


if __name__ == "__main__":
    main()
