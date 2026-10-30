#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Benchmark driver: NVRC (unmodified, release profile, static musl) vs nvsystemd, interleaved A/B.

  bench/run.py --scenarios S0,S1,S2,S3 --runs 30 [--variants nvrc,nvsd] [--label baseline]

Everything is SIMULATED hardware (fixture/): no GPU exists on this host. Per run it boots one image in
QEMU/KVM (same kernel, same flags, same command line for both inits) and parses the report that the
kata-agent stub prints. Raw per-run JSON lines go to $NVSD_WORK/bench/raw/<label>.jsonl (not committed);
bench/summarize.py turns them into the CSV/markdown that is committed.
"""
import argparse
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


def cpu_busy(sample=0.1):
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
    ap.add_argument("--variants", default="nvrc,nvsd")
    ap.add_argument("--label", default="run")
    ap.add_argument("--compress", default="zstd")
    ap.add_argument("--append", default="", help="extra kernel command line items given to BOTH variants")
    ap.add_argument("--warmup", type=int, default=2)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--busy-limit", type=float, default=2.5, help="wait (up to 90 s per round) while other host load exceeds this many CPUs")
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
    L.build_tools()
    os.makedirs(os.path.join(WORK, "bench", "raw"), exist_ok=True)
    raw = os.path.join(WORK, "bench", "raw", a.label + ".jsonl")
    rnd = random.Random(a.seed)
    for name in a.scenarios.split(","):
        sc = SCENARIOS[name]
        images = {}
        for v in variants:
            images[v] = L.build_image(specs[v], sc["conf"], "bench-" + name + "-" + v.replace("/", "_"), comps[v] or a.compress)
        sizes = {v: os.path.getsize(images[v]) for v in variants}
        print("== %s: %s  images=%s" % (name, sc["desc"], sizes), flush=True)
        for r in range(-a.warmup, a.runs):
            order = list(variants)
            if r % 2:  # interleave, alternating who goes first
                order.reverse()
            busy = wait_quiet(a.busy_limit)
            for v in order:
                rep = L.boot(specs[v], pci=sc["pci"], append=(sc["append"] + " " + a.append + " " + cmdlines[v]).strip(), image=images[v], timeout=60)
                m = metrics(rep, sizes[v])
                if r < 0:
                    continue  # warm-up: page cache, KVM, qemu binary
                rec = {"label": a.label, "scenario": name, "variant": v, "round": r, "ok": m is not None,
                       "ts": time.time(), "host_busy_cpus": round(busy, 2), "loadavg1": os.getloadavg()[0]}
                if m:
                    rec.update(m)
                else:
                    rec["tail"] = rep.text[-600:]
                with open(raw, "a") as f:
                    f.write(json.dumps(rec) + "\n")
            if r >= 0 and (r + 1) % 10 == 0:
                print("   %s round %d/%d" % (name, r + 1, a.runs), flush=True)


if __name__ == "__main__":
    main()
