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


VARIANTS = [("nvrc", "NVRC"), ("nvsd", "nvsystemd (Fedora systemd)"), ("nvsd-centos10", "nvsystemd (CentOS 10 systemd)")]
CENTOS10 = os.environ.get("NVSD_CENTOS10", os.path.join(WORK, "centos10", "sysroot"))


def paired(rows, sc, key, a, b):
    """Median over rounds of (b - a) in the same round, ms."""
    da = {r["round"]: r[key] for r in rows if r["scenario"] == sc and r["variant"] == a}
    db = {r["round"]: r[key] for r in rows if r["scenario"] == sc and r["variant"] == b}
    d = [(db[k] - da[k]) / 1000.0 for k in da if k in db]
    return statistics.median(d) if d else None


def pc(x, nd=1):
    return "%+.*f" % (nd, x) if x is not None else "-"


def sizes():
    """On-disk sizes of the init and everything it needs, per base (Fedora from the build host, CentOS 10 from the sysroot)."""
    libs = set(mkimage.ldd_libs("/usr/lib/systemd/systemd")) | set(mkimage.ldd_libs("/usr/lib/systemd/systemd-executor"))
    fed = {"systemd": os.path.getsize("/usr/lib/systemd/systemd"), "executor": os.path.getsize("/usr/lib/systemd/systemd-executor"),
           "libs": sum(os.path.getsize(os.path.realpath(x)) for x in libs)}
    c10 = {}
    if os.path.isdir(CENTOS10):
        c10 = {"systemd": os.path.getsize(os.path.join(CENTOS10, "usr/lib/systemd/systemd")),
               "executor": os.path.getsize(os.path.join(CENTOS10, "usr/lib/systemd/systemd-executor"))}
        tot = 0
        for d, _dn, fn in os.walk(CENTOS10):
            for f in fn:
                tot += os.path.getsize(os.path.join(d, f))
        c10["libs"] = tot - c10["systemd"] - c10["executor"] - os.path.getsize(os.path.join(CENTOS10, "usr/bin/mount"))
        # libs here = every library file in the sysroot (mount binary excluded), comparable to the Fedora ldd set
    return {"nvrc": os.path.getsize(NVRC_BIN), "fedora": fed, "centos10": c10, "helper": os.path.getsize(os.path.join(OUT, "nvsd-helper"))}


def raw_size(pattern):
    f = glob.glob(os.path.join(OUT, "images", pattern))
    if not f:
        return None
    p = subprocess.run("zstd -dc %s | wc -c" % f[0], shell=True, capture_output=True, text=True)
    return int(p.stdout.strip())


def versions():
    v = {}
    try:
        v["fedora"] = subprocess.run(["rpm", "-q", "systemd", "--qf", "%{VERSION}-%{RELEASE}"], capture_output=True, text=True).stdout
    except OSError:
        v["fedora"] = "?"
    try:
        v["centos10"] = open(os.path.join(os.path.dirname(CENTOS10), "VERSION")).read().strip()
    except OSError:
        v["centos10"] = "?"
    return v


