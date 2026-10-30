#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Boot tests: boot images in QEMU/KVM and assert on what the serial console shows.

  scripts/boot-test.py [--variant nvsd|nvrc] [-k substring] [-v]

Each test lists the kernel command line, the fake PCI topology, the fixture
config and the assertions. Nothing is reported as passing unless QEMU was booted
and the markers were seen.
"""
import argparse
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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--variant", default="nvsd", choices=["nvsd", "nvrc"])
    ap.add_argument("-k", default="")
    ap.add_argument("-v", action="store_true")
    a = ap.parse_args()
    L.build_tools()
    fails = 0
    for fn in TESTS:
        if a.k and a.k not in fn.__name__:
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
