# SPDX-License-Identifier: Apache-2.0
"""Adds the nvsystemd layer on top of the systemd-only image: everything under
overlay/ (unit files, system.conf, os-release, machine-id, ...) plus the helper."""
import os
import stat


def ensure_dirs(c, path):
    parts = path.strip("/").split("/")[:-1]
    cur = ""
    for p in parts:
        cur += "/" + p
        if cur.lstrip("/") not in c.seen:
            c.dir(cur)


def apply(c, repo, out):
    root = os.path.join(repo, "overlay")
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames.sort()
        rel = "/" + os.path.relpath(dirpath, root).strip(".").strip("/")
        for d in sorted(dirnames):
            full = os.path.join(dirpath, d)
            p = os.path.join(rel, d)
            if os.path.islink(full):
                ensure_dirs(c, p)
                c.symlink(p, os.readlink(full))
            elif p.lstrip("/") not in c.seen:
                ensure_dirs(c, p)
                c.dir(p)
        for f in sorted(filenames):
            full = os.path.join(dirpath, f)
            p = os.path.join(rel, f)
            ensure_dirs(c, p)
            if os.path.islink(full):
                c.symlink(p, os.readlink(full))
            else:
                mode = 0o755 if os.stat(full).st_mode & stat.S_IXUSR else 0o644
                with open(full, "rb") as fh:
                    c.file(p, fh.read(), mode)
    helper = os.path.join(out, "nvsd-helper")
    if os.path.exists(helper):
        ensure_dirs(c, "/usr/lib/nvsd/x")
        with open(helper, "rb") as fh:
            c.file("/usr/lib/nvsd/nvsd-helper", fh.read(), 0o755)
