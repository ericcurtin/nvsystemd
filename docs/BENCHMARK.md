# Benchmark: NVRC vs nvsystemd

**Everything below is SIMULATED hardware.** There is no GPU on the benchmark host. The NVIDIA tools are stubs
(`fixture/stub/nvstub.c`) that validate their argument lists, sleep for a configurable latency (default 0) and log
timestamps. The numbers measure init-system behaviour, not GPU driver bring-up. Real `modprobe nvidia`, `nvidia-smi`
and CDI generation take far longer than anything measured here, which only makes the absolute share of the init
system smaller.

## What was compared

| | NVRC | nvsystemd |
|---|---|---|
| PID 1 | NVRC `b9700f3` (v0.1.7), unmodified, NVRC's own `[profile.release]`, `x86_64-unknown-linux-musl`, static, built by `scripts/build-nvrc.sh` | Fedora `systemd` 259.10-1.fc44, unmodified, plus its `ldd` libraries copied from the host |
| rest of the image | identical fixture files, stub binaries (byte for byte the same file, hard-linked under the tool names), `kata-agent` stub | same, plus `overlay/` (units) and `nvsd-helper` |
| machine | the same kernel (`/lib/modules/7.2.13-200.fc44.x86_64/vmlinuz`, `-kernel`), QEMU/KVM q35, `-cpu host`, 2 vCPU, 512 MB, serial console, `-no-reboot` | same |
| kernel command line | `console=ttyS0 quiet panic=-1 8250.nr_uarts=1 random.trust_cpu=on TERM=dumb tsc=reliable no_timer_check log_buf_len=1M` plus the scenario's `nvrc.*` items | same, byte for byte |
| initramfs | cpio (newc), zstd -19 | same codec |
| simulated PCI | `fixture/fakepci/fakepci.py` over vfio-user: 1 GPU `10de:2330` class 0302 (S1-S3), 4 NVSwitch `10de:22a3` class 0680 (S4) | same |

## Method

* `bench/run.py`: for every scenario it builds both images, runs 2 warm-up rounds, then **30 measured rounds per scenario**. In
  every round both variants boot once, **interleaved**, and the order alternates between rounds (A B, B A, ...) so that drift
  and bursts of load from other jobs on this shared host hit both. Each QEMU run is wrapped in `timeout -s KILL 60`. Before each
  round the driver waits (at most 90 s) until other host load is below 2.5 CPUs; the measured value is stored with every run
  (`host_busy_cpus`, `loadavg1`).
* The end marker is the `kata-agent` stub. It is the same file for both inits. It records `CLOCK_BOOTTIME` the instant it starts, then
  prints a report (memory, process census, per-process RSS/PSS, PID 1 CPU time, mounts, the stub event log) on the serial console
  and powers the VM off. A run without a complete report is counted as failed and excluded (none occurred unless the table says so).
* **Definition of "time to kata-agent start"**: `CLOCK_BOOTTIME` in the kata-agent stub at its first instruction. It includes
  kernel boot and initramfs unpack. It is reported together with its two parts: *kernel time* = the kernel's own timestamp of
  `Run /init as init process`, and *init to kata-agent* = the difference (userspace). QEMU start-up time on the host is not included.
* Statistics per cell: median, p95 (nearest rank), min, max, sample standard deviation. The *paired diff* is the median over rounds of
  (nvsystemd - NVRC) in the same round.
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

<!-- TABLES:final -->
<!-- /TABLES:final -->

Raw per-run data: `/home/ecurtin/nvsd-work/bench/raw/final.jsonl` (and `baseline2.jsonl` for the untuned nvsystemd; the older
`baseline.jsonl` is the first untuned run, made before a stub bug was fixed that mounted over NVRC's `/proc`, `/sys`
and `/run` in the end-marker process; it is kept for transparency). Summaries: `bench/results/final.csv`,
`bench/results/final.md`, `bench/results/baseline*.csv`. Tuning experiments: `bench/results/tuning.md`.

## What was tuned, and what did not help

See `bench/results/tuning.md` for the numbers. Kept: folding `cmdline`, the `/tmp` and configfs mounts and mode detection into one
helper process (`nvsd-init.service`: each unit costs PID 1 a `systemd-executor` start, about 2 ms here), `Slice=system.slice` for the two
templates (no extra slice units), an eagerly started syslog sink so the persistence daemon's readiness marker is not waiting for
socket activation, the lockdown folded into mode detection for cpu mode, independent chains running in parallel, an `OnFailure=` power-off
unit instead of `FailureAction=poweroff-immediate` (which sleeps 5 s). No measurable effect: `system.conf` accounting/logging
switches, `selinux=0`, `audit=0`, `systemd.log_level=err`, `systemd.show_status=0`. Compression: zstd -19, gzip, lz4 and uncompressed are
within about 3 ms of each other; xz costs 223 ms. Not done: a trimmed rebuild of Fedora's systemd SRPM (no network access to the
SRPM; it would be a separate, labelled artifact anyway), and per-mode dispatch of the transaction (described in `tuning.md`).

## Verdict

<!-- VERDICT -->

## Threats to validity

* **Simulated everything.** Stub tools, fake PCI. Real tools are slower, make different syscalls, may be more or less parallelisable.
  The S3 latency profile is invented (round numbers chosen by the task, not measured on hardware); the sweep shows the shape.
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
