#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""fakepci: SIMULATED PCI devices for QEMU via the vfio-user protocol.

NVRC (and nvsystemd) pick their operating mode by scanning
/sys/bus/pci/devices for vendor 0x10de (class 0x03xx = GPU, 0x0680 = NVSwitch)
and vendor 0x15b3 with "SW_MNG" in the PCI VPD (CX7 NVLink management NIC).
QEMU cannot emulate those IDs natively, so this tiny vfio-user server serves a
config space with arbitrary vendor/device/class (and optionally a VPD
capability) to QEMU's vfio-user-pci client. There are no BARs, no interrupts
and no DMA: the guest only sees "a PCI device with these IDs".

Only the vfio-user message subset QEMU needs to hot-plug a config-space-only
device is implemented: VERSION, DEVICE_GET_INFO, DEVICE_GET_REGION_INFO,
GET_IRQ_INFO, SET_IRQS, REGION_READ/WRITE (config space only), DMA_MAP/UNMAP
(ignored), DEVICE_RESET.

Device spec syntax (repeatable): vendor:device:class[:vpd=<text>]
  e.g. 10de:2330:030200 (an H100-like 3D controller), 10de:22a3:068000
  (NVSwitch), 15b3:1021:020700:vpd=SW_MNG
"""
import json
import os
import socket
import struct
import sys
import threading

HDR = struct.Struct("<HHIII")  # msg_id, cmd, size, flags, error
CMD_VERSION, CMD_DMA_MAP, CMD_DMA_UNMAP, CMD_GET_INFO, CMD_GET_REGION_INFO = 1, 2, 3, 4, 5
CMD_GET_IO_FDS, CMD_GET_IRQ_INFO, CMD_SET_IRQS, CMD_REGION_READ, CMD_REGION_WRITE = 6, 7, 8, 9, 10
CMD_DMA_READ, CMD_DMA_WRITE, CMD_RESET = 11, 12, 13
F_REPLY, F_NOREPLY, F_ERROR = 1, 1 << 4, 1 << 5
CFG_REGION = 7
VPD_CAP = 0x40


def build_config(vendor, device, cls, vpd=None):
    c = bytearray(256)
    struct.pack_into("<HH", c, 0, vendor, device)
    struct.pack_into("<H", c, 4, 0)  # command
    status = 0x0010 if vpd else 0  # capabilities list
    struct.pack_into("<H", c, 6, status)
    c[8] = 1  # revision
    c[9] = cls & 0xFF  # prog-if
    c[10] = (cls >> 8) & 0xFF  # subclass
    c[11] = (cls >> 16) & 0xFF  # base class
    c[14] = 0  # header type 0
    struct.pack_into("<HH", c, 0x2C, vendor, device)  # subsystem ids
    if vpd:
        c[0x34] = VPD_CAP
        c[VPD_CAP] = 0x03  # VPD capability id
        c[VPD_CAP + 1] = 0  # next
    return c


def vpd_blob(text):
    # large resource string tag (0x82) "identifier string", then read-only tag with
    # one keyword carrying the marker, then the end tag. Enough for sysfs "vpd" to
    # return bytes containing the marker; the kernel trims to the end tag.
    ident = b"fakepci"
    ro = b"V0" + bytes([len(text)]) + text.encode()
    blob = b"\x82" + struct.pack("<H", len(ident)) + ident
    blob += b"\x90" + struct.pack("<H", len(ro)) + ro
    blob += b"\x78"
    return blob


class Device(threading.Thread):
    def __init__(self, path, vendor, device, cls, vpd=None):
        super().__init__(daemon=True)
        self.path = path
        self.cfg = build_config(vendor, device, cls, vpd)
        self.vpd = vpd_blob(vpd) if vpd else None
        self.vpd_data = 0
        if os.path.exists(path):
            os.unlink(path)
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.bind(path)
        self.sock.listen(1)

    def run(self):
        try:
            conn, _ = self.sock.accept()
        except OSError:
            return
        with conn:
            try:
                self.serve(conn)
            except (OSError, ConnectionError, struct.error):
                pass

    def recv_exact(self, conn, n):
        buf = b""
        while len(buf) < n:
            chunk = conn.recv(n - len(buf))
            if not chunk:
                raise ConnectionError("closed")
            buf += chunk
        return buf

    def serve(self, conn):
        while True:
            # first read of a message may carry SCM_RIGHTS fds (DMA_MAP): use recvmsg
            data, anc, _fl, _ad = conn.recvmsg(HDR.size, socket.CMSG_LEN(16 * 4))
            # DMA_MAP hands over a dup of QEMU's guest RAM memfd. We never map it, so close it at once: leaving it
            # open kept every booted guest's 512 MiB of shared memory alive for as long as the benchmark ran (tens of
            # GiB over a full run, which exhausted the host).
            for level, kind, cdata in anc:
                if level == socket.SOL_SOCKET and kind == socket.SCM_RIGHTS:
                    for fd in struct.unpack("%di" % (len(cdata) // 4), cdata[:len(cdata) // 4 * 4]):
                        os.close(fd)
            if not data:
                return
            while len(data) < HDR.size:
                data += self.recv_exact(conn, HDR.size - len(data))
            msg_id, cmd, size, flags, _err = HDR.unpack(data)
            payload = self.recv_exact(conn, size - HDR.size) if size > HDR.size else b""
            self.handle(conn, msg_id, cmd, flags, payload)

    def reply(self, conn, msg_id, cmd, payload=b"", err=0):
        flags = F_REPLY | (F_ERROR if err else 0)
        conn.sendall(HDR.pack(msg_id, cmd, HDR.size + len(payload), flags, err) + payload)

    def handle(self, conn, msg_id, cmd, flags, p):
        noreply = bool(flags & F_NOREPLY)
        if cmd == CMD_VERSION:
            caps = {"capabilities": {"max_msg_fds": 1, "max_data_xfer_size": 1048576,
                                     "migration": {"pgsize": 4096}}}
            body = struct.pack("<HH", 0, 0) + json.dumps(caps).encode() + b"\0"
            self.reply(conn, msg_id, cmd, body)
        elif cmd == CMD_GET_INFO:
            # argsz, flags(PCI=1<<1), num_regions=9, num_irqs=5
            self.reply(conn, msg_id, cmd, struct.pack("<IIII", 16, 1 << 1, 9, 5))
        elif cmd == CMD_GET_REGION_INFO:
            argsz, _fl, index, _cap, _size, _off = struct.unpack("<IIIIQQ", p[:32])
            if index == CFG_REGION:
                body = struct.pack("<IIIIQQ", 32, 3, index, 0, len(self.cfg), 0)
            else:
                body = struct.pack("<IIIIQQ", 32, 0, index, 0, 0, 0)
            self.reply(conn, msg_id, cmd, body)
        elif cmd == CMD_GET_IRQ_INFO:
            argsz, _fl, index, _cnt = struct.unpack("<IIII", p[:16])
            self.reply(conn, msg_id, cmd, struct.pack("<IIII", 16, 0, index, 0))
        elif cmd == CMD_SET_IRQS:
            self.reply(conn, msg_id, cmd)
        elif cmd == CMD_REGION_READ:
            off, region, count = struct.unpack("<QII", p[:16])
            if region != CFG_REGION or off + count > len(self.cfg):
                self.reply(conn, msg_id, cmd, err=22)
                return
            self.reply(conn, msg_id, cmd, struct.pack("<QII", off, region, count) + self.read_cfg(off, count))
        elif cmd == CMD_REGION_WRITE:
            off, region, count = struct.unpack("<QII", p[:16])
            if region != CFG_REGION or off + count > len(self.cfg):
                self.reply(conn, msg_id, cmd, err=22)
                return
            self.write_cfg(off, p[16:16 + count])
            self.reply(conn, msg_id, cmd, struct.pack("<QII", off, region, count))
        elif cmd == CMD_DMA_MAP:
            if not noreply:
                self.reply(conn, msg_id, cmd)
        elif cmd == CMD_DMA_UNMAP:
            if not noreply:
                self.reply(conn, msg_id, cmd, p)
        elif cmd == CMD_RESET:
            self.reply(conn, msg_id, cmd)
        else:
            self.reply(conn, msg_id, cmd, err=38)

    # --- config space, with a minimal VPD capability state machine ---
    def read_cfg(self, off, count):
        if self.vpd is not None and off <= VPD_CAP + 8 and off + count > VPD_CAP + 4:
            c = bytearray(self.cfg)
            struct.pack_into("<I", c, VPD_CAP + 4, self.vpd_data)
            return bytes(c[off:off + count])
        return bytes(self.cfg[off:off + count])

    def write_cfg(self, off, data):
        if off in (0x04, 0x05) or (0x0C <= off < 0x10) or off == 0x3C:
            self.cfg[off:off + len(data)] = data
            return
        if self.vpd is not None and VPD_CAP + 2 <= off < VPD_CAP + 4:
            addr = struct.unpack_from("<H", bytes(self.cfg[VPD_CAP + 2:VPD_CAP + 4]) if off != VPD_CAP + 2 or len(data) < 2 else data)[0]
            write = bool(addr & 0x8000)
            a = addr & 0x7FFF
            if not write:
                chunk = (self.vpd + b"\0" * 4)[a:a + 4].ljust(4, b"\0")
                self.vpd_data = struct.unpack("<I", chunk)[0]
                # flag bit set means "data ready"
                struct.pack_into("<H", self.cfg, VPD_CAP + 2, (a | 0x8000))
            else:
                struct.pack_into("<H", self.cfg, VPD_CAP + 2, a)  # writes ignored, flag cleared = done
            return
        # everything else (BARs, etc.) is read-only zero


def parse_spec(spec):
    parts = spec.split(":")
    vpd = None
    for extra in parts[3:]:
        if extra.startswith("vpd="):
            vpd = extra[4:]
    return int(parts[0], 16), int(parts[1], 16), int(parts[2], 16), vpd


def start_devices(specs, sockdir):
    """Start servers for spec strings; returns (devices, qemu_args)."""
    devs, args = [], []
    for i, spec in enumerate(specs):
        v, d, c, vpd = parse_spec(spec)
        path = os.path.join(sockdir, "fakepci%d.sock" % i)
        dev = Device(path, v, d, c, vpd)
        dev.start()
        devs.append(dev)
        args += ["-device", json.dumps({"driver": "vfio-user-pci", "socket": {"type": "unix", "path": path}})]
    return devs, args


if __name__ == "__main__":
    # standalone: print the qemu args and serve until killed
    import tempfile
    d = tempfile.mkdtemp(prefix="fakepci-")
    devs, args = start_devices(sys.argv[1:], d)
    print(" ".join(args), flush=True)
    for dv in devs:
        dv.join()
