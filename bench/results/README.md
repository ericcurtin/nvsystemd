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

Runs made after the fix, with the driver guard of `bench/run.py` (abort when the driver holds more than 64 fds or system Shmem
exceeds 4 GiB after a round) active: see the entries labelled `final6` and `baseline3` (when present) and docs/BENCHMARK.md.
