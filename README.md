# nvsystemd

**What this is.** A version of [NVIDIA/NVRC](https://github.com/NVIDIA/nvrc) (a ~5k-line Rust PID 1 for ephemeral
NVIDIA GPU micro VMs under Kata Containers) that uses an **unmodified stock systemd** as PID 1, on either of two bases:
**Fedora's** (`/usr/lib/systemd/systemd`, 259.10-1.fc44, plus the shared libraries `ldd` reports, copied from the build
host) or **CentOS Stream 10's** (257-35.el10 with its own glibc and libraries, fetched from the CentOS container image
by `scripts/fetch-centos10-systemd.sh`). The same unit files run on both, no unit needed a change. NVRC's job is expressed
as unit files: one systemd unit per process NVRC forks or execs (`nvidia-persistenced`, each `nvidia-smi` call,
`nv-hostengine`, `dcgm-exporter`, `nvidia-ctk cdi generate`, `nv-fabricmanager`, `nvlsm`, one `modprobe@.service`
instance per module, `kata-agent`), targets for the modes (cpu, gpu, servicevm-nvl4, servicevm-nvl5), ordering and
conditions instead of NVRC's straight-line `main()`, and `OnFailure=` to power the VM off the way NVRC's panic hook does.
A tiny static helper (`nvsd-helper`) exists only where systemd cannot do the job (plus two mounts folded in for speed,
see `docs/MAPPING.md`); each subcommand is justified there.

**About the name.** The request called the thing being replaced "nvinit". No such NVIDIA project exists; the project
that does this job is NVRC (NVIDIA Runtime Container init), which is what this repository mirrors. Where the request used
NVRC kernel parameter names, the ones that exist in the reference revision (`b9700f3`) are kept
(`nvrc.log`, `nvrc.uvm.persistence.mode`, `nvrc.uvm.tools`, `nvrc.dcgm`, `nvrc.smi.srs|lgc|lmc|pl`); `nvrc.mode`,
`nvrc.fm.mode` and `nvrc.fm.rail.policy` are not in that revision and are not implemented.

**Everything measured here used SIMULATED hardware.** There is no GPU on the build host. A stub fixture
(`fixture/stub/nvstub.c`, one static binary hard-linked under the names NVRC execs) stands in for the NVIDIA userspace,
and `fixture/fakepci/fakepci.py` presents PCI devices with NVIDIA ids to QEMU over vfio-user so that
NVRC's and nvsystemd's PCI topology detection runs for real. No claim in this repository is about real GPU initialisation
time; `docs/BENCHMARK.md` says what the numbers do and do not show.

Licence: Apache-2.0 (`LICENSE`), `NOTICE` credits NVIDIA NVRC. Fedora's and CentOS's systemd (LGPL-2.1-or-later) are
**not redistributed**: they are copied from the host, respectively from a podman container, into images and a sysroot
that stay in the (git-ignored) scratch directory.

## Layout

```
overlay/etc/systemd/system/   the units (the product)
overlay/usr/lib/nvsd/         static data (ld.so.conf for the gpu extension)
helper/nvsd-helper.c          the static helper (libc-free C)
common/mini.h                 syscall layer shared by helper and fixture
fixture/stub/nvstub.c         simulated NVIDIA userspace + kata-agent stub (end marker, records timing and memory)
fixture/fakepci/fakepci.py    vfio-user server that fakes PCI devices (GPU, NVSwitch, CX7 with SW_MNG VPD)
scripts/                      image builder (no root needed, --base fedora|centos10), QEMU runner, boot tests, NVRC build,
                              fetch-centos10-systemd.sh (CentOS Stream 10 systemd sysroot via podman)
bench/                        run.py (interleaved NVRC / nvsd-Fedora / nvsd-CentOS10), summarize.py, compare.py, report.py,
                              results/ (small CSV/markdown)
docs/                         MAPPING.md, BENCHMARK.md, DESIGN.md
tests/nvsd-enabled.txt        which tests nvsystemd must pass (grew one unit at a time)
```

## Build

Scratch/build products live in `/home/ecurtin/nvsd-work` (persistent; override with `NVSD_OUT`/`OUT`), never in the repo.

```sh
export PATH=$HOME/.cargo/bin:/usr/bin:$PATH
export CARGO_TARGET_DIR=/home/ecurtin/nvsd-work/target
# reference NVRC (unmodified clone, read-only) at /home/ecurtin/nvsd-work/nvrc
git clone https://github.com/NVIDIA/nvrc /home/ecurtin/nvsd-work/nvrc && git -C /home/ecurtin/nvsd-work/nvrc checkout b9700f3
rustup toolchain install 1.94.0 && rustup target add x86_64-unknown-linux-musl   # NVRC pins rustc 1.94.0
scripts/build-nvrc.sh      # static musl release build with NVRC's own [profile.release]; no musl-gcc needed,
                           # the linker is overridden with an environment variable, NVRC's files are untouched
make stub helper           # fixture and helper (plain gcc, freestanding)
scripts/fetch-centos10-systemd.sh   # optional: CentOS Stream 10 systemd 257 sysroot (needs podman and the container's network
                                    # access for `dnf install systemd`); lands in /home/ecurtin/nvsd-work/centos10, git-ignored
```

