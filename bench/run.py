#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Benchmark driver: NVRC (unmodified, release profile, static musl) vs nvsystemd, interleaved A/B.

  bench/run.py --scenarios S0,S1,S2,S3 --runs 30 [--variants nvrc,nvsd] [--label baseline]

Variants: nvrc, nvsd (nvsystemd on Fedora's systemd), nvsd-centos10 (same overlay on CentOS Stream 10's systemd);
"<v>-probe" are the calibration images. All variants boot once per round, in a permuted order (interleaved).
Resumable: rounds already present in the raw file are skipped. Rounds taken while other host load was above
--busy-limit (before or after the round) are discarded into <label>.discarded.jsonl and redone.

Everything is SIMULATED hardware (fixture/): no GPU exists on this host. Per run it boots one image in
QEMU/KVM (same kernel, same flags, same command line for both inits) and parses the report that the
kata-agent stub prints. Raw per-run JSON lines go to $NVSD_WORK/bench/raw/<label>.jsonl (not committed);
bench/summarize.py turns them into the CSV/markdown that is committed.
"""
import argparse
import itertools
import json
import os
import random
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "scripts"))
import nvsdlib as L  # noqa: E402
import vmrun  # noqa: E402

WORK = os.environ.get("NVSD_WORK", "/home/ecurtin/nvsd-work")

SMI = "nvrc.smi.lmc=5001 nvrc.smi.lgc=1500 nvrc.smi.pl=300 nvrc.smi.srs=1 nvrc.dcgm=on"
# SIMULATED latency profile of S3 (milliseconds); every entry is a fixture knob (key=value in /etc/nvstub.conf)
S3_PROFILE = {"latency.modprobe.nvidia": 300, "latency.nvidia-persistenced": 50, "latency.nvidia-smi.lmc": 100,
              "latency.nvidia-smi.lgc": 100, "latency.nvidia-smi.pl": 100, "latency.nvidia-smi.srs": 100,
              "latency.nv-hostengine": 80, "latency.nvidia-ctk": 150}


def profile_text(scale):
    return "".join("%s=%d\n" % (k, round(v * scale)) for k, v in S3_PROFILE.items())


SCENARIOS = {
    "S0": dict(desc="cpu mode", pci=[], append="", conf=""),
    "S1": dict(desc="gpu defaults, latency 0", pci=["gpu1"], append="", conf=""),
    "S2": dict(desc="gpu + smi lmc/lgc/pl/srs + dcgm=on, latency 0", pci=["gpu1"], append=SMI, conf=""),
    "S3": dict(desc="S2 with SIMULATED latency profile x1", pci=["gpu1"], append=SMI, conf=profile_text(1)),
    "S4": dict(desc="servicevm-nvl4 (4 NVSwitch, fabric manager), latency 0", pci=["nvl4"], append="", conf=""),
}
for sc in (0.25, 0.5, 2):
    SCENARIOS["S3x%g" % sc] = dict(desc="S2 with SIMULATED latency profile x%g" % sc, pci=["gpu1"], append=SMI,
                                   conf=profile_text(sc))


def cpu_busy(sample=0.3):
    """Host CPUs busy (sum over all CPUs, in CPU units) during a short sample, from /proc/stat."""
    def read():
        with open("/proc/stat") as f:
            v = [int(x) for x in f.readline().split()[1:]]
        return sum(v) - v[3] - v[4], sum(v)  # busy (not idle/iowait), total
    b0, t0 = read()
    time.sleep(sample)
    b1, t1 = read()
    return (b1 - b0) / max(1, t1 - t0) * os.cpu_count()


def wait_quiet(limit, max_wait=90):
    """The host is shared with other jobs. Before each round wait (bounded) until the others use fewer than
    `limit` CPUs; returns the last measured busy value so it is stored with the run."""
    t0 = time.monotonic()
    busy = cpu_busy()
    while busy > limit and time.monotonic() - t0 < max_wait:
        time.sleep(1.0)
        busy = cpu_busy()
    return busy


def pinned_busy(cpus, sample=0.2):
    """Busy fraction (0..len(cpus)) of the given host CPUs during a short sample."""
    def read():
        out = {}
        with open("/proc/stat") as f:
            for ln in f:
                if ln.startswith("cpu") and ln[3].isdigit():
                    v = [int(x) for x in ln.split()[1:]]
                    out[int(ln.split()[0][3:])] = (sum(v) - v[3] - v[4], sum(v))
        return out
    a = read()
    time.sleep(sample)
    b = read()
    return sum((b[c][0] - a[c][0]) / max(1, b[c][1] - a[c][1]) for c in cpus)


def stat_snapshot():
    out = {}
    with open("/proc/stat") as f:
        for ln in f:
            if ln.startswith("cpu") and ln[3].isdigit():
                v = [int(x) for x in ln.split()[1:]]
                out[int(ln.split()[0][3:])] = (sum(v) - v[3] - v[4], sum(v))
    return out


def others_busy(s0, s1, pinned):
    """Average busy CPUs of the host CPUs that no QEMU is pinned to, between two snapshots (the QEMU guests of the
    round run only on the pinned CPUs, so everything seen elsewhere is other load plus our own small driver)."""
    cpus = [c for c in s0 if c not in pinned] or list(s0)
    return sum((s1[c][0] - s0[c][0]) / max(1, s1[c][1] - s0[c][1]) for c in cpus)


def parse_cpus(spec):
    out = []
    for part in spec.split(","):
        lo, _, hi = part.partition("-")
        out += range(int(lo), int(hi or lo) + 1)
    return out


def host_state():
    """Load and memory of the host, stored with every run."""
    mi = {}
    with open("/proc/meminfo") as f:
        for ln in f:
            k, v = ln.split(":")
            mi[k] = int(v.split()[0])
    l1, l5, l15 = os.getloadavg()
    return {"load1": l1, "load5": l5, "load15": l15, "mem_free_mb": mi["MemFree"] // 1024,
            "mem_avail_mb": mi["MemAvailable"] // 1024, "swap_used_mb": (mi["SwapTotal"] - mi["SwapFree"]) // 1024}


def mem_pressure():
    """Host-wide memory stall counters, sampled around every QEMU run. A guest that has to fault in 512 MB while the
    host is short of free memory (direct reclaim, swap) stalls inside the kernel boot for 0.5-4 s whatever the init is."""
    out = {"psi_some_us": 0, "psi_full_us": 0}
    try:
        with open("/proc/pressure/memory") as f:
            for ln in f:
                kind, *rest = ln.split()
                out["psi_%s_us" % kind] = int(dict(x.split("=") for x in rest)["total"])
    except OSError:
        pass
    with open("/proc/vmstat") as f:
        for ln in f:
            k, v = ln.split()
            if k.startswith("allocstall"):
                out["allocstall"] = out.get("allocstall", 0) + int(v)
            elif k in ("pswpin", "pswpout", "pgscan_direct"):
                out[k] = int(v)
    return out


def mem_pressure_delta(a, b):
    d = {k: b[k] - a[k] for k in a if k in b}
    return {"mem_stall_ms": round(d["psi_some_us"] / 1000.0, 1), "mem_stall_full_ms": round(d["psi_full_us"] / 1000.0, 1),
            "swap_pages": d.get("pswpin", 0) + d.get("pswpout", 0), "direct_reclaim_pages": d.get("pgscan_direct", 0),
            "allocstalls": d.get("allocstall", 0)}


def metrics(rep, image_bytes):
    """One run's numbers. Memory/CPU come from the stub's report at the moment kata-agent starts."""
    k = rep.kata
    if not k or not rep.complete:
        return None
    p1 = rep.procs.get("at-start", [])
    # PID 1 is systemd for nvsystemd; NVRC execs kata-agent into PID 1, so NVRC's own process is its syslog
    # poller child (a fork of NVRC taken just before the exec, exe /init, pid != 1). For the calibration images the
    # init is the stub itself (pid 1).
    def init_proc(plist):
        return next((p for p in plist if p["comm"] == "systemd" or (p["exe"] == "/init" and p["pid"] != "1")),
                    next((p for p in plist if p["pid"] == "1"), {}))
    pid1 = init_proc(p1)
    mem = rep.mem.get("at-start", {})
    cen = rep.census.get("at-start", {})
    cen2 = rep.census.get("after-grace", {})
    mem2 = rep.mem.get("after-grace", {})
    pid1b = init_proc(rep.procs.get("after-grace", []))
    init_ts = k["init_ts_us"]
    return {
        "kata_start_us": k["start_boottime_us"],               # CLOCK_BOOTTIME at kata-agent stub start
        "kernel_us": init_ts,                                 # kernel time: printk stamp of "Run /init as init process"
        "user_us": k["start_boottime_us"] - init_ts,          # init-to-kata-agent (userspace) time
        "host_wall_to_kata_s": rep.kata_host_t,              # host monotonic from QEMU spawn to the stub's first report line
        "pid1_comm": k["pid1_comm"], "init_comm": pid1.get("comm"),
        "pid1_run_ns": k["pid1_run_ns"],                      # schedstat run time of PID 1 up to kata start
        "pid1_cpu_ticks": k["pid1_utime_ticks"] + k["pid1_stime_ticks"],
        "pid1_children_cpu_ticks": k["pid1_cutime_ticks"] + k["pid1_cstime_ticks"],
        "pid1_rss_kb": int(pid1.get("rss_kb", 0)), "pid1_pss_kb": int(pid1.get("pss_kb", 0)),
        "pid1_rss_kb_late": int(pid1b.get("rss_kb", 0)),
        "mem_used_kb": mem.get("used_kb"), "mem_used_kb_late": mem2.get("used_kb"),
        "mem_total_kb": mem.get("total_kb"),
        "procs": cen.get("user_procs"), "threads": cen.get("user_threads"),
        "procs_late": cen2.get("user_procs"), "threads_late": cen2.get("user_threads"),
        "kthreads": cen.get("kthreads"),
        "image_bytes": image_bytes, "qemu_wall_s": rep.wall,
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--scenarios", default="S0,S1,S2,S3")
    ap.add_argument("--runs", type=int, default=30)
    ap.add_argument("--variants", default="nvrc,nvsd,nvsd-centos10")
    ap.add_argument("--label", default="run")
    ap.add_argument("--compress", default="zstd")
    ap.add_argument("--append", default="", help="extra kernel command line items given to BOTH variants")
    ap.add_argument("--warmup", type=int, default=2)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--busy-limit", type=float, default=2.5, help="other host load (busy CPUs: sampled before a round, and over the round on the CPUs QEMU is not pinned to) above this discards the round")
    ap.add_argument("--pinned-limit", type=float, default=0.5, help="busy fraction of the pinned cores (sum) above this before a round discards it")
    ap.add_argument("--cpus", default=os.environ.get("NVSD_CPUS", ""), help="pin every QEMU to these host CPUs (taskset), e.g. 12-15")
    ap.add_argument("--mem-stall-limit", type=float, default=0.0, help="host memory pressure (PSI 'some' stall, ms, during any one QEMU run of the round) above this discards the round; 0 = record only")
    ap.add_argument("--max-attempts", type=int, default=8, help="attempts per round before the round is kept flagged noisy")
    a = ap.parse_args()
    # "name" or "alias=spec" (spec like nvsd@/dir for an experimental overlay directory, or nvsd-probe)
    specs = {}
    cmdlines = {}
    comps = {}
    for item in a.variants.split(","):
        alias, _, spec = item.partition("=")
        spec = spec or alias
        spec, _, comp = spec.partition("~")   # "nvsd~xz": initramfs compression for this variant only
        comps[alias] = comp
        spec, _, extra_cmdline = spec.partition("#")  # "nvsd#selinux=0": extra kernel command line for this variant only
        specs[alias] = spec
        cmdlines[alias] = extra_cmdline
    variants = list(specs)
    if a.cpus:
        os.environ["NVSD_CPUS"] = a.cpus
    pinned = parse_cpus(a.cpus) if a.cpus else []
    L.build_tools()
    os.makedirs(os.path.join(WORK, "bench", "raw"), exist_ok=True)
    raw = os.path.join(WORK, "bench", "raw", a.label + ".jsonl")
    discarded = os.path.join(WORK, "bench", "raw", a.label + ".discarded.jsonl")
    done = set()  # (scenario, round) already complete in the raw file (resume)
    if os.path.exists(raw):
        have = {}
        with open(raw) as f:
            for ln in f:
                if ln.strip():
                    r = json.loads(ln)
                    have.setdefault((r["scenario"], r["round"]), set()).add(r["variant"])
        done = {k for k, vs in have.items() if vs >= set(variants)}
    perms = list(itertools.permutations(variants))
    for name in a.scenarios.split(","):
        sc = SCENARIOS[name]
        if all((name, r) in done for r in range(a.runs)):
            print("== %s: complete, skipped" % name, flush=True)
            continue
        images = {}
        for v in variants:
            images[v] = L.build_image(specs[v], sc["conf"], "bench-" + name + "-" + v.replace("/", "_"), comps[v] or a.compress)
        sizes = {v: os.path.getsize(images[v]) for v in variants}
        print("== %s: %s  images=%s  cpus=%s" % (name, sc["desc"], sizes, a.cpus or "unpinned"), flush=True)
        warm = a.warmup
        for r in range(-warm, a.runs):
            if r >= 0 and (name, r) in done:
                continue
            order = list(perms[(r + warm) % len(perms)])  # all permutations in turn: position bias cancels out
            for attempt in range(a.max_attempts):
                busy0 = wait_quiet(a.busy_limit, max_wait=20)
                pb0 = pinned_busy(pinned) if pinned else 0.0
                st0 = host_state()
                snap0 = stat_snapshot()
                recs = []
                for v in order:
                    mp0 = mem_pressure()
                    rep = L.boot(specs[v], pci=sc["pci"], append=(sc["append"] + " " + a.append + " " + cmdlines[v]).strip(), image=images[v], timeout=60)
                    mpd = mem_pressure_delta(mp0, mem_pressure())
                    m = metrics(rep, sizes[v])
                    rec = {"label": a.label, "scenario": name, "variant": v, "round": r, "ok": m is not None,
                           "ts": time.time(), "host_busy_cpus": round(busy0, 2), "pinned_busy": round(pb0, 3),
                           "attempt": attempt, "order": order, "host_before": st0}
                    rec.update(mpd)
                    if m:
                        rec.update(m)
                    else:
                        rec["tail"] = rep.text[-600:]
                    recs.append(rec)
                busy1 = others_busy(snap0, stat_snapshot(), pinned)  # over the whole round, outside the QEMU cores
                st1 = host_state()
                stall = max(rec["mem_stall_ms"] for rec in recs)
                quiet = busy0 <= a.busy_limit and busy1 <= a.busy_limit and pb0 <= a.pinned_limit and (
                    a.mem_stall_limit <= 0 or stall <= a.mem_stall_limit)
                for rec in recs:
                    rec["host_busy_cpus_during"] = round(busy1, 2)
                    rec["host_after"] = st1
                    rec["noisy"] = not quiet
                if r < 0:
                    break  # warm-up: page cache, KVM, qemu binary
                if quiet or attempt == a.max_attempts - 1:
                    with open(raw, "a") as f:
                        for rec in recs:
                            f.write(json.dumps(rec) + "\n")
                    break
                with open(discarded, "a") as f:
                    for rec in recs:
                        f.write(json.dumps(rec) + "\n")
                print("   %s round %d attempt %d discarded (busy before %.1f during %.1f pinned %.2f, load1 %.1f, mem stall %.0f ms)" % (
                    name, r, attempt, busy0, busy1, pb0, st1["load1"], stall), flush=True)
                time.sleep(5)
            if r >= 0 and (r + 1) % 10 == 0:
                print("   %s round %d/%d  load1=%.1f" % (name, r + 1, a.runs, os.getloadavg()[0]), flush=True)


if __name__ == "__main__":
    main()
