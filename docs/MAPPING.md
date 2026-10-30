# NVRC to nvsystemd mapping

Reference: NVIDIA/NVRC at commit `b9700f3` (v0.1.7, `src/*.rs`, 5,392 lines including tests). Every
function, every forked process and every behaviour of `main()` is listed here with what replaced it:

* **unit**: a systemd unit (file in `overlay/etc/systemd/system/`),
* **helper**: a subcommand of the static `nvsd-helper` (`helper/nvsd-helper.c`, one libc-free C file, about 1000
  lines including comments), allowed only where systemd cannot do the job (reason given for each),
* **dropped**: intentionally not reproduced (reason given).

Boot test ids (`t..`, `u..`) refer to `scripts/boot-test.py`. They run the same expectations against the
NVRC baseline and, where marked, only against nvsystemd. All of it runs on a SIMULATED machine
(see README "Fixture").

## main() step by step

| NVRC (`main.rs` order) | nvsystemd | Test |
|---|---|---|
| `init::as_pid1()` (print version and exit if not PID 1) | dropped: Fedora's systemd has its own PID 1 logic | |
| `lockdown::set_panic_hook()` (kmsg line, `sync()`, power off on any panic) | `nvsd-poweroff.service` + helper `poweroff`; every unit has `OnFailure=nvsd-poweroff.service` | t30-t36, u10 |
| `NVRC::default()` | none (state is the files under `/run/nvsd`) | |
| `mount::setup()`: /proc /sys /run (nosuid,noexec,nodev,relatime; run mode=0755) | systemd's built-in early API mounts (no unit) | t01 |
| `mount::setup()`: /tmp tmpfs (nosuid,nodev,relatime) | `nvsd-init.service`, helper `init` (`mount(2)`). The first implementation was the native `tmp.mount`; each mount unit forks `systemd-executor` and `/usr/bin/mount`, measured at 4.8 ms for the two mount units on the zero-latency gpu scenario (`bench/results/tuning.md`), so they were folded into the init helper (git history has the units) | t01 |
| `mount::setup()`: securityfs on /sys/kernel/security (optional) | systemd's built-in table (`/sys/kernel/security` is mounted when the kernel has it) | |
| `mount::setup()`: configfs on /sys/kernel/config (optional: only if the kernel has it and the target exists) | `nvsd-init.service`, helper `init` (same reason as /tmp) | t01 |
| `net::loopback_up()` | systemd brings `lo` up itself at startup (checked: `lo_flags` has IFF_UP) | t01 |
| `kmsg::kernlog_setup()`: four `net.core` buffers = 16 MiB | `nvsd-init.service`, helper `cmdline` (systemd has no sysctl-write directive at a chosen point; systemd-sysctl would be a second large binary) | t04 |
| `kmsg::kernlog_setup()`: kernlog logger | dropped: systemd logs to the kernel ring buffer by itself (`log_target` kmsg without journald) | |
| `syslog::poll()` (bind `/dev/log`, drain to `/run/syslog.log`) | `nvsd-syslog.socket` (owns `/dev/log`) + `nvsd-syslog.service` (socket activated), helper `syslog` | t06 |
| `process_kernel_params()` | `nvsd-init.service`, helper `cmdline`: writes `/run/nvsd/*` files that unit conditions and `EnvironmentFile=` consume. systemd cannot split `nvrc.smi.lgc=1500` into an argument, normalise lenient booleans or validate numbers | t04, t15, t16, t34, t35 |
| `hash::self_exe()` (sha256 of the init, logged for `nvrc.log=info|debug|trace`) | `nvsd-hash.service`, helper `hash`: hashes `systemd`, `systemd-executor` and the helper, only when logging is on (conditions on the three levels) | t05 |
| `guest_extension_image::mount_all()` | `nvsd-extensions.service` (helper `ext-plan`: device/params reconciliation, fail closed), `nvsd-extension@.service` (`veritysetup open`), `run-kata\x2dextensions-gpu.mount` (erofs), all gathered by `nvsd-extensions.target` | u13 |
| `gpu_extension::setup()` bind firmware | `usr-lib-firmware-nvidia.mount` | u14 |
| `gpu_extension::setup()` ldconfig + bind cache | `nvsd-ldcache.service` (static conf `/usr/lib/nvsd/ld.so.conf`) + `etc-ld.so.cache.mount` | u14 |
| `mode::detect()` | `nvsd-init.service`, helper `detect` (systemd cannot read sysfs file contents or VPD) + `nvsd-{cpu,gpu,servicevm-nvl4,servicevm-nvl5}.target` | u07, t33 |
| `match detected.mode` | the four mode targets; per-unit `ConditionPathExists=/run/nvsd/...` (systemd pulls `Requires=/Wants=` dependencies into the transaction before conditions are evaluated, so every unit carries its own condition) | u07 |
| `lockdown::disable_modules_loading()` | `nvsd-lockdown.service`, helper `lockdown` (a late sysctl write again). In cpu mode nothing is loaded after the extension mount, so `detect` applies it itself and the unit is skipped (`/run/nvsd/locked`) | t03 |
| `kata_agent::fork_agent()` | `kata-agent.service` (below) | t02 |

