# Benchmark: NVRC vs nvsystemd

**Everything below is SIMULATED hardware.** There is no GPU on the benchmark host. The NVIDIA tools are stubs
(`fixture/stub/nvstub.c`) that validate their argument lists, sleep for a configurable latency (default 0) and log
timestamps. The numbers measure init-system behaviour, not GPU driver bring-up. Real `modprobe nvidia`, `nvidia-smi`
and CDI generation take far longer than anything measured here, which only makes the absolute share of the init
system smaller.

## What was compared

Three columns, in every table: **nvrc** | **nvsd** (nvsystemd on Fedora's systemd) | **nvsd-centos10** (the same
nvsystemd on CentOS Stream 10's systemd).

| | NVRC | nvsystemd, Fedora base ("nvsd") | nvsystemd, CentOS 10 base ("nvsd-centos10") |
|---|---|---|---|
| PID 1 | NVRC `b9700f3` (v0.1.7), unmodified, NVRC's own `[profile.release]`, `x86_64-unknown-linux-musl`, static, built by `scripts/build-nvrc.sh` | Fedora `systemd` 259.10-1.fc44, unmodified, plus its `ldd` libraries (glibc 2.43-9.fc44) copied from the host | CentOS Stream 10 `systemd` 257-35.el10, unmodified, with its own glibc 2.39 and libraries from `scripts/fetch-centos10-systemd.sh` (no file shared with the Fedora base) |
| rest of the image | identical fixture files, stub binaries (byte for byte the same file, hard-linked under the tool names), `kata-agent` stub | same, plus `overlay/` (units) and `nvsd-helper` | same overlay and helper, byte for byte (no `overlay-centos10/` was needed) |
| machine | the same kernel (`/lib/modules/7.2.13-200.fc44.x86_64/vmlinuz`, `-kernel`), QEMU/KVM q35, `-cpu host`, 2 vCPU, 512 MB, serial console, `-no-reboot`; QEMU pinned with `taskset -c 12-15` | same | same |
| kernel command line | `console=ttyS0 quiet panic=-1 8250.nr_uarts=1 random.trust_cpu=on TERM=dumb tsc=reliable no_timer_check log_buf_len=1M` plus the scenario's `nvrc.*` items | same, byte for byte | same, byte for byte |
| initramfs | cpio (newc), zstd -19 | same codec | same codec |
| simulated PCI | `fixture/fakepci/fakepci.py` over vfio-user: 1 GPU `10de:2330` class 0302 (S1-S3), 4 NVSwitch `10de:22a3` class 0680 (S4) | same | same |

## Method

* `bench/run.py`: for every scenario it builds the three images, runs 2 warm-up rounds, then **30 measured rounds per scenario**. In
  every round all three variants boot once, **interleaved**, in a different permutation each round (all 6 orders in turn) so that
  position effects and drift hit every column equally. Each QEMU run is wrapped in `timeout -s KILL 60` and **pinned with `taskset -c 12-15`**
  (two physical cores) so the guest never competes with the benchmark driver or with other jobs for its cores.
* **Host noise gate.** The host is shared (another agent session, `updatedb`, a busy `kswapd0`; load average reached about 46 during
  an earlier, discarded attempt). A round is accepted only if (a) before it the other host load is at most 1.0 busy CPUs and the four
  pinned cores are at most 0.3 busy, (b) averaged over the round, the CPUs QEMU is not pinned to are at most 1.0 busy, and (c) the
  host-wide memory-pressure counter (`/proc/pressure/memory`, "some" stall) rose by at most 50 ms during every one of the round's QEMU
  runs. Otherwise the whole round is discarded (kept in `<label>.discarded.jsonl`) and redone, up to 120 attempts, after which it would be
  kept and flagged `noisy` (this never happened). Every record stores load average, free/available memory, swap, the busy figures, the
  memory stall (ms), allocation stalls, swapped pages and direct-reclaim pages. The summary line under the tables counts discarded and
  noisy rounds.
* **Driver guard.** After every round `bench/run.py` checks its own process: more than 64 open file descriptors, or more than 4 GiB of
  system `Shmem`, aborts the run with an error (exit 3, the round is not written). A read-only watcher (`memwatch.py`, outside the repo)
  additionally logged shared memory, available memory, swap and the number of QEMU processes every 30 s during the run. The boot test
  `t40` boots GPU and NVSwitch guests four times in one process and asserts that no memfd or other fd accumulates; it fails on the
  version of `fakepci.py` that had the leak described under "Threats to validity", and passes on the fixed one.
* The end marker is the `kata-agent` stub. It is the same file for every init. It records `CLOCK_BOOTTIME` the instant it starts, then
  prints a report (memory, process census, per-process RSS/PSS, PID 1 CPU time, mounts, the stub event log) on the serial console
  and powers the VM off. A run without a complete report is counted as failed and excluded.
* **Definition of "time to kata-agent start"**: `CLOCK_BOOTTIME` in the kata-agent stub at its first instruction. It includes
  kernel boot and initramfs unpack. It is reported together with its two parts: *kernel time* = the kernel's own timestamp of
  `Run /init as init process`, and *init to kata-agent* = the difference (userspace). QEMU start-up time on the host is not included.
* Statistics per cell: median, p95 (nearest rank), min-max, sample standard deviation. A *paired diff* is the median over rounds of the
  difference of two columns measured in the same round.
* Memory is `MemTotal - MemAvailable` at kata-agent start (the unpacked initramfs counts: it lives in RAM). Init RSS/PSS comes from
  `/proc/<pid>/smaps_rollup`. NVRC execs kata-agent into PID 1, so NVRC's own process is the syslog-poller child it forks just before
  the exec (same address space snapshot): that is the NVRC figure.
* Init CPU time: PID 1's `schedstat` run time when kata-agent starts. PID 1 also carries the kernel's own init thread (initramfs
  unpack, initcalls, about 200 ms), so a **calibration image** per variant (same contents, `/init` = the stub) is booted 30 times and
  its median PID 1 run time is subtracted. The residual is the init's own CPU time.
* Simulated latency (S3, labelled SIMULATED): `modprobe nvidia` 300 ms, `nvidia-persistenced` 50 ms (to readiness), each `nvidia-smi` 100 ms,
  `nv-hostengine` 80 ms, `nvidia-ctk cdi generate` 150 ms (all in `bench/run.py`, `S3_PROFILE`; scaled copies x0.25, x0.5 and x2 form the sweep).

## Scenarios

| id | what | PCI | kernel parameters |
|---|---|---|---|
| S0 | cpu mode | none | none |
| S1 | gpu mode, defaults, latency 0 | 1 GPU | none |
| S2 | gpu mode, all `nvidia-smi` parameters and dcgm, latency 0 | 1 GPU | `nvrc.smi.lmc=5001 nvrc.smi.lgc=1500 nvrc.smi.pl=300 nvrc.smi.srs=1 nvrc.dcgm=on` |
| S3 | S2 with the SIMULATED latency profile | 1 GPU | same |
| S4 | servicevm-nvl4 (fabric manager), latency 0 (extra; **nvl5 is not feasible**: NVRC needs a real `/sys/class/infiniband`, which a read-only sysfs cannot fake, so only nvsystemd can run the nvl5 flow, against a test-only fake tree, test u12) | 4 NVSwitch | none |

## Results

<!-- TABLES:final6 -->
Cells: median / p95 / min-max / sample stdev, in ms. Paired columns: median over rounds of the difference measured in the same round (negative = first-named is faster).

#### Time to kata-agent stub start, CLOCK_BOOTTIME, ms

| scenario | n | NVRC | nvsystemd (Fedora systemd) | nvsystemd (CentOS 10 systemd) | paired nvsd-Fedora - NVRC | paired nvsd-CentOS10 - NVRC | paired nvsd-CentOS10 - nvsd-Fedora |
|---|---|---|---|---|---|---|---|
| S0 cpu | 30 | 605.7 / 619.2 / 593.6-619.7 / 6.2 | 655.3 / 672.8 / 645.7-674.7 / 7.9 | 655.4 / 674.5 / 641.1-674.9 / 9.3 | +49.5 | +48.3 | +1.7 |
| S1 gpu defaults | 30 | 614.8 / 622.9 / 605.0-623.0 / 4.8 | 682.4 / 696.8 / 673.8-700.6 / 7.8 | 688.1 / 705.3 / 672.9-713.8 / 9.5 | +66.9 | +74.3 | +6.0 |
| S2 gpu + smi x4 + dcgm | 30 | 614.3 / 624.1 / 605.9-625.1 / 5.6 | 696.1 / 704.5 / 670.2-704.6 / 9.1 | 700.7 / 721.7 / 683.8-724.9 / 10.6 | +81.8 | +85.4 | +4.0 |
| S3 = S2 + SIMULATED latency x1 | 30 | 1518.1 / 1526.6 / 1504.2-1528.3 / 5.4 | 1256.3 / 1265.5 / 1243.3-1269.3 / 6.9 | 1267.2 / 1283.7 / 1241.0-1294.5 / 11.5 | -260.9 | -251.6 | +9.7 |
| S4 servicevm-nvl4 | 30 | 623.3 / 631.4 / 614.0-631.6 / 4.9 | 689.3 / 702.7 / 676.8-702.8 / 7.3 | 693.4 / 708.4 / 671.4-709.9 / 7.9 | +66.8 | +69.9 | +2.3 |
| S3 latency x0.25 | 30 | 843.4 / 852.1 / 834.4-852.6 / 5.2 | 845.4 / 868.5 / 823.2-869.2 / 11.3 | 847.5 / 867.4 / 829.4-873.6 / 9.4 | +3.0 | +2.9 | +0.9 |
| S3 latency x0.5 | 30 | 1069.3 / 1078.1 / 1060.2-1080.9 / 5.2 | 983.1 / 1000.9 / 969.4-1007.4 / 8.5 | 993.8 / 1001.4 / 972.3-1003.3 / 8.0 | -84.9 | -77.0 | +8.9 |
| S3 latency x2 | 30 | 2416.4 / 2426.4 / 2410.2-2431.6 / 5.3 | 1807.3 / 1827.5 / 1796.3-1829.3 / 9.8 | 1813.7 / 1837.5 / 1799.2-1838.7 / 10.5 | -605.8 | -602.7 | +5.7 |

#### Of which: kernel time before /init, ms (same kernel; the difference is the initramfs size)

| scenario | n | NVRC | nvsystemd (Fedora systemd) | nvsystemd (CentOS 10 systemd) | paired nvsd-Fedora - NVRC | paired nvsd-CentOS10 - NVRC | paired nvsd-CentOS10 - nvsd-Fedora |
|---|---|---|---|---|---|---|---|
| S0 cpu | 30 | 605.3 / 618.9 / 593.2-619.4 / 6.2 | 618.6 / 636.1 / 607.6-636.5 / 7.4 | 616.4 / 637.3 / 602.5-638.9 / 9.5 | +13.7 | +9.8 | +1.8 |
| S1 gpu defaults | 30 | 612.7 / 620.6 / 602.8-620.8 / 4.8 | 620.5 / 630.8 / 612.1-634.7 / 6.3 | 623.2 / 635.9 / 611.9-646.8 / 7.8 | +7.5 | +11.0 | +5.1 |
| S2 gpu + smi x4 + dcgm | 30 | 611.3 / 621.2 / 602.7-621.7 / 5.6 | 622.2 / 633.7 / 606.1-636.3 / 6.9 | 622.4 / 637.8 / 612.1-638.8 / 8.6 | +12.1 | +10.1 | +1.0 |
| S3 = S2 + SIMULATED latency x1 | 30 | 610.8 / 619.7 / 596.0-620.9 / 5.5 | 620.9 / 630.1 / 611.7-631.2 / 5.6 | 622.8 / 638.2 / 611.2-647.8 / 9.2 | +12.2 | +16.3 | +3.5 |
| S4 servicevm-nvl4 | 30 | 621.4 / 629.5 / 611.7-629.7 / 4.9 | 633.3 / 647.1 / 624.5-647.7 / 6.2 | 634.4 / 644.6 / 622.5-652.1 / 6.4 | +11.1 | +15.7 | +0.3 |
| S3 latency x0.25 | 30 | 611.9 / 620.7 / 603.1-621.1 / 5.3 | 621.3 / 635.4 / 605.9-637.0 / 7.4 | 619.4 / 636.9 / 611.7-645.0 / 9.0 | +9.9 | +7.8 | -2.4 |
| S3 latency x0.5 | 30 | 612.4 / 621.3 / 603.1-624.2 / 5.3 | 621.7 / 637.5 / 611.8-645.4 / 7.5 | 628.7 / 637.7 / 612.7-639.1 / 6.8 | +9.2 | +16.7 | +6.3 |
| S3 latency x2 | 30 | 608.6 / 619.0 / 603.0-622.8 / 5.3 | 620.6 / 638.3 / 610.8-638.9 / 7.9 | 622.7 / 641.1 / 611.9-647.4 / 9.0 | +11.4 | +15.9 | +2.4 |

#### Of which: init to kata-agent start (userspace), ms

| scenario | n | NVRC | nvsystemd (Fedora systemd) | nvsystemd (CentOS 10 systemd) | paired nvsd-Fedora - NVRC | paired nvsd-CentOS10 - NVRC | paired nvsd-CentOS10 - nvsd-Fedora |
|---|---|---|---|---|---|---|---|
| S0 cpu | 30 | 0.3 / 0.5 / 0.1-0.6 / 0.1 | 37.4 / 43.2 / 33.1-43.8 / 2.6 | 37.0 / 39.9 / 31.5-43.2 / 2.3 | +37.0 | +36.7 | -0.1 |
| S1 gpu defaults | 30 | 2.1 / 2.5 / 1.4-2.6 / 0.3 | 62.4 / 73.3 / 56.4-78.8 / 5.5 | 63.6 / 76.4 / 48.7-86.7 / 7.5 | +60.3 | +61.8 | +2.0 |
| S2 gpu + smi x4 + dcgm | 30 | 3.0 / 3.4 / 2.5-3.5 / 0.3 | 71.9 / 81.7 / 58.3-83.0 / 6.5 | 76.5 / 87.0 / 68.2-90.9 / 6.1 | +68.9 | +73.7 | +4.5 |
| S3 = S2 + SIMULATED latency x1 | 30 | 907.4 / 908.9 / 906.5-909.0 / 0.7 | 635.8 / 643.7 / 621.5-644.5 / 5.5 | 641.7 / 653.2 / 627.7-663.7 / 7.0 | -271.5 | -265.5 | +6.9 |
| S4 servicevm-nvl4 | 30 | 1.9 / 2.3 / 1.5-2.5 / 0.2 | 53.3 / 63.6 / 48.0-65.5 / 4.6 | 56.9 / 66.4 / 49.0-71.7 / 5.0 | +51.6 | +55.2 | +2.2 |
| S3 latency x0.25 | 30 | 231.9 / 232.9 / 231.0-233.2 / 0.6 | 223.8 / 238.7 / 212.4-247.1 / 8.0 | 226.7 / 234.4 / 215.4-236.6 / 5.5 | -8.4 | -5.3 | +3.4 |
| S3 latency x0.5 | 30 | 457.0 / 458.1 / 456.2-459.1 / 0.6 | 361.2 / 370.9 / 347.9-375.6 / 5.4 | 364.5 / 373.2 / 357.0-374.3 / 4.7 | -95.7 | -92.6 | +3.1 |
| S3 latency x2 | 30 | 1807.7 / 1808.7 / 1806.9-1809.2 / 0.5 | 1188.1 / 1198.2 / 1178.3-1201.9 / 5.9 | 1189.4 / 1199.7 / 1178.4-1199.8 / 5.0 | -620.2 | -617.8 | +1.6 |

#### Resources at kata-agent start (S2 gpu + smi x4 + dcgm), median / p95 / min-max / stdev

| metric | NVRC | nvsystemd (Fedora systemd) | nvsystemd (CentOS 10 systemd) |
|---|---|---|---|
| initramfs, compressed (zstd -19), KiB | 259 / 259 / 259-259 / 0 | 7955 / 7955 / 7955-7955 / 0 | 7410 / 7410 / 7410-7410 / 0 |
| init RSS, KiB (NVRC: its syslog-poller child, the process image NVRC keeps after exec) | 276 / 320 / 236-328 / 28 | 11766 / 11916 / 11568-11924 / 115 | 11060 / 11224 / 10912-11228 / 111 |
| init PSS, KiB | 276 / 320 / 236-328 / 28 | 11766 / 11916 / 11568-11924 / 115 | 11060 / 11224 / 10912-11228 / 111 |
| memory used = MemTotal - MemAvailable, KiB | 49670 / 52348 / 43504-52588 / 2494 | 84338 / 88944 / 78220-89560 / 3015 | 83882 / 87448 / 78148-87780 / 2135 |
| memory used 200 ms later, KiB | 49788 / 52408 / 43532-52620 / 2489 | 84338 / 88944 / 78220-89560 / 3019 | 83882 / 87500 / 78148-87780 / 2136 |
| user processes | 5 / 5 / 5-5 / 0 | 6 / 6 / 6-6 / 0 | 6 / 6 / 6-6 / 0 |
| user threads | 5 / 5 / 5-5 / 0 | 6 / 6 / 6-6 / 0 | 6 / 6 / 6-6 / 0 |
| PID 1 CPU time of the init itself, ms (PID 1 schedstat run time minus the calibration image of the same variant) | 6.2 / 12.6 / -2.4-12.9 / 3.5 | 39.0 / 45.9 / 29.8-48.8 / 4.9 | 36.8 / 49.1 / 30.7-49.2 / 5.3 |
| PID 1 binary on disk, KiB | 569 (static musl) | 144 + 1459 (executor) | 144 + 1380 (executor) |
| everything the init needs on disk (binaries + shared libraries), KiB | 569 | 22110 in 13 libraries (+ mount, helper 16, units) | 20830 in 20 libraries (+ mount, helper 16, units) |
| initramfs uncompressed (incl. identical fixture), KiB | 592 | 22239 | 20960 |

Systemd versions: Fedora `259.10-1.fc44`, CentOS Stream 10 `257-35.el10`.

Host during this run: load1 median 0.79, max 1.26; other-CPU busy during rounds median 0.05, max 0.92 CPUs; 0 rounds discarded and redone (kept in `final6.discarded.jsonl`), 0 rounds kept although flagged noisy after the attempt limit; 0 failed runs.

#### Effect of the tuning (nvsystemd before tuning = label baseline3, same harness, run separately)

Medians in ms. The NVRC columns are the same unmodified NVRC image in each of the two runs: they show how far the host or the kernel moved between the runs (it should be about zero).

| scenario | init to kata-agent before | after (Fedora) | after (CentOS 10) | time to kata-agent before | after (Fedora) | after (CentOS 10) | NVRC time to kata-agent in the baseline run | in this run |
|---|---|---|---|---|---|---|---|---|
| S0 cpu | 46.8 | 37.4 | 37.0 | 664.4 | 655.3 | 655.4 | 604.5 | 605.7 |
| S1 gpu defaults | 66.1 | 62.4 | 63.6 | 688.1 | 682.4 | 688.1 | 613.4 | 614.8 |
| S2 gpu + smi x4 + dcgm | 78.9 | 71.9 | 76.5 | 704.4 | 696.1 | 700.7 | 614.7 | 614.3 |
| S3 = S2 + SIMULATED latency x1 | 701.5 | 635.8 | 641.7 | 1323.5 | 1256.3 | 1267.2 | 1520.3 | 1518.1 |
| S4 servicevm-nvl4 | 62.8 | 53.3 | 56.9 | 696.0 | 689.3 | 693.4 | 625.1 | 623.3 |
<!-- /TABLES:final6 -->

### Runs that are not in the tables

The tables come from one run, label `final6`, taken on 2026-10-31 (12:47 to 13:14 GMT) on the host after the fd leak below was fixed,
with the gate and the guard active: 810 boots (8 scenarios x 30 rounds x 3 variants, plus 90 calibration boots), all complete, 0 rounds
discarded, 0 flagged noisy, memory stall 0 ms in every run, swap unused, `Shmem` at most 199 MB (`memwatch.log`). The before/after tuning
table comes from `baseline3` (untuned commit `7aea786` plus the fixture fix, nvrc and Fedora columns, 360 boots, 2 rounds discarded and
redone, 0 noisy), taken right after `final6` on the same host. Raw data (not committed, in the scratch directory
`/home/ecurtin/nvsd-work/bench/raw/`): `final6.jsonl`, `baseline3.jsonl`, `baseline3.discarded.jsonl`. Committed summaries:
`bench/results/final6.csv`, `final6.md`, `baseline3.csv`, `baseline3.md`.

**All older raw data was collected under the file descriptor leak (see below), is kept but labelled** (`bench/results/README.md`, a
`README.md` next to the raw files, a banner in the older summaries) and is not used for any conclusion here:

* `final3.jsonl` (+ `final3.discarded.jsonl`), `final4.jsonl` (an incomplete rerun that the host's out-of-memory killer ended), `baseline2.jsonl`:
  earlier three- and two-column runs. `final3` has 67 of its 810 runs (S2 and the latency sweep) with 0.8 to 6.4 s of kernel time before
  `/init` instead of the usual 0.6 s, for all three inits alike: the 512 MB guest being paged in while the host was short of memory.
  Compared with the clean `final6`, the **medians** of `final3` agree to within 0.1 to 17 ms of time to kata-agent (largest: S3x0.5 Fedora
  17 ms, S2 Fedora 14 ms; nearly always on the slow side; NVRC's S3 columns within 2 to 8 ms), but its standard deviations and p95 are
  meaningless (S2: sd 640 to 756 ms instead of 6 to 11 ms; S3x0.5: sd up to 1245 ms). The conclusions do not change, the dispersion figures do.
* `baseline.jsonl` (first untuned run, made before a stub bug was fixed that mounted over NVRC's `/proc`, `/sys` and `/run` in the end-marker
  process), `earlier-2col-final-loadedhost.jsonl` (host load above 40), `exp1` to `exp8` and `comp1` (tuning experiments,
  `bench/results/tuning.md`: they decided which ideas were kept; the clean before/after of the whole tuning is the last table above).

## What was tuned, and what did not help

See `bench/results/tuning.md` for the per-idea numbers; they come from the experiments made under the fd leak, so treat the
individual millisecond figures as indicative. The whole tuning, measured cleanly (same harness, `baseline3` vs `final6`, table above):
init to kata-agent drops from 46.8 to 37.4 ms (S0), 66.1 to 62.4 ms (S1), 78.9 to 71.9 ms (S2), 701.5 to 635.8 ms (S3; this includes the change that stopped `nvidia-smi-srs`
waiting for hostengine, the exporter and CDI generation), 62.8 to 53.3 ms (S4), while the unmodified NVRC image moved by at most 2.2 ms between the two runs. Kept: folding `cmdline`, the `/tmp` and configfs mounts and mode detection into one
helper process (`nvsd-init.service`: each unit costs PID 1 a `systemd-executor` start, about 2 ms here), `Slice=system.slice` for the two
templates (no extra slice units), an eagerly started syslog sink so the persistence daemon's readiness marker is not waiting for
socket activation, the lockdown folded into mode detection for cpu mode, independent chains running in parallel, an `OnFailure=` power-off
unit instead of `FailureAction=poweroff-immediate` (which sleeps 5 s). No measurable effect: `system.conf` accounting/logging
switches, `selinux=0`, `audit=0`, `systemd.log_level=err`, `systemd.show_status=0`. Compression: zstd -19, gzip, lz4 and uncompressed are
within about 3 ms of each other; xz costs 223 ms. Not done: a trimmed rebuild of Fedora's systemd SRPM (no network access to the
SRPM; it would be a separate, labelled artifact anyway), and per-mode dispatch of the transaction (described in `tuning.md`).

## Verdict

Plain language, for the simulated fixture on this host (2 vCPU guest, same kernel for every column). "Time to kata-agent" is what a
Kata user waits for; the rest of a real boot (the real `nvidia` module load, real `nvidia-smi`) is not in these numbers.

* **When the tools take no time (S0, S1, S2, S4), NVRC is faster.** A stock systemd costs **37 ms (cpu mode) to 72 ms (gpu with all smi
  settings and dcgm)** of userspace time more than NVRC's 0.3 to 3 ms, and about 7 to 16 ms more kernel time because its initramfs is
  31x larger to unpack (7.8 MiB vs 259 KiB compressed). In total nvsystemd is **+48 to +85 ms** slower (about 8 to 14 percent of a
  0.6 to 0.7 s boot), paired differences, with standard deviations of 5 to 11 ms, so these gaps are well outside the noise.
* **When the tools take time, nvsystemd wins by running things concurrently.** NVRC runs `nvidia-smi` x4, hostengine and CDI generation one
  after another; systemd runs the independent ones in parallel. With the (invented) S3 latency profile nvsystemd reaches the kata-agent
  **261 ms sooner (1256 vs 1518 ms, -17 percent)**, 85 ms sooner at half the latencies, 606 ms sooner at double (1807 vs 2416 ms,
  -25 percent). The break-even is near a quarter of the S3 latencies (total tool time about 230 ms): there the two are tied (+3 ms,
  within noise). How much of this exists on real hardware depends on whether NVML calls really run concurrently, which was not
  verified; if the real calls serialise inside the driver, this advantage shrinks to whatever the real tools allow.
* **Resources: NVRC is far smaller.** Init RSS is 0.27 MiB for NVRC against 11.5 MiB (Fedora) / 10.8 MiB (CentOS 10); memory used at
  kata-agent start 48.5 MiB vs 82.4 / 81.9 MiB (this includes the unpacked initramfs, which is the root filesystem here); PID 1 CPU time
  of the init itself 6 ms vs 39 / 37 ms; everything the init needs on disk 0.6 MiB vs 21.6 / 20.3 MiB.
* **CentOS Stream 10's systemd 257 vs Fedora's 259.** The same unit files boot and pass all 33 boot tests on both. CentOS 10 is a little
  smaller (7.2 vs 7.8 MiB initramfs, 10.8 vs 11.5 MiB init RSS) and **no faster**: paired time to kata-agent is +2 to +10 ms slower than
  Fedora in the gpu scenarios (S1 +6.0, S2 +4.0, S3 +9.7, S4 +2.3 ms; S0 +1.7 ms, within noise), roughly one standard deviation. Do not
  read a difference of a few milliseconds between the two bases as significant; do read "equal to within a few percent" as supported.
* **The tuning paid off** by 4 to 10 ms of init time in zero-latency scenarios and 66 ms in S3 (previous section), without touching the
  security properties.
* **What would change the answer:** real tools much faster than the invented profile (NVRC's lead is then at most ~80 ms), or
  real tools that serialise (the parallel gain disappears), or a larger guest (more vCPUs help systemd's parallel part only).


## Threats to validity

* **Simulated everything.** Stub tools, fake PCI. Real tools are slower, make different syscalls, may be more or less parallelisable.
  The S3 latency profile is invented (round numbers chosen by the task, not measured on hardware); the sweep shows the shape.
* **The file descriptor leak in the benchmark driver (an incident of this project, found late).** Until commit `b99398a`
  `fixture/fakepci/fakepci.py` ignored the `SCM_RIGHTS` fds that QEMU sends with the vfio-user `DMA_MAP` message (a dup of the guest RAM
  memfd). Every GPU-scenario guest booted by the long-lived benchmark process therefore left a 512 MiB memfd open in that process:
  hundreds of fds and about 20 GB of host `Shmem` after roughly 40 rounds, with growing swap and allocation stalls. This made the host
  run out of memory three times (04:10, 09:36 to 10:35 and 11:03 GMT; the last time systemd-oomd killed the agent session running the
  benchmark) and **distorted every earlier
  GPU-scenario benchmark, mostly in its late rounds**. It did not affect the boot tests (few guests per process) or the CPU-mode
  scenario. All affected raw data is labelled and excluded (previous section); the tables above were collected again from scratch after
  the fix, together with the untuned baseline, and the driver now aborts when it holds more than 64 fds or the system holds more than
  4 GiB of `Shmem`. The `final6` run shows the symptom gone: 0 ms of memory stall in all 810 boots, swap unused, `Shmem` at most 199 MB.
  The earlier and the clean medians agree to within a few percent, so the verdict did not move, but nothing from the old runs is quoted.
* **Different glibc, systemd version and build flags** between the two nvsystemd columns (glibc 2.43 vs 2.39, systemd 259 vs 257, Fedora
  vs CentOS compiler flags and enabled features): the column difference is a bundle, it does not isolate systemd's source version.
* **Shared host.** Other jobs ran on the same machine (cargo builds of other projects). The interleaving, the paired differences,
  the quiet-host wait and the stored load figures mitigate but do not remove this. Standard deviations are reported; look at them.
* **2 vCPU guest.** PID 1 in systemd is single-threaded and each unit start costs it a fork; with more vCPUs the parallel parts would
  scale further, the serial parts would not.
* **Kernel dominates.** About 600 ms of every boot is the kernel before init. Kernel time differs by the image size (nvsystemd's initramfs
  is 31x bigger compressed); the init-to-agent column isolates what the init systems themselves do.
* **Memory figures include the unpacked initramfs** (it is the root filesystem). A real Kata guest has a read-only rootfs on a block
  device where the files would be paged in lazily; the nvsystemd penalty would be smaller there.
* **Parallel `nvidia-smi` calls** (and hostengine/ctk concurrent with them) are an assumption about real NVML behaviour.
* **PID 1 RSS of NVRC** is measured on a fork of itself taken just before exec, which can include pages shared copy-on-write.
* **Single kernel, single QEMU, single host CPU model**; no cross-checks on other hardware.
* nvsystemd's **helper is C** and runs as root; no fuzzing was done.
