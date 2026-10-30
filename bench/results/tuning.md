# Tuning experiments (paired, interleaved, SIMULATED fixture)

Each experiment boots the variants round-robin in the same rounds on the same QEMU configuration; the table shows medians of the
init-to-kata-agent time (`user_us`, i.e. from "Run /init as init process" to the kata-agent stub start) and the median of the
per-round difference to the first variant ("paired diff", robust to bursts of load from other jobs on this shared host).
Raw lines: /home/ecurtin/nvsd-work/bench/raw/<label>.jsonl (labels exp3..exp8, comp1).
The overlay directories used for the variants were throwaway files under /home/ecurtin/nvsd-work/exp/ (described below), the committed
overlay/ contains only what was kept.

## exp3 (S2: gpu + smi + dcgm, latency 0): what two process starts and two slices cost
- `nomount`: tmp.mount and sys-kernel-config.mount skipped by a failing condition (lower bound for folding the two mounts into the init helper; kept: folded into nvsd-init.service)
- `noslice`: `Slice=system.slice` for modprobe@ and nvsd-extension@ (kept)
```
S2 (user_us, us->ms), base=nvsd
  nvsd         median    82.21  p25    77.94  min    71.45 | paired diff median   +0.00 (n=25)
  nomount      median    77.63  p25    71.66  min    62.30 | paired diff median   -4.84 (n=25)
  noslice      median    80.53  p25    76.84  min    69.67 | paired diff median   -2.07 (n=25)
```

## exp4: system.conf (accounting off, DefaultStandardOutput=null, LogLevel=err, ShowStatus=no): no effect, not adopted
```
S0 (user_us, us->ms), base=nvsd
  nvsd         median    36.49  p25    35.34  min    33.34 | paired diff median   +0.00 (n=30)
  sysconf      median    44.57  p25    41.02  min    33.22 | paired diff median   +8.22 (n=30)
S2 (user_us, us->ms), base=nvsd
  nvsd         median    72.04  p25    67.18  min    63.45 | paired diff median   +0.00 (n=30)
  sysconf      median    73.66  p25    66.23  min    62.12 | paired diff median   +0.16 (n=30)
```

## exp5: kernel command line items given to nvsystemd only (NVRC would ignore them): selinux=0; systemd.log_level=err; all of selinux=0 audit=0 systemd.log_level=err systemd.show_status=0
The S2 part of this run was cut short by the 290 s timeout (n=8-9 per variant); differences are within the noise: not adopted.
```
S0 (user_us, us->ms), base=nvsd
  nvsd         median    36.50  p25    35.73  min    34.16 | paired diff median   +0.00 (n=30)
  sel          median    36.85  p25    34.82  min    28.45 | paired diff median   -0.62 (n=30)
  lvl          median    37.98  p25    35.64  min    33.71 | paired diff median   +1.19 (n=30)
  sel_aud_lvl  median    36.15  p25    34.98  min    30.27 | paired diff median   -0.28 (n=30)
S2 (user_us, us->ms), base=nvsd
  nvsd         median    73.08  p25    67.24  min    64.66 | paired diff median   +0.00 (n=9)
  sel          median    69.61  p25    67.24  min    57.01 | paired diff median   -2.46 (n=9)
  lvl          median    75.24  p25    68.24  min    64.41 | paired diff median   -1.30 (n=8)
  sel_aud_lvl  median    72.96  p25    68.14  min    65.08 | paired diff median   -5.31 (n=8)
```

## exp6/exp7/exp8 (S0 cpu mode): how much of the init-to-agent time is the systemd floor
- `floor`: boot target requires only kata-agent.service (one unit, one process start)
- `slim`: nvsd-init.service + syslog socket + kata-agent only (no extension, mode, lockdown targets, no gpu units)
- `cpuonly`: full units for the cpu path (init, cpu target, lockdown, kata-agent) but the gpu/nvl units are not reachable
- `noext`: the full overlay without the extension chain
```
S0 (user_us, us->ms), base=nvsd
  nvsd         median    37.79  p25    36.06  min    34.26 | paired diff median   +0.00 (n=30)
  floor        median    18.43  p25    17.79  min    14.82 | paired diff median  -19.78 (n=30)
S0 (user_us, us->ms), base=nvsd
  nvsd         median    37.38  p25    35.89  min    33.19 | paired diff median   +0.00 (n=30)
  noext        median    34.94  p25    34.11  min    30.22 | paired diff median   -2.18 (n=30)
  cpuonly      median    23.72  p25    22.67  min    19.57 | paired diff median  -12.60 (n=30)
  slim         median    23.50  p25    22.46  min    19.58 | paired diff median  -13.60 (n=30)
```

Reading: stock systemd needs about 18 ms from the kernel's "Run /init" to start one service; a minimal 3-unit boot 24 ms; the full
overlay (about 40 units reachable from the boot target, because one image must serve all four modes and systemd builds the transaction
before it knows the mode) costs about 13 ms more in cpu mode than the cpu-only subset. A dispatch design (path units that start a per-mode
target after nvsd-init has detected the mode) would recover most of that for cpu mode and little for gpu modes; it was not built because
the Requires= chain that keeps kata-agent from starting after a failure cannot cross two transactions without per-mode template units.

## comp1 (S0): initramfs compression, same kernel, same image contents
Sizes (bytes): none 22772308, lz4 11025610, gzip 9630115, zstd -19 8145378, xz 7546488. Metric: time to kata-agent start (ms, kernel included).
```
S0 (kata_start_us, us->ms), base=sd_zstd
  nvrc         median   605.40  p25   604.96  min   602.28 | paired diff median  -50.02 (n=25)
  sd_none      median   657.06  p25   651.13  min   647.21 | paired diff median   +2.54 (n=25)
  sd_zstd      median   654.02  p25   649.37  min   642.56 | paired diff median   +0.00 (n=25)
  sd_gzip      median   654.98  p25   650.11  min   641.40 | paired diff median   -1.08 (n=25)
  sd_xz        median   875.03  p25   869.41  min   862.73 | paired diff median +222.81 (n=25)
  sd_lz4       median   658.20  p25   650.76  min   649.30 | paired diff median   +1.68 (n=25)
```
xz decompression is slow (+223 ms); zstd -19, gzip, lz4 and uncompressed are within 3 ms of each other. zstd -19 is used for both inits
in all headline runs (it makes the nvsystemd image smallest among the fast codecs; NVRC's 259 KB image is insensitive).
