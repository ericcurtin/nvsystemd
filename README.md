# nvsystemd

**What this is.** A version of [NVIDIA/NVRC](https://github.com/NVIDIA/nvrc) (a ~5k-line Rust PID 1 for ephemeral
NVIDIA GPU micro VMs under Kata Containers) that uses **Fedora's unmodified systemd** (`/usr/lib/systemd/systemd`,
259.10-1.fc44, plus the shared libraries `ldd` reports, copied from the build host) as PID 1. NVRC's job is expressed
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

Licence: Apache-2.0 (`LICENSE`), `NOTICE` credits NVIDIA NVRC. Fedora's systemd (LGPL-2.1-or-later) is **not
redistributed**: it is copied from the host into images that stay in the (git-ignored) scratch directory.

## Layout

```
overlay/etc/systemd/system/   the units (the product)
overlay/usr/lib/nvsd/         static data (ld.so.conf for the gpu extension)
helper/nvsd-helper.c          the static helper (libc-free C)
common/mini.h                 syscall layer shared by helper and fixture
fixture/stub/nvstub.c         simulated NVIDIA userspace + kata-agent stub (end marker, records timing and memory)
fixture/fakepci/fakepci.py    vfio-user server that fakes PCI devices (GPU, NVSwitch, CX7 with SW_MNG VPD)
scripts/                      image builder (no root needed), QEMU runner, boot tests, NVRC build
bench/                        run.py (interleaved A/B), summarize.py, results/ (small CSV/markdown)
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
```

Images are built on demand by the tests and the benchmark (`scripts/mkimage.py`, pure Python cpio writer, no root).
The kernel is the host's `/lib/modules/7.2.13-200.fc44.x86_64/vmlinuz`, used directly with `-kernel`/`-initrd`.

## Test

```sh
scripts/boot-test.sh                    # nvsystemd, the tests enabled in tests/nvsd-enabled.txt
scripts/boot-test.sh --all              # every test against nvsystemd
scripts/boot-test.sh --variant nvrc     # the same expectations against the NVRC baseline
scripts/boot-test.sh -k t13 -v          # one test
```

Every test boots a VM in QEMU/KVM (2 vCPU, 512 MB, `timeout -s KILL` around it) and asserts on what the kata-agent stub
prints on the serial console. Gpu, cpu, servicevm-nvl4, gpu with NVL4 switches, nvl5 (against a fake InfiniBand tree,
nvsystemd only: sysfs is read-only so NVRC cannot be given one), smi/dcgm parameters, failure injection (smi, modprobe,
daemon, persistenced), invalid parameters, bad topology, extension fail-closed paths.

## Run the benchmark

```sh
python3 bench/run.py --scenarios S0,S1,S2,S3,S4 --runs 30 --label mytest     # raw JSON lines in /home/ecurtin/nvsd-work/bench/raw/
python3 bench/summarize.py mytest --csv bench/results/mytest.csv --md bench/results/mytest.md
```

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
* Fedora's systemd binary and libraries come from the build host and are not part of this repository.
