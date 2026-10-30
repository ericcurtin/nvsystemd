#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Summarise raw benchmark JSON lines (bench/run.py) into a CSV and markdown tables.

  bench/summarize.py LABEL [LABEL...] --csv bench/results/x.csv --md bench/results/x.md

Statistics per (label, scenario, variant, metric): n, median, p95 (nearest rank), min, max, stdev, mean.
Calibration runs (variant "<v>-probe": same image, /init = stub) give the PID 1 CPU time of the kernel's own
init thread; "init_cpu_ms" = median PID 1 run time of the real init minus that of its probe, per variant.
"""
import argparse
import csv
import json
import math
import os
import statistics
import sys

WORK = os.environ.get("NVSD_WORK", "/home/ecurtin/nvsd-work")
METRICS = [  # (key, scale, unit, label)
    ("kata_start_us", 1e-3, "ms", "time to kata-agent start (CLOCK_BOOTTIME)"),
    ("kernel_us", 1e-3, "ms", "  of which kernel time before /init"),
    ("user_us", 1e-3, "ms", "  of which init to kata-agent"),
    ("host_wall_to_kata_s", 1e3, "ms", "host wall: QEMU spawn to first stub report line"),
    ("image_bytes", 1 / 1024, "KiB", "initramfs size (compressed)"),
    ("pid1_rss_kb", 1, "KiB", "init RSS at kata start"),
    ("pid1_pss_kb", 1, "KiB", "init PSS at kata start"),
    ("mem_used_kb", 1, "KiB", "memory used (MemTotal-MemAvailable) at kata start"),
    ("mem_used_kb_late", 1, "KiB", "memory used 200 ms later"),
    ("procs", 1, "", "user processes at kata start"),
    ("threads", 1, "", "user threads at kata start"),
    ("pid1_cpu_ticks", 10, "ms", "PID 1 utime+stime (10 ms ticks, includes kernel init thread)"),
    ("pid1_run_ns", 1e-6, "ms", "PID 1 run time (schedstat, includes kernel init thread)"),
]


def pctl(v, q):
    v = sorted(v)
    return v[max(0, math.ceil(q * len(v)) - 1)]


def stats(v):
    return {"n": len(v), "median": statistics.median(v), "p95": pctl(v, 0.95), "min": min(v), "max": max(v),
            "stdev": statistics.stdev(v) if len(v) > 1 else 0.0, "mean": statistics.fmean(v)}


def load(labels):
    rows = []
    for lb in labels:
        with open(os.path.join(WORK, "bench", "raw", lb + ".jsonl")) as f:
            rows += [json.loads(ln) for ln in f if ln.strip()]
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("labels", nargs="+")
    ap.add_argument("--csv")
    ap.add_argument("--md")
    a = ap.parse_args()
    rows = load(a.labels)
    groups = {}
    fails = {}
    for r in rows:
        key = (r["label"], r["scenario"], r["variant"])
        if not r["ok"]:
            fails[key] = fails.get(key, 0) + 1
            continue
        groups.setdefault(key, []).append(r)
    out = []  # csv rows
    for (lb, sc, var), rs in sorted(groups.items()):
        for key, scale, unit, _ in METRICS:
            vals = [r[key] * scale for r in rs if r.get(key) is not None]
            if not vals:
                continue
            st = stats(vals)
            out.append(dict(label=lb, scenario=sc, variant=var, metric=key, unit=unit, **{k: round(v, 3) for k, v in st.items()}))
    # derived: init CPU excess over the calibration (probe) image of the same variant, same scenario image set
    for (lb, sc, var), rs in sorted(groups.items()):
        if var.endswith("-probe"):
            continue
        probe = next((p for (l2, s2, v2), p in groups.items() if l2 == lb and v2 == var + "-probe"), None)
        if not probe:
            continue
        base = statistics.median(r["pid1_run_ns"] for r in probe)
        vals = [(r["pid1_run_ns"] - base) * 1e-6 for r in rs]
        st = stats(vals)
        out.append(dict(label=lb, scenario=sc, variant=var, metric="init_cpu_ms_vs_probe", unit="ms",
                        **{k: round(v, 3) for k, v in st.items()}))
    if a.csv:
        with open(a.csv, "w", newline="") as f:
            w = csv.DictWriter(f, fieldnames=["label", "scenario", "variant", "metric", "unit", "n", "median", "p95", "min", "max", "stdev", "mean"])
            w.writeheader()
            w.writerows(out)
    md = []
    by = {}
    for r in out:
        by.setdefault((r["label"], r["scenario"]), {}).setdefault(r["metric"], {})[r["variant"]] = r
    names = {m[0]: m[3] for m in METRICS}
    names["init_cpu_ms_vs_probe"] = "init CPU time (PID 1 run time minus calibration image)"
    for (lb, sc), ms in sorted(by.items()):
        variants = sorted({v for d in ms.values() for v in d if not v.endswith("-probe")})
        if not variants:
            continue
        md.append("\n#### %s / %s\n" % (lb, sc))
        md.append("| metric | " + " | ".join("%s median (p95, min-max, sd)" % v for v in variants) + " |")
        md.append("|---|" + "---|" * len(variants))
        for key in [m[0] for m in METRICS] + ["init_cpu_ms_vs_probe"]:
            if key not in ms:
                continue
            cells = []
            for v in variants:
                r = ms[key].get(v)
                cells.append("%.1f (%.1f, %.1f-%.1f, %.1f) n=%d" % (r["median"], r["p95"], r["min"], r["max"], r["stdev"], r["n"]) if r else "-")
            md.append("| %s [%s] | " % (names[key], ms[key][variants[0]]["unit"] if variants[0] in ms[key] else "") + " | ".join(cells) + " |")
    if fails:
        md.append("\nFailed runs (no report): " + ", ".join("%s/%s/%s: %d" % (k[0], k[1], k[2], n) for k, n in sorted(fails.items())))
    text = "\n".join(md) + "\n"
    if a.md:
        with open(a.md, "w") as f:
            f.write(text)
    else:
        print(text)


if __name__ == "__main__":
    main()
