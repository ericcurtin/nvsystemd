# Benchmark results: what is what

All timing data is from SIMULATED hardware (fake PCI devices, stub tools) on a shared host. Raw per-run data (one JSON line per
boot) is not committed; it lives in `/home/ecurtin/nvsd-work/bench/raw/` and is listed in the `README.md` there.

## Collected under the file descriptor leak (suspect)

Until commit `b99398a` the fake PCI fixture (`fixture/fakepci/fakepci.py`) ignored the `SCM_RIGHTS` fds that QEMU sends with the
vfio-user `DMA_MAP` message (a dup of the guest RAM memfd, 512 MiB). Every GPU scenario guest booted by the benchmark driver therefore
leaked a memfd into the driver process: hundreds of fds and about 20 GB of host `Shmem` by round 40 of a run. Memory pressure, swap and
allocation stalls grew throughout every long run (and the host was OOM-killed three times), so **late rounds of every run that
booted GPU scenarios are possibly distorted**. This does not affect the unit and boot tests (they boot a handful of guests).

Files in this directory collected under that leak, kept only for transparency and **not used for any conclusion**:

* `baseline.csv`, `baseline.md` (first untuned run; also made before a stub bug fix, see docs/BENCHMARK.md)
* `final3.csv`, `final3.md` (first three-column run; it also shows the memory-stall symptoms described in docs/BENCHMARK.md)
* `tuning.md` (tuning experiments exp1 to exp8 with PCI presets)

The superseded data of runs that never completed (`final4`, `baseline2`) was never summarized here.

## Clean data

Collected on 2026-10-31 after the fix, with the driver guard of `bench/run.py` active (abort when the driver holds more than 64 fds or
system Shmem exceeds 4 GiB after a round):

* `final6.csv`, `final6.md`: the three-column run (S0-S4 and the latency sweep, 30 rounds each, interleaved), the source of the tables in docs/BENCHMARK.md
* `baseline3.csv`, `baseline3.md`: the untuned commit (nvrc and Fedora columns) for the before/after tuning table

Memory behaviour of these runs: 0 ms memory stall in every boot, swap unused, Shmem at most 199 MB (host watcher log).
