# nvsystemd design notes: security mapping and attack surface

nvsystemd replaces NVRC's hand-written PID 1 with Fedora's stock `/usr/lib/systemd/systemd` (259.10-1.fc44, unmodified)
plus static unit files and a small helper. This file says how NVRC's security-relevant behaviours are
carried over, what is weaker, and what is larger. Nothing here was tested on a confidential VM; it is a design
argument backed by the simulated boot tests listed in `docs/MAPPING.md`.

## Properties NVRC provides, and how they are mapped

| NVRC property | nvsystemd mechanism | Where it differs |
|---|---|---|
| Fail fast: any panic writes to `/dev/kmsg`, `sync()`s and powers the VM off | `OnFailure=nvsd-poweroff.service` on every unit (helper `poweroff`: kmsg line, `sync()`, `reboot(RB_POWER_OFF)`). `Requires=` chains make a failed unit cancel `nvsd-gpu.target`, `nvsd-lockdown.service` and `kata-agent.service`, so the agent is never started after a failure (tests t30-t36, u10-u13) | between the failure and the power-off systemd keeps running other jobs for a few milliseconds (a parallel unit can finish). `FailureAction=poweroff-immediate` was rejected: it sleeps 5 s. A unit that fails to *start at all because its binary is missing* is a failure too (`Failed to start`), also covered |
| No module can be loaded after setup | `nvsd-lockdown.service` writes `kernel.modules_disabled=1` after the four mode targets (which contain every `modprobe@` instance); `kata-agent.service` has `Requires=`+`After=` on it (t03) | the lock is applied by a unit job, not in the same process as the last modprobe; the ordering is by dependency, not by program order. Modules requested by the kernel on behalf of systemd itself (none in this image) would also be blocked |
| kata-agent survives memory pressure | `OOMScoreAdjust=-997` on `kata-agent.service` (t02). NVRC sets it on the process that is PID 1 | systemd and its helpers keep their default scores; a runaway daemon could in principle be killed first, as in NVRC |
| kata-agent has RLIMIT_NOFILE = 1 Mi | `LimitNOFILE=1048576` (t02) | |
| Fail closed on unverified extensions | `ext-plan` reconciles devices with the measured kernel command line, `veritysetup` runs before any mount, failure of either powers off (u13) | only names with a mount unit in the image are accepted |
| Configuration only from the measured kernel command line | same: `nvsd-init.service` (helper `cmdline`) is the only reader; unit files are immutable image content | the units themselves are part of the image and therefore of the dm-verity measurement; see below for the EnvironmentFile channel |
| Child output goes to `/dev/null` unless debug logging | `StandardOutput=file:/run/nvsd/out`, a symlink created from `nvrc.log` (`/dev/null` or `/dev/kmsg`) | |
| No shell, no scripts | no unit uses a shell; every `ExecStart=` is a binary. The helper is not a shell | |

## What the helper is trusted with

`nvsd-helper` is the only code in the image that is neither Fedora's nor the fixture. It is a single C file (no libc,
raw syscalls, fixed-size buffers). It runs as root. Its inputs:

* `/proc/cmdline` (measured by the confidential-VM firmware, same trust as NVRC's input),
* sysfs text (PCI ids, VPD, `/sys/class/infiniband`, `/sys/block/*/serial`), which the (untrusted) hypervisor controls
  to the same extent as it does for NVRC,
* files it wrote itself under `/run/nvsd`.

Values that reach unit files through `EnvironmentFile=` are restricted: numbers go through the strict decimal u32 parse
(so `NVSD_VALUE` is digits only), the persistenced and fabric-manager arguments are fixed strings, the verity
`root_hash` and `salt` are restricted to `[0-9A-Za-z]`, GUIDs come from hex groups in sysfs and end up as one word
in `NVSD_FM_GUID_ARGS`. systemd splits `$VAR` on whitespace only, never through a shell.

A memory-safety bug in the helper is a real risk that NVRC (Rust) does not have in its own code. The mitigation is size
(about 800 lines including comments, no allocation, no libc) and the fact that every subcommand is a one-shot, short-lived
process; a fuzz target was not written (remaining work).

## Attack surface of a full systemd

Measured from the image built by `scripts/mkimage.py` (`bench/results/*` has the sizes of the real runs):

| | NVRC | nvsystemd |
|---|---|---|
| PID 1 code | one static musl binary, 0.58 MB, about 5,400 lines of Rust (incl. tests), crates: nix, libc, log, kernlog, sha2, rlimit, once_cell | `systemd` (148 KB) + `systemd-executor` (1.5 MB) + `libsystemd-core` (2.7 MB) + `libsystemd-shared` (5.7 MB) + libcrypto 5.7 MB, libc, libm, libmount, libblkid, libselinux, libpcre2, libcrypt, libz, libgcc_s (22.7 MB uncompressed in total) |
| Dynamic loading | none | ld.so; systemd `dlopen()`s libmount (included) and others on demand (not present in the image: the call fails, systemd continues) |
| IPC endpoints | none | `/run/systemd/private` (root only), notify socket (`/run/systemd/notify`), the control socket; there is no D-Bus daemon, no `systemctl` binary, no journald, no udev, no logind in the image |
| Parsing of external input | `/proc/cmdline` | `/proc/cmdline` (systemd looks at `systemd.*` and `rd.*` items, and `ConditionKernelCommandLine`), unit files (static), plus everything above |
| Processes at boot | PID 1 only | PID 1 plus a `systemd-executor` per started process (short lived) |
| Runtime memory | see `docs/BENCHMARK.md` | |

Things nvsystemd does to keep the surface smaller than a distribution boot: no generators, no `systemd-udevd`,
`systemd-journald`, `systemd-logind`, `dbus`, `systemctl`, `journalctl`; units are `DefaultDependencies=no`; no
`Wants=` on anything that was not listed in `docs/MAPPING.md`; the image has no unit files of Fedora's (only
`/etc/systemd/system` from the overlay). What it cannot do with a stock binary: remove the seccomp, selinux, PAM/audit
code paths that are compiled in, or the libcrypto dependency.

An optional stretch (not done unless stated in `docs/BENCHMARK.md`) is a trimmed rebuild of Fedora's systemd SRPM;
that would be a different, separately labelled artifact.

## Unit hardening that was not applied

The units run as root with no `ProtectSystem=`, `PrivateDevices=`, `NoNewPrivileges=` etc. NVRC runs its children as
root with the inherited environment too, the NVIDIA tools need `/dev/nvidia*` and `/proc`/`/sys` access, and
every sandboxing option costs a mount namespace (measured cost: see `docs/BENCHMARK.md` if tried). Sandboxing the
helper and the daemons is possible later and would be an improvement over NVRC.