## Forked or exec'd processes

| NVRC process | nvsystemd unit | Notes |
|---|---|---|
| `/sbin/modprobe [--dirname ROOT] <module> [NVreg_NvLinkDisable=1]` (`modprobe.rs`), one per module | `modprobe@.service` template; drop-ins `modprobe@{nvidia,nvidia-uvm,ib_umad,mlx5_ib}.service.d/nvsd.conf` hold the per-module condition and ordering. `NVSD_PARAMS`/`NVSD_DIRNAME` come from `/run/nvsd/modprobe-%i.env` | u08, t12, t31. Single-GPU `NvLinkDisable=1` decided by the helper's `detect`. Module loads stay serialised (nvidia, nvidia-uvm, ib_umad, mlx5_ib): NVRC's own test comment says parallel modprobe can race |
| `/bin/nvidia-persistenced --verbose [--uvm-persistence-mode]` + wait for the syslog marker `Local RPC services initialized` (600 s) | `nvidia-persistenced.service`: `Type=forking`, `RuntimeDirectory=nvidia-persistenced`, `ExecStartPost=nvsd-helper wait-marker ... 600` | u10. The wait is helper `wait-marker` (inotify on `/run/syslog.log`): systemd has no "wait for text" primitive |
| `nvidia-smi -lmc N` | `nvidia-smi-lmc.service` | u11 |
| `nvidia-smi -lgc N` | `nvidia-smi-lgc.service` | u11 |
| `nvidia-smi -pl N` | `nvidia-smi-pl.service` | u11 |
| `nvidia-smi conf-compute -srs 0|1` | `nvidia-smi-srs.service` | u11, t14 |
| `/bin/nv-hostengine --service-account nvidia-dcgm --home-dir /tmp` (if `nvrc.dcgm`) | `nv-hostengine.service` (`Type=forking`) | t13 |
| `/bin/dcgm-exporter -k -f <csv>` (if `nvrc.dcgm`) | `dcgm-exporter.service` (`Type=exec`) | t13 |
| `/bin/nvidia-ctk -d cdi generate --output=/var/run/cdi/nvidia.yaml [ext flags]` | `nvidia-ctk-cdi.service` | t11, u14 |
| `/bin/nv-fabricmanager -c /run/fabricmanager.cfg [-g GUID]` + marker `FM starting NvLink Inband` (120 s) | `nv-fabricmanager.service`: `ExecStartPre=nvsd-helper fm-config` (copy + edit + chmod 0400, replacing `config.rs update_config_file`), `Type=forking`, `ExecStartPost=wait-marker ... 120` | t20, t21, u12 |
| `/sbin/nvlsm -F <cfg> -g GUID -f stdout` | `nvlsm.service` (`Type=exec`), `Before=nv-fabricmanager` via the FM unit's `After=` | u12 |
| `infiniband::detect_port_guid()` (in-process) | `nvsd-ib-guid.service`, helper `ib-guid` (sorting and parsing sysfs text) | u12, t22 |
| `uvm::setup_uvm_tools()` (in-process mknod) | `nvsd-uvm-tools.service`, helper `uvm-tools` (the device major is dynamic: not expressible in a static device unit or tmpfiles) | u09, t36 |
| `/usr/sbin/veritysetup open ...` (`guest_extension_image.rs`) | `nvsd-extension@.service` | u13 |
| `/sbin/ldconfig -X -f ... -C /run/ld.so.cache` | `nvsd-ldcache.service` | u14 |
| `kata-agent` (`exec`, becomes PID 1; sets RLIMIT_NOFILE 1 Mi and oom_score_adj -997; `KATA_ATTESTER_VARIANT=nvidia` with the gpu extension) | `kata-agent.service`: `LimitNOFILE=1048576`, `OOMScoreAdjust=-997`, `EnvironmentFile=-/run/nvsd/kata.env`, `OnSuccess=`/`OnFailure=nvsd-poweroff.service` | t02, u14 |
| syslog poller child that survives next to kata-agent | `nvsd-syslog.service` (socket activated: never started in cpu mode) | t06 |
| `health_checks()` (point-in-time `try_wait` of tracked daemons, at several points of the flow) | replaced, not reproduced: each daemon unit has `OnFailure=nvsd-poweroff.service`, so an exit at any moment (before or after kata-agent) powers the VM off; and `Requires=` chains keep kata-agent from starting after a failure | t32 |

