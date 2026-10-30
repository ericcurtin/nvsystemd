#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Paired comparison of variants measured in the same rounds (robust to bursts of host noise):

  bench/compare.py LABEL [--base VARIANT] [--metric user_us]

For every scenario prints, per variant, the median of (variant - base) over rounds, plus the plain medians.
"""
import argparse
import json
import os
import statistics

WORK = os.environ.get("NVSD_WORK", "/home/ecurtin/nvsd-work")
ap = argparse.ArgumentParser()
ap.add_argument("label")
ap.add_argument("--base")
ap.add_argument("--metric", default="user_us")
a = ap.parse_args()
rows = [json.loads(ln) for ln in open(os.path.join(WORK, "bench", "raw", a.label + ".jsonl")) if ln.strip()]
rows = [r for r in rows if r["ok"] and not r["variant"].endswith("-probe")]
for sc in sorted({r["scenario"] for r in rows}):
    by = {}
    for r in rows:
        if r["scenario"] == sc:
            by.setdefault(r["variant"], {})[r["round"]] = r[a.metric]
    base = a.base or next(iter(by))
    print("%s (%s, us->ms), base=%s" % (sc, a.metric, base))
    for v, d in by.items():
        diffs = [(d[k] - by[base][k]) / 1000 for k in d if k in by[base]]
        vals = [x / 1000 for x in d.values()]
        q = statistics.quantiles(vals, n=4) if len(vals) > 3 else [0, 0, 0]
        print("  %-12s median %8.2f  p25 %8.2f  min %8.2f | paired diff median %+7.2f (n=%d)" % (
            v, statistics.median(vals), q[0], min(vals), statistics.median(diffs) if diffs else 0, len(diffs)))
