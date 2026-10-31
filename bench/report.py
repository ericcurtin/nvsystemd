#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Build the markdown tables of docs/BENCHMARK.md from raw benchmark lines.

  bench/report.py LABEL [--baseline LABEL2] [--update docs/BENCHMARK.md]

With --update the text between <!-- TABLES:LABEL --> and <!-- /TABLES:LABEL --> is replaced.
Every number comes from the raw JSON lines (bench/run.py); nothing is typed in.
"""
import argparse
import glob
import json
import os
import re
import statistics
import subprocess
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "scripts"))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mkimage  # noqa: E402
import summarize as S  # noqa: E402

WORK = S.WORK
OUT = os.environ.get("NVSD_OUT", os.path.join(WORK, "out"))
NVRC_BIN = os.environ.get("NVRC_BIN", os.path.join(WORK, "target/x86_64-unknown-linux-musl/release/NVRC"))


def load(label):
    return [r for r in S.load([label]) if r["ok"]]


def col(rows, sc, var, key, scale=1.0):
    v = [r[key] * scale for r in rows if r["scenario"] == sc and r["variant"] == var and r.get(key) is not None]
    return S.stats(v) if v else None


def fmt(st, nd=1):
    return "%.*f" % (nd, st["median"]) if st else "-"


def full(st, nd=1):
    return "%.*f / %.*f / %.*f-%.*f / %.*f" % (nd, st["median"], nd, st["p95"], nd, st["min"], nd, st["max"], nd, st["stdev"]) if st else "-"


def paired(rows, sc, key, a="nvrc", b="nvsd"):
    da = {r["round"]: r[key] for r in rows if r["scenario"] == sc and r["variant"] == a}
    db = {r["round"]: r[key] for r in rows if r["scenario"] == sc and r["variant"] == b}
    d = [(db[k] - da[k]) / 1000.0 for k in da if k in db]
    return statistics.median(d) if d else None


def sizes():
    libs = set(mkimage.ldd_libs("/usr/lib/systemd/systemd")) | set(mkimage.ldd_libs("/usr/lib/systemd/systemd-executor"))
    lib_total = sum(os.path.getsize(os.path.realpath(x)) for x in libs)
    sd = os.path.getsize("/usr/lib/systemd/systemd")
    ex = os.path.getsize("/usr/lib/systemd/systemd-executor")
    helper = os.path.getsize(os.path.join(OUT, "nvsd-helper"))
    return {"nvrc": os.path.getsize(NVRC_BIN), "systemd": sd, "executor": ex, "libs": lib_total, "helper": helper}


def raw_size(pattern):
    f = glob.glob(os.path.join(OUT, "images", pattern))
    if not f:
        return None
    p = subprocess.run("zstd -dc %s | wc -c" % f[0], shell=True, capture_output=True, text=True)
    return int(p.stdout.strip())


def tables(label, base_label):
    rows = load(label)
    scs = [s for s in ["S0", "S1", "S2", "S3", "S4", "S3x0.25", "S3x0.5", "S3x2"] if any(r["scenario"] == s for r in rows)]
    md = []
    names = {"S0": "S0 cpu", "S1": "S1 gpu defaults", "S2": "S2 gpu + smi x4 + dcgm", "S3": "S3 = S2 + SIMULATED latency x1",
             "S4": "S4 servicevm-nvl4", "S3x0.25": "S3 latency x0.25", "S3x0.5": "S3 latency x0.5", "S3x2": "S3 latency x2"}
    md.append("#### Time to kata-agent stub start, CLOCK_BOOTTIME, ms (median / p95 / min-max / stdev), n runs per cell\n")
    md.append("| scenario | n | NVRC | nvsystemd | paired diff (nvsd - nvrc), median | winner |")
    md.append("|---|---|---|---|---|---|")
    for sc in scs:
        a, b = col(rows, sc, "nvrc", "kata_start_us", 1e-3), col(rows, sc, "nvsd", "kata_start_us", 1e-3)
        d = paired(rows, sc, "kata_start_us")
        md.append("| %s | %d | %s | %s | %+.1f | %s |" % (names[sc], b["n"], full(a), full(b), d, "nvsystemd" if d < 0 else "NVRC"))
    md.append("\n#### Of which: kernel time before /init, ms (same kernel; difference = initramfs size)\n")
    md.append("| scenario | NVRC | nvsystemd |")
    md.append("|---|---|---|")
    for sc in scs:
        md.append("| %s | %s | %s |" % (names[sc], full(col(rows, sc, "nvrc", "kernel_us", 1e-3)), full(col(rows, sc, "nvsd", "kernel_us", 1e-3))))
    md.append("\n#### Of which: init to kata-agent start (userspace), ms\n")
    md.append("| scenario | NVRC | nvsystemd | paired diff, median |")
    md.append("|---|---|---|---|")
    for sc in scs:
        md.append("| %s | %s | %s | %+.1f |" % (names[sc], full(col(rows, sc, "nvrc", "user_us", 1e-3), 2), full(col(rows, sc, "nvsd", "user_us", 1e-3), 1), paired(rows, sc, "user_us")))
    md.append("\n#### Resources at kata-agent start (S2 unless noted), median / p95 / min-max / stdev\n")
    sz = sizes()
    sc = "S2" if "S2" in scs else scs[0]
    rawn = raw_size("nvrc-bench-%s-nvrc-zstd.cpio.zst" % sc)
    rawd = raw_size("nvsd-bench-%s-nvsd-zstd.cpio.zst" % sc)
    md.append("| metric | NVRC | nvsystemd |")
    md.append("|---|---|---|")
    items = [("initramfs, compressed (zstd -19), KiB", "image_bytes", 1 / 1024.0, 0),
             ("init RSS, KiB (NVRC: its syslog-poller child, the process image NVRC keeps after exec)", "pid1_rss_kb", 1, 0),
             ("init PSS, KiB", "pid1_pss_kb", 1, 0),
             ("memory used = MemTotal - MemAvailable, KiB", "mem_used_kb", 1, 0),
             ("memory used 200 ms later, KiB", "mem_used_kb_late", 1, 0),
             ("user processes", "procs", 1, 0), ("user threads", "threads", 1, 0)]
    for lbl, key, scale, nd in items:
        md.append("| %s | %s | %s |" % (lbl, full(col(rows, sc, "nvrc", key, scale), nd), full(col(rows, sc, "nvsd", key, scale), nd)))
    ncpu = col(rows, sc, "nvrc", "init_cpu_ms_vs_probe") if False else None
    # init CPU = PID 1 run time minus the calibration image of the same variant
    probes = {v: [r["pid1_run_ns"] for r in S.load([label]) if r["ok"] and r["variant"] == v + "-probe"] for v in ("nvrc", "nvsd")}
    cpu = {}
    for v in ("nvrc", "nvsd"):
        if probes[v]:
            base = statistics.median(probes[v])
            vals = [(r["pid1_run_ns"] - base) / 1e6 for r in rows if r["scenario"] == sc and r["variant"] == v]
            cpu[v] = S.stats(vals)
    if cpu:
        md.append("| PID 1 CPU time of the init itself, ms (PID 1 schedstat run time minus the calibration image of the same variant) | %s | %s |" % (full(cpu.get("nvrc"), 1), full(cpu.get("nvsd"), 1)))
    md.append("| PID 1 binary on disk, KiB | %.0f (NVRC, static musl) | %.0f (systemd) + %.0f (systemd-executor) |" % (sz["nvrc"] / 1024, sz["systemd"] / 1024, sz["executor"] / 1024))
    md.append("| everything the init needs on disk (binary + shared libraries), KiB | %.0f | %.0f (+ helper %.0f, units) |" % (sz["nvrc"] / 1024, (sz["systemd"] + sz["executor"] + sz["libs"]) / 1024, sz["helper"] / 1024))
    if rawn and rawd:
        md.append("| initramfs uncompressed (incl. identical fixture), KiB | %.0f | %.0f |" % (rawn / 1024, rawd / 1024))
    if base_label:
        brows = load(base_label)
        md.append("\n#### Effect of the tuning (nvsystemd before tuning = label %s, same harness)\n" % base_label)
        md.append("| scenario | init to kata-agent before | after | time to kata-agent before | after |")
        md.append("|---|---|---|---|---|")
        for sc in scs:
            b0, b1 = col(brows, sc, "nvsd", "user_us", 1e-3), col(rows, sc, "nvsd", "user_us", 1e-3)
            k0, k1 = col(brows, sc, "nvsd", "kata_start_us", 1e-3), col(rows, sc, "nvsd", "kata_start_us", 1e-3)
            if b0 and b1:
                md.append("| %s | %s | %s | %s | %s |" % (names[sc], fmt(b0), fmt(b1), fmt(k0), fmt(k1)))
    return "\n".join(md) + "\n"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("label")
    ap.add_argument("--baseline")
    ap.add_argument("--update")
    a = ap.parse_args()
    t = tables(a.label, a.baseline)
    if a.update:
        s = open(a.update).read()
        pat = re.compile(r"(<!-- TABLES:%s -->\n).*?(<!-- /TABLES:%s -->)" % (a.label, a.label), re.S)
        if not pat.search(s):
            sys.exit("markers not found in " + a.update)
        s = pat.sub(lambda m: m.group(1) + t + m.group(2), s)
        open(a.update, "w").write(s)
    else:
        print(t)


if __name__ == "__main__":
    main()