Images are built on demand by the tests and the benchmark (`scripts/mkimage.py`, pure Python cpio writer, no root).
The kernel is the host's `/lib/modules/7.2.13-200.fc44.x86_64/vmlinuz`, used directly with `-kernel`/`-initrd`.

## Test

```sh
scripts/boot-test.sh                    # nvsystemd, the tests enabled in tests/nvsd-enabled.txt
scripts/boot-test.sh --all              # every test against nvsystemd
scripts/boot-test.sh --variant nvrc     # the same expectations against the NVRC baseline
scripts/boot-test.sh --variant nvsd-centos10   # nvsystemd on CentOS Stream 10's systemd (all 33 tests pass; the same 33 pass on the Fedora base)
scripts/boot-test.sh -k t13 -v          # one test
```

Every test boots a VM in QEMU/KVM (2 vCPU, 512 MB, `timeout -s KILL` around it) and asserts on what the kata-agent stub
prints on the serial console. Gpu, cpu, servicevm-nvl4, gpu with NVL4 switches, nvl5 (against a fake InfiniBand tree,
nvsystemd only: sysfs is read-only so NVRC cannot be given one), smi/dcgm parameters, failure injection (smi, modprobe,
daemon, persistenced), invalid parameters, bad topology, extension fail-closed paths.

## Run the benchmark

```sh
# three columns by default: nvrc, nvsd (Fedora systemd), nvsd-centos10; QEMU pinned to host CPUs 12-15, rounds taken while
# other host load is high are discarded and redone; resumable (rerun the same command after an interruption)
NVSD_CPUS=12-15 python3 bench/run.py --scenarios S0,S1,S2,S3,S4 --runs 30 --label mytest   # raw JSON lines in /home/ecurtin/nvsd-work/bench/raw/
python3 bench/summarize.py mytest --csv bench/results/mytest.csv --md bench/results/mytest.md
python3 bench/report.py mytest --update docs/BENCHMARK.md   # the markdown tables of docs/BENCHMARK.md (needs <!-- TABLES:mytest --> markers)
```

The driver aborts (exit 3) after a round if it holds more than 64 open file descriptors or the system `Shmem` exceeds 4 GiB
(`--max-fds`, `--max-shmem-gb`); boot test `t40` asserts that repeated GPU boots in one process leave no memfd behind. Both exist
because the fake PCI server once leaked every guest's RAM memfd into the benchmark process (see `docs/BENCHMARK.md`, threats to
validity); data collected before the fix (`bench/results/README.md`) is labelled suspect.

See `docs/BENCHMARK.md` for the methodology, the numbers and the verdict.

## Kernel command line reference

| parameter | meaning (as in NVRC) |
|---|---|
| `nvrc.log=off\|error\|warn\|info\|debug\|trace` | any value: `printk_devkmsg=on`; `info`/`debug`/`trace`: log the init digest; `debug`/`trace`: daemon output to `/dev/kmsg` instead of `/dev/null` |
| `nvrc.smi.lmc=<MHz>` `nvrc.smi.lgc=<MHz>` `nvrc.smi.pl=<W>` | `nvidia-smi -lmc/-lgc/-pl <n>` (gpu mode); decimal u32, invalid value powers off |
| `nvrc.smi.srs=on\|off\|1\|0\|true\|false\|yes\|no` | `nvidia-smi conf-compute -srs 1\|0`; any other value powers off |
| `nvrc.uvm.persistence.mode=<bool>` | `--uvm-persistence-mode` for nvidia-persistenced (default on; lenient boolean, unrecognised = off) |
| `nvrc.uvm.tools=<bool>` | create `/dev/nvidia-uvm-tools` (lenient boolean) |
| `nvrc.dcgm=<bool>` | start `nv-hostengine` and `dcgm-exporter` (lenient boolean) |
| `kata.extension.<name>.verity_params=root_hash=..,salt=..,data_blocks=..,data_block_size=..,hash_block_size=..` | verity parameters of the cold-plugged extension device with serial `extension-<name>` |
| `systemd.log_level=`, `systemd.show_status=` | passed to systemd (ignored by NVRC); the benchmark gives the same command line to both |

Duplicated keys: the last occurrence wins. Unknown `nvrc.*` items are ignored.

## Caveats

* No GPU here: all timings are of the init system plus *simulated* tools. Real `modprobe nvidia`, `nvidia-smi`, driver
  initialisation and CDI generation take far longer than any systemd overhead measured here.
* Concurrent `nvidia-smi` calls (nvsystemd runs lmc/lgc/pl in parallel, NVRC serially) were not verified on real hardware.
* The dm-verity/erofs extension mount cannot run in the test VM; only its planning and failure paths are tested.
* The helper is C, not Rust, and runs as root (see `docs/DESIGN.md`).
* systemd is a far larger attack surface than NVRC (see `docs/DESIGN.md`).
* Fedora's systemd binary and libraries come from the build host and CentOS 10's from a podman container; neither is part of
  this repository. The two bases differ in systemd version (259 vs 257), glibc (2.43 vs 2.39) and distribution build flags.