def tables(label, base_label):
    allrows = S.load([label])
    rows = [r for r in allrows if r["ok"]]
    present = [(k, n) for k, n in VARIANTS if any(r["variant"] == k for r in rows)]
    scs = [s for s in ["S0", "S1", "S2", "S3", "S4", "S3x0.25", "S3x0.5", "S3x2"] if any(r["scenario"] == s for r in rows)]
    md = []
    names = {"S0": "S0 cpu", "S1": "S1 gpu defaults", "S2": "S2 gpu + smi x4 + dcgm", "S3": "S3 = S2 + SIMULATED latency x1",
             "S4": "S4 servicevm-nvl4", "S3x0.25": "S3 latency x0.25", "S3x0.5": "S3 latency x0.5", "S3x2": "S3 latency x2"}
    hdr = " | ".join(n for _k, n in present)
    sep = "|---" * (len(present))
    pairs = [(a, b) for a, b in (("nvrc", "nvsd"), ("nvrc", "nvsd-centos10"), ("nvsd", "nvsd-centos10"))
             if any(k == a for k, _ in present) and any(k == b for k, _ in present)]
    plab = {"nvrc": "NVRC", "nvsd": "nvsd-Fedora", "nvsd-centos10": "nvsd-CentOS10"}
    phdr = " | ".join("paired %s - %s" % (plab[b], plab[a]) for a, b in pairs)

    def table(title, key, scale, nd, with_pairs=True):
        md.append("\n#### %s\n" % title)
        md.append("| scenario | n | %s%s |" % (hdr, " | " + phdr if with_pairs else ""))
        md.append("|---|---" + sep + ("|---" * len(pairs) if with_pairs else "") + "|")
        for sc in scs:
            cols = [col(rows, sc, k, key, scale) for k, _ in present]
            n = min(c["n"] for c in cols if c)
            line = "| %s | %d | %s" % (names[sc], n, " | ".join(full(c, nd) for c in cols))
            if with_pairs:
                line += " | " + " | ".join(pc(paired(rows, sc, key, a, b)) for a, b in pairs)
            md.append(line + " |")

    md.append("Cells: median / p95 / min-max / sample stdev, in ms. Paired columns: median over rounds of the difference measured in the same round (negative = first-named is faster).")
    table("Time to kata-agent stub start, CLOCK_BOOTTIME, ms", "kata_start_us", 1e-3, 1)
    table("Of which: kernel time before /init, ms (same kernel; the difference is the initramfs size)", "kernel_us", 1e-3, 1)
    table("Of which: init to kata-agent start (userspace), ms", "user_us", 1e-3, 1)

    sz = sizes()
    vs = versions()
    sc = "S2" if "S2" in scs else scs[0]
    md.append("\n#### Resources at kata-agent start (%s), median / p95 / min-max / stdev\n" % names[sc])
    md.append("| metric | %s |" % hdr)
    md.append("|---|" + sep.lstrip("|") + "|")
    items = [("initramfs, compressed (zstd -19), KiB", "image_bytes", 1 / 1024.0, 0),
             ("init RSS, KiB (NVRC: its syslog-poller child, the process image NVRC keeps after exec)", "pid1_rss_kb", 1, 0),
             ("init PSS, KiB", "pid1_pss_kb", 1, 0),
             ("memory used = MemTotal - MemAvailable, KiB", "mem_used_kb", 1, 0),
             ("memory used 200 ms later, KiB", "mem_used_kb_late", 1, 0),
             ("user processes", "procs", 1, 0), ("user threads", "threads", 1, 0)]
    for lbl, key, scale, nd in items:
        md.append("| %s | %s |" % (lbl, " | ".join(full(col(rows, sc, k, key, scale), nd) for k, _ in present)))
    probes = {k: [r["pid1_run_ns"] for r in allrows if r["ok"] and r["variant"] == k + "-probe"] for k, _ in present}
    cpu = {}
    for k, _ in present:
        if probes[k]:
            base = statistics.median(probes[k])
            cpu[k] = S.stats([(r["pid1_run_ns"] - base) / 1e6 for r in rows if r["scenario"] == sc and r["variant"] == k])
    if cpu:
        md.append("| PID 1 CPU time of the init itself, ms (PID 1 schedstat run time minus the calibration image of the same variant) | %s |" % " | ".join(full(cpu.get(k), 1) for k, _ in present))
    cells = {"nvrc": "%.0f (static musl)" % (sz["nvrc"] / 1024)}
    for b, k in (("fedora", "nvsd"), ("centos10", "nvsd-centos10")):
        if sz[b]:
            cells[k] = "%.0f + %.0f (executor)" % (sz[b]["systemd"] / 1024, sz[b]["executor"] / 1024)
    md.append("| PID 1 binary on disk, KiB | %s |" % " | ".join(cells.get(k, "-") for k, _ in present))
    cells = {"nvrc": "%.0f" % (sz["nvrc"] / 1024)}
    for b, k in (("fedora", "nvsd"), ("centos10", "nvsd-centos10")):
        if sz[b]:
            cells[k] = "%.0f (+ helper %.0f, units)" % ((sz[b]["systemd"] + sz[b]["executor"] + sz[b]["libs"]) / 1024, sz["helper"] / 1024)
    md.append("| everything the init needs on disk (binaries + shared libraries), KiB | %s |" % " | ".join(cells.get(k, "-") for k, _ in present))
    cells = {}
    for k, _ in present:
        rn = raw_size("%s-bench-%s-%s-zstd.cpio.zst" % (k, sc, k))
        cells[k] = "%.0f" % (rn / 1024) if rn else "-"
    md.append("| initramfs uncompressed (incl. identical fixture), KiB | %s |" % " | ".join(cells.get(k, "-") for k, _ in present))
    md.append("\nSystemd versions: Fedora `%s`, CentOS Stream 10 `%s`." % (vs["fedora"], vs["centos10"]))

    # discarded rounds (host noise gating)
    dpath = os.path.join(WORK, "bench", "raw", label + ".discarded.jsonl")
    nd_rounds = 0
    if os.path.exists(dpath):
        seen = set()
        for ln in open(dpath):
            r = json.loads(ln)
            seen.add((r["scenario"], r["round"], r["attempt"]))
        nd_rounds = len(seen)
    noisy = len({(r["scenario"], r["round"]) for r in rows if r.get("noisy")})
    loads = [r["host_before"]["load1"] for r in rows if "host_before" in r]
    busy = [r["host_busy_cpus_during"] for r in rows if "host_busy_cpus_during" in r]
    if loads:
        md.append("\nHost during this run: load1 median %.2f, max %.2f; other-CPU busy during rounds median %.2f, max %.2f CPUs; "
                  "%d rounds discarded and redone (kept in `%s.discarded.jsonl`), %d rounds kept although flagged noisy after the attempt limit; "
                  "%d failed runs." % (statistics.median(loads), max(loads), statistics.median(busy), max(busy), nd_rounds, label,
                                       noisy, sum(1 for r in allrows if not r["ok"])))
    if base_label:
        brows = load(base_label)
        md.append("\n#### Effect of the tuning (nvsystemd before tuning = label %s, same harness, run separately)\n" % base_label)
        md.append("| scenario | init to kata-agent before | after (Fedora) | after (CentOS 10) | time to kata-agent before | after (Fedora) | after (CentOS 10) |")
        md.append("|---|---|---|---|---|---|---|")
        for sc in scs:
            b0, b1, b2 = (col(brows, sc, "nvsd", "user_us", 1e-3), col(rows, sc, "nvsd", "user_us", 1e-3), col(rows, sc, "nvsd-centos10", "user_us", 1e-3))
            k0, k1, k2 = (col(brows, sc, "nvsd", "kata_start_us", 1e-3), col(rows, sc, "nvsd", "kata_start_us", 1e-3), col(rows, sc, "nvsd-centos10", "kata_start_us", 1e-3))
            if b0 and b1:
                md.append("| %s | %s | %s | %s | %s | %s | %s |" % (names[sc], fmt(b0), fmt(b1), fmt(b2), fmt(k0), fmt(k1), fmt(k2)))
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