## Dependency graph

```
 kernel -> systemd (PID 1) -> built-in API mounts, lo up
   |
   +-- nvsd-boot.target (= default.target)  Requires=kata-agent.service, OnFailure=nvsd-poweroff
        |
        kata-agent.service  Requires=nvsd-lockdown.service nvsd-base.target
            |  After
        nvsd-lockdown.service  Requires/After= the four mode targets
            |
   +--------+------------------------+--------------------------+------------------------+
   nvsd-cpu.target     nvsd-gpu.target        nvsd-servicevm-nvl4.target  nvsd-servicevm-nvl5.target
   (cond mode.cpu)     (cond mode.gpu)        (cond mode.servicevm-nvl4)  (cond mode.servicevm-nvl5)

 nvsd-base.target: nvsd-syslog.socket, nvsd-init.service (Wants nvsd-hash)

 nvsd-extensions.target: nvsd-extensions.service -> nvsd-extension@gpu -> run-kata\x2dextensions-gpu.mount
                          -> usr-lib-firmware-nvidia.mount | nvsd-ldcache.service -> etc-ld.so.cache.mount
 nvsd-init.service: mounts /tmp and configfs, kernel parameters, mode detection (After nvsd-extensions.target)

 gpu mode (parallelism that NVRC leaves on the table is marked *):

   nvsd-init
        |
   modprobe@nvidia -> modprobe@nvidia-uvm -> nvsd-uvm-tools (if nvrc.uvm.tools)
        |                                         |
        |            [NVSwitch: modprobe@ib_umad -> modprobe@mlx5_ib -> nvsd-ib-guid -> nvlsm -> nv-fabricmanager]
        |                                         |
        +--------------------> nvidia-persistenced (ExecStartPost waits for the syslog marker)
                                   |      |         |        |
                      nvidia-smi-lmc*  -lgc*  -pl*   nv-hostengine* -> dcgm-exporter     nvidia-ctk-cdi*
                                   \      |      /           |                              |
                                    +-----+------+-----------+------------------------------+
                                                     |
                                              nvidia-smi-srs  (last, like NVRC)
```

## Honest semantic differences

