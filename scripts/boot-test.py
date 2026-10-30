#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Boot tests: boot images in QEMU/KVM and assert on what the serial console shows.

  scripts/boot-test.py [--variant nvsd|nvrc] [-k substring] [-v]

Each test lists the kernel command line, the fake PCI topology, the fixture
config and the assertions. Nothing is reported as passing unless QEMU was booted
and the markers were seen.
"""
import argparse
import os
import sys
import traceback

sys.path.insert(0, __import__("os").path.dirname(__import__("os").path.abspath(__file__)))
import nvsdlib as L  # noqa: E402

TESTS = []


def test(fn):
    TESTS.append(fn)
    return fn


@test
def t00_systemd_reaches_boot_target_and_powers_off(variant, v):
    """commit 0: stock systemd is PID 1, reaches nvsd-boot.target and powers off cleanly."""
    if variant != "nvsd":
        return "skip: nvsd only"
    rep = L.boot("nvsd", append="systemd.show_status=yes", timeout=20)
    assert "Reached target nvsd-boot.target" in rep.text, "target marker missing:\n" + rep.text
    assert "Power down" in rep.text, "VM did not power off:\n" + rep.text
    assert not rep.timed_out


# ---------------------------------------------------------------------------
# Behavioural tests. They are variant independent: the same expectations (derived
# from NVRC's source) must hold for the NVRC baseline and for nvsystemd, except where
# a test calls out a deliberate difference.
# ---------------------------------------------------------------------------
ECHO = "nvstub.echo=1"


def seq_names(rep):
    """Ordered list of 'name event' for stub log entries that matter."""
    keep = {"start", "done", "ready", "FAIL", "BADARGV", "ERROR"}
    return ["%s %s" % (e["name"], e["event"]) for e in rep.log if e["event"] in keep]


def argv_of(rep, name):
    e = rep.first(name, "start")
    assert e, "no start event for %s in %s" % (name, seq_names(rep))
    return e["detail"].replace("argv=", "", 1)


def booted_agent(rep, why=""):
    assert rep.kata is not None, "kata-agent stub never ran%s\n%s" % (why, rep.text[-1500:])
    assert rep.complete


def vm_off_without_agent(rep):
    assert rep.kata is None, "kata-agent must not run after a failure"
    assert "Power down" in rep.text, "VM did not power off:\n" + rep.text[-1500:]
    assert not rep.timed_out


def assert_before(rep, a, b):
    """Stub log event `a` (name, event) happened before `b`."""
    ea, eb = rep.first(*a), rep.first(*b)
    assert ea and eb, "missing %s or %s in %s" % (a, b, seq_names(rep))
    assert ea["t"] <= eb["t"], "%s must come before %s" % (a, b)


def common_agent_checks(rep):
    booted_agent(rep)
    e = rep.env
    assert e["oom_score_adj"] == "-997", e
    assert e["nofile_cur"] == "1048576" and e["nofile_max"] == "1048576", e
    assert e["modules_disabled"] == "1", "lockdown must be applied before kata-agent: %s" % e
    assert e["attester_variant"] == "unset", e


@test
def t01_api_filesystems_and_loopback(variant, v):
    """NVRC mount.rs + net.rs: /proc /sys /run /tmp are mounted (tmp: nosuid,nodev) and lo is up."""
    rep = L.boot(variant)
    booted_agent(rep)
    for mp, fs in (("/proc", "proc"), ("/sys", "sysfs"), ("/run", "tmpfs"), ("/tmp", "tmpfs")):
        assert mp in rep.mounts and rep.mounts[mp][0] == fs, (mp, rep.mounts.get(mp))
    opts = rep.mounts["/tmp"][1]
    assert "nosuid" in opts and "nodev" in opts, opts
    assert int(rep.facts["lo_flags"], 16) & 1, "lo must be UP: %s" % rep.facts


@test
def t02_agent_limits(variant, v):
    """kata-agent: OOM score -997 and RLIMIT_NOFILE 1048576 (NVRC kata_agent.rs agent_setup)."""
    rep = L.boot(variant)
    booted_agent(rep)
    e = rep.env
    assert e["oom_score_adj"] == "-997", e
    assert e["nofile_cur"] == "1048576" and e["nofile_max"] == "1048576", e


@test
def t03_module_lockdown_before_agent(variant, v):
    """lockdown.rs: kernel.modules_disabled=1 is in force when kata-agent starts."""
    rep = L.boot(variant)
    booted_agent(rep)
    assert rep.env["modules_disabled"] == "1", rep.env


@test
def t04_socket_buffer_sysctls_and_log(variant, v):
    """kmsg.rs kernlog_setup(): four net.core buffers are 16 MiB; nvrc.log=<any> turns printk_devkmsg on."""
    rep = L.boot(variant)
    booted_agent(rep)
    for k in ("rmem_default", "rmem_max", "wmem_default", "wmem_max"):
        assert rep.facts[k] == "16777216", rep.facts
    # deliberate difference: systemd itself switches printk_devkmsg to "on" at startup (no rate limit),
    # NVRC leaves the kernel default ("ratelimit") unless nvrc.log is given
    assert rep.facts["printk_devkmsg"] == ("ratelimit" if variant == "nvrc" else "on"), rep.facts
    rep = L.boot(variant, append="nvrc.log=info")
    booted_agent(rep)
    assert rep.facts["printk_devkmsg"] == "on", rep.facts


@test
def t05_init_digest_logged_only_when_logging(variant, v):
    """hash.rs self_exe(): the init's sha256 appears in the kernel log with nvrc.log=info and matches the
    file; with logging off nothing is printed. (NVRC hashes itself; nvsystemd hashes systemd + helper.)"""
    import hashlib
    rep = L.boot(variant, append="nvrc.log=info nvstub.kmsg_grep=sha256=")
    booted_agent(rep)
    want = hashlib.sha256(open(L.NVRC_BIN if variant == "nvrc" else "/usr/lib/systemd/systemd", "rb").read()).hexdigest()
    assert any("sha256=" + want in m for m in rep.kmsg), (want, rep.kmsg)
    rep = L.boot(variant, append="nvstub.kmsg_grep=sha256=")
    booted_agent(rep)
    assert not rep.kmsg, rep.kmsg


@test
def t10_cpu_mode(variant, v):
    """No NVIDIA PCI devices -> cpu mode: nothing but kata-agent runs."""
    rep = L.boot(variant, append=ECHO)
    common_agent_checks(rep)
    assert seq_names(rep) == ["kata-agent start"], seq_names(rep)


@test
def t11_gpu_default_single_gpu(variant, v):
    """One GPU -> gpu mode; NVreg_NvLinkDisable=1 for a single GPU; fixed NVRC order."""
    rep = L.boot(variant, pci=["gpu1"], append=ECHO)
    common_agent_checks(rep)
    assert seq_names(rep) == [
        "modprobe start", "modprobe done", "modprobe start", "modprobe done",
        "nvidia-persistenced start", "nvidia-persistenced ready",
        "nvidia-ctk start", "nvidia-ctk done", "kata-agent start"], seq_names(rep)
    mods = [e["detail"] for e in rep.events("modprobe", "start")]
    assert mods[0].endswith("argv=nvidia NVreg_NvLinkDisable=1"), mods
    assert mods[1].endswith("argv=nvidia-uvm"), mods
    assert argv_of(rep, "nvidia-persistenced") == "--verbose --uvm-persistence-mode"
    assert argv_of(rep, "nvidia-ctk") == "-d cdi generate --output=/var/run/cdi/nvidia.yaml"
    assert not rep.events("nvidia-smi:lmc"), "no smi params -> no nvidia-smi"


@test
def t12_gpu_two_gpus_no_nvlink_disable(variant, v):
    rep = L.boot(variant, pci=["gpu2"], append=ECHO)
    common_agent_checks(rep)
    mods = [e["detail"] for e in rep.events("modprobe", "start")]
    assert mods[0].endswith("argv=nvidia"), mods


@test
def t13_gpu_smi_and_dcgm_params(variant, v):
    """lmc, lgc, pl, srs and dcgm=on; persistence mode off."""
    rep = L.boot(variant, pci=["gpu1"], append=ECHO + " nvrc.smi.lmc=5001 nvrc.smi.lgc=1500 nvrc.smi.pl=300 "
                 "nvrc.smi.srs=1 nvrc.dcgm=on nvrc.uvm.persistence.mode=off")
    common_agent_checks(rep)
    assert argv_of(rep, "nvidia-persistenced") == "--verbose"
    assert argv_of(rep, "nvidia-smi:lmc") == "-lmc 5001"
    assert argv_of(rep, "nvidia-smi:lgc") == "-lgc 1500"
    assert argv_of(rep, "nvidia-smi:pl") == "-pl 300"
    assert argv_of(rep, "nvidia-smi:srs") == "conf-compute -srs 1"
    assert argv_of(rep, "nv-hostengine") == "--service-account nvidia-dcgm --home-dir /tmp"
    assert argv_of(rep, "dcgm-exporter") == "-k -f /etc/dcgm-exporter/default-counters.csv"
    # required ordering (parallelism allowed otherwise): modules before smi, persistenced before smi,
    # everything before kata-agent
    for smi in ("lmc", "lgc", "pl"):
        assert_before(rep, ("modprobe", "done"), ("nvidia-smi:" + smi, "start"))
        assert_before(rep, ("nvidia-persistenced", "ready"), ("nvidia-smi:" + smi, "start"))
        assert_before(rep, ("nvidia-smi:" + smi, "done"), ("nvidia-smi:srs", "start"))
    for name, ev in (("nvidia-smi:srs", "done"), ("nvidia-ctk", "done"), ("nvidia-persistenced", "ready")):
        assert_before(rep, (name, ev), ("kata-agent", "start"))


@test
def t14_srs_off(variant, v):
    rep = L.boot(variant, pci=["gpu1"], append=ECHO + " nvrc.smi.srs=off")
    booted_agent(rep)
    assert argv_of(rep, "nvidia-smi:srs") == "conf-compute -srs 0"


@test
def t15_unrecognised_params_ignored(variant, v):
    """Bare/unknown nvrc.* items are ignored; duplicated keys: last wins; booleans are lenient."""
    rep = L.boot(variant, pci=["gpu1"], append=ECHO + " nvrc.smi.lgc=1000 nvrc.smi.lgc=1200 nvrc.bogus=1 nvrc.dcgm "
                 "nvrc.log=debug")
    booted_agent(rep)
    assert argv_of(rep, "nvidia-smi:lgc") == "-lgc 1200"
    assert not rep.events("nv-hostengine"), "bare nvrc.dcgm must not enable dcgm"


@test
def t16_dcgm_lenient_boolean(variant, v):
    rep = L.boot(variant, pci=["gpu1"], append=ECHO + " nvrc.dcgm=maybe")
    booted_agent(rep)
    assert not rep.events("nv-hostengine"), "unrecognised boolean means false"
    rep = L.boot(variant, pci=["gpu1"], append=ECHO + " nvrc.dcgm=YES")
    booted_agent(rep)
    assert rep.events("nv-hostengine", "start")


@test
def t20_servicevm_nvl4(variant, v):
    """4 NVSwitch, 0 GPU -> servicevm-nvl4: nvidia + fabric manager FABRIC_MODE=1 greedy."""
    rep = L.boot(variant, pci=["nvl4"], append=ECHO)
    common_agent_checks(rep)
    assert seq_names(rep) == ["modprobe start", "modprobe done", "nv-fabricmanager start",
                              "nv-fabricmanager ready", "kata-agent start"], seq_names(rep)
    assert argv_of(rep, "nv-fabricmanager") == "-c /run/fabricmanager.cfg FABRIC_MODE=1 PARTITION_RAIL_POLICY=greedy"


@test
def t21_gpu_with_nvl4_switches(variant, v):
    """8 GPU + 4 NVSwitch -> gpu mode with fabric manager FABRIC_MODE=0."""
    rep = L.boot(variant, pci=["gpu8-nvl4"], append=ECHO)
    common_agent_checks(rep)
    assert argv_of(rep, "nv-fabricmanager") == "-c /run/fabricmanager.cfg FABRIC_MODE=0 PARTITION_RAIL_POLICY=greedy"
    assert rep.events("nvidia-persistenced", "start")
    mods = [e["detail"] for e in rep.events("modprobe", "start")]
    assert mods[0].endswith("argv=nvidia"), mods


@test
def t22_nvl5_without_infiniband_fails_closed(variant, v):
    """4 CX7 SW_MNG NICs (found via PCI VPD) -> servicevm-nvl5. The fake machine has no
    /sys/class/infiniband, so port GUID detection panics and the VM powers off (NVRC behaviour)."""
    rep = L.boot(variant, pci=["nvl5"], append=ECHO)
    vm_off_without_agent(rep)
    names = [e["detail"].split()[0] for e in rep.events("modprobe", "start")]
    assert names == ["module=ib_umad", "module=mlx5_ib"], names


@test
def t30_failure_injection_smi_powers_off(variant, v):
    rep = L.boot(variant, pci=["gpu1"], append=ECHO + " nvrc.smi.lgc=1500 nvstub.fail.nvidia-smi.lgc=3")
    vm_off_without_agent(rep)
    assert "nvidia-smi:lgc FAIL" in rep.text


@test
def t31_failure_injection_modprobe_powers_off(variant, v):
    rep = L.boot(variant, pci=["gpu1"], append=ECHO + " nvstub.fail.modprobe.nvidia-uvm=1")
    vm_off_without_agent(rep)


@test
def t32_failing_daemon_powers_off(variant, v):
    """A daemon that exits non-zero (here nv-hostengine) must power the VM off. NVRC notices in
    health_checks() before handing off to kata-agent, but only if the daemon already died at that
    instant (point-in-time try_wait), so the test slows nvidia-ctk down to make that deterministic.
    nvsystemd uses FailureAction= on the unit and does not depend on timing."""
    rep = L.boot(variant, pci=["gpu1"], append=ECHO + " nvrc.dcgm=on nvstub.fail.nv-hostengine=1 "
                 "nvstub.latency.nvidia-ctk=300", timeout=30)
    vm_off_without_agent(rep)


@test
def t33_bad_topology_powers_off(variant, v):
    """2 NVSwitches match no known topology -> NVRC panics ('unexpected topology')."""
    rep = L.boot(variant, pci=["bad-topology"], append=ECHO)
    vm_off_without_agent(rep)
    assert not rep.log or not any(e["name"] == "modprobe" for e in rep.log)


@test
def t34_invalid_numeric_param_powers_off(variant, v):
    rep = L.boot(variant, pci=["gpu1"], append=ECHO + " nvrc.smi.lgc=fast")
    vm_off_without_agent(rep)


@test
def t35_invalid_srs_powers_off(variant, v):
    rep = L.boot(variant, pci=["gpu1"], append=ECHO + " nvrc.smi.srs=maybe")
    vm_off_without_agent(rep)


@test
def t36_uvm_tools_without_uvm_major_fails_closed(variant, v):
    """nvrc.uvm.tools=on needs the nvidia-uvm major in /proc/devices; the simulated modprobe cannot
    register one, so both inits fail closed and power off."""
    rep = L.boot(variant, pci=["gpu1"], append=ECHO + " nvrc.uvm.tools=on")
    vm_off_without_agent(rep)




def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--variant", default="nvsd", choices=["nvsd", "nvrc"])
    ap.add_argument("-k", default="")
    ap.add_argument("-v", action="store_true")
    ap.add_argument("--all", action="store_true", help="run every test against nvsd, not only the enabled ones")
    a = ap.parse_args()
    L.build_tools()
    enabled = None
    if a.variant == "nvsd" and not a.all:
        # nvsystemd is built up one unit at a time; tests/nvsd-enabled.txt grows with each commit
        with open(os.path.join(L.REPO, "tests", "nvsd-enabled.txt")) as f:
            enabled = {ln.split()[0] for ln in f if ln.strip() and not ln.startswith("#")}
    fails = 0
    for fn in TESTS:
        if a.k and a.k not in fn.__name__:
            continue
        if enabled is not None and fn.__name__.split("_")[0] not in enabled:
            continue
        try:
            r = fn(a.variant, a.v)
            print("%-4s %s%s" % ("SKIP" if r else "PASS", fn.__name__, " (%s)" % r if r else ""))
        except Exception:
            fails += 1
            print("FAIL %s" % fn.__name__)
            traceback.print_exc()
    print("%d failed" % fails if fails else "all passed")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