1. **Parallelism.** NVRC runs everything serially. nvsystemd keeps (a) driver modules before anything that needs the
   driver, (b) module loads one after the other, (c) `nvidia-persistenced` ready (marker seen) before `nvidia-smi` and
   CDI generation, (d) `srs` after every other configuration step, (e) lockdown after every unit of every mode,
   (f) kata-agent last. It runs `-lmc`, `-lgc`, `-pl`, `nv-hostengine`/`dcgm-exporter` and `nvidia-ctk cdi generate`
   concurrently. Concurrent `nvidia-smi` invocations are normally safe (NVML serialises in the driver) but this was
   **not verified on real hardware** (no GPU here); an operator who wants NVRC's order can add
   `After=nvidia-smi-lmc.service` / `After=nvidia-smi-lgc.service` drop-ins.
2. **Failure detection is stronger and earlier.** NVRC checks daemon exit status only at a few points
   (`health_checks()` after a `try_wait`), and waits up to 600 s for a syslog marker that a dead
   `nvidia-persistenced` never sends. nvsystemd powers off as soon as a daemon exits (test u10, t32).
3. **Power-off path.** NVRC's panic hook writes the panic text to kmsg, calls `sync()` and `reboot(POWER_OFF)`.
   nvsystemd does the same through `nvsd-poweroff.service`, but the message names no unit (systemd's own failure
   line does). `FailureAction=poweroff-immediate` was measured to add a fixed 5 s sleep ("proceeding in 5s") and is
   therefore not used; the first implementation used it and the failure tests took 6.1 s instead of 1.1 s.
4. **Extension names.** NVRC mounts any `extension-<name>` device. systemd cannot instantiate units dynamically, so only
   names with a mount unit in the image (shipped: `gpu`) are accepted; others are fatal (fail closed). A stray
   `kata.extension.<name>.verity_params` is noticed only if some block device with a serial exists
   (`ConditionPathExistsGlob`), which makes the common no-extension boot free of a helper process.
   The dm-verity/erofs mount itself cannot be exercised in this environment (no dm-verity or erofs modules in the test
   image, no veritysetup): the plan, the `veritysetup` argument list and the fail-closed paths are tested (u13); the
   happy-path mount is **unverified**.
5. **Logging.** `nvrc.log=<level>` sets `printk_devkmsg=on` (as NVRC) and for `debug`/`trace` sends daemon output to
   `/dev/kmsg` instead of `/dev/null`. NVRC's per-level `log::` messages do not exist; systemd's verbosity is
   controlled separately by `systemd.log_level=` (documented in the README). systemd itself also switches
   `printk_devkmsg` to `on` at startup (visible in test t04).
6. **PID 1.** NVRC execs kata-agent so that it *is* PID 1. Here systemd stays PID 1 and kata-agent is its child
   (`OOMScoreAdjust=-997`, which is what keeps the agent alive under memory pressure, applies equally). If kata-agent
   exits with success or failure the VM powers off (`OnSuccess=`/`OnFailure=`).
7. **Version stamp.** There is no `NVRC version=... sha256=...` line; `nvsd-hash.service` logs the sha256 of systemd,
   its executor and the helper instead (only for `nvrc.log=info|debug|trace`, the same condition as NVRC's output).
8. **Kernel parameter names.** `nvrc.log`, `nvrc.uvm.persistence.mode`, `nvrc.uvm.tools`, `nvrc.dcgm`,
   `nvrc.smi.srs|lgc|lmc|pl` are implemented with NVRC's exact parsing rules (lenient booleans, last occurrence wins,
   strict boolean for `srs`, decimal u32 for the rest, validation errors are fatal). `nvrc.mode`, `nvrc.fm.mode` and
   `nvrc.fm.rail.policy` do **not exist** in the reference revision (mode comes from PCI topology, FM mode and rail
   policy are fixed per topology), so they are not implemented.
9. **The attack surface is larger.** See `docs/DESIGN.md`.
