/* SPDX-License-Identifier: Apache-2.0
 *
 * nvsd-helper: the only non-systemd, non-fixture binary in the nvsystemd image.
 *
 * A static, libc-free (see common/mini.h) multi-subcommand helper. Every
 * subcommand exists because neither systemd nor the binaries already in the image
 * can do the job; docs/MAPPING.md justifies each one and names the NVRC function it
 * replaces. Each is run by exactly one unit (ExecStart=/usr/lib/nvsd/nvsd-helper <sub>).
 *
 * Failure convention (NVRC "fail fast"): print a line to /dev/kmsg and exit 1; the
 * unit's FailureAction=poweroff-immediate turns that into a VM power-off.
 */
#define MINI_IMPL
#include "../common/mini.h"

#define RUN "/run/nvsd"

static char g_cmdline[4096];

static void kmsg_log(const char *sub, const char *msg) {
	char l[512];
	struct buf b = BUF_INIT(l);
	b_str(&b, "<3>nvsd-helper "); b_str(&b, sub); b_str(&b, ": "); b_str(&b, msg); b_char(&b, '\n'); b_term(&b);
	write_file("/dev/kmsg", l, b.len, 0, 0);
	sys_write(2, l + 3, b.len - 3);
}

static int die(const char *sub, const char *msg) {
	kmsg_log(sub, msg);
	return 1;
}

static void ensure_run_dir(void) { sys_mkdir(RUN, 0755); }

/* write RUN/<name> with content (truncate) */
static int put(const char *name, const char *content, int mode) {
	char p[128];
	struct buf b = BUF_INIT(p);
	b_str(&b, RUN "/"); b_str(&b, name); b_term(&b);
	long r = write_file(p, content, slen(content), O_CREAT | O_TRUNC, mode);
	return r < 0 ? -1 : 0;
}

static int ci_eq(const char *a, const char *b) {
	for (;; a++, b++) {
		char x = *a, y = *b;
		if (x >= 'A' && x <= 'Z') x += 32;
		if (y >= 'A' && y <= 'Z') y += 32;
		if (x != y) return 0;
		if (!x) return 1;
	}
}

/* NVRC kernel_params.rs parse_boolean(): lenient, unrecognised -> false */
static int parse_bool_lenient(const char *v) {
	return ci_eq(v, "on") || ci_eq(v, "true") || ci_eq(v, "1") || ci_eq(v, "yes");
}
/* parse_boolean_strict(): returns 1/0, or -1 if unrecognised */
static int parse_bool_strict(const char *v) {
	if (parse_bool_lenient(v)) return 1;
	if (ci_eq(v, "off") || ci_eq(v, "false") || ci_eq(v, "0") || ci_eq(v, "no")) return 0;
	return -1;
}

/* ------------------------------------------------------------------ cmdline
 * Replaces NVRC kernel_params.rs (process_kernel_params), kmsg.rs kernlog_setup()
 * sysctls, and execute.rs's choice of stdout sink (/dev/kmsg when debug logging,
 * else /dev/null). Translates nvrc.* kernel parameters into files under /run/nvsd
 * that units consume through ConditionPathExists=/EnvironmentFile=:
 *
 *   smi.lmc smi.lgc smi.pl smi.srs   "NVSD_VALUE=<n>"  (present only when the param is set)
 *   dcgm                             empty marker       (nvrc.dcgm true)
 *   uvm-tools                        empty marker       (nvrc.uvm.tools true)
 *   persistenced.env                 NVSD_ARGS="--verbose[ --uvm-persistence-mode]"
 *   out                              symlink to /dev/kmsg (nvrc.log=debug|trace) or /dev/null
 */
static int cmd_cmdline(void) {
	char v[128];
	ensure_run_dir();

	/* kernlog_setup(): four net.core buffers to 16 MiB */
	static const char *const S[] = { "/proc/sys/net/core/rmem_default", "/proc/sys/net/core/wmem_default",
					 "/proc/sys/net/core/rmem_max", "/proc/sys/net/core/wmem_max" };
	for (int i = 0; i < 4; i++)
		if (write_file(S[i], "16777216", 8, 0, 0) < 0) return die("cmdline", "cannot write net.core sysctl");

	int dbg = 0;
	if (cmdline_get(g_cmdline, "nvrc.log", v, sizeof(v))) {
		if (ci_eq(v, "debug") || ci_eq(v, "trace")) dbg = 1;
		if (write_file("/proc/sys/kernel/printk_devkmsg", "on\n", 3, 0, 0) < 0)
			return die("cmdline", "cannot write printk_devkmsg");
	}
	sys_unlink(RUN "/out");
	if (sys_symlink(dbg ? "/dev/kmsg" : "/dev/null", RUN "/out") < 0) return die("cmdline", "cannot create out symlink");

	int uvm_pm = 1;
	if (cmdline_get(g_cmdline, "nvrc.uvm.persistence.mode", v, sizeof(v))) uvm_pm = parse_bool_lenient(v);
	put("persistenced.env", uvm_pm ? "NVSD_ARGS=\"--verbose --uvm-persistence-mode\"\n" : "NVSD_ARGS=\"--verbose\"\n", 0644);

	if (cmdline_get(g_cmdline, "nvrc.uvm.tools", v, sizeof(v)) && parse_bool_lenient(v)) put("uvm-tools", "", 0644);
	if (cmdline_get(g_cmdline, "nvrc.dcgm", v, sizeof(v)) && parse_bool_lenient(v)) put("dcgm", "", 0644);

	if (cmdline_get(g_cmdline, "nvrc.smi.srs", v, sizeof(v))) {
		int r = parse_bool_strict(v);
		if (r < 0) return die("cmdline", "nvrc.smi.srs: unrecognized boolean");
		put("smi.srs", r ? "NVSD_VALUE=1\n" : "NVSD_VALUE=0\n", 0644);
	}
	static const struct { const char *key, *file; } N[] = {
		{ "nvrc.smi.lgc", "smi.lgc" }, { "nvrc.smi.lmc", "smi.lmc" }, { "nvrc.smi.pl", "smi.pl" } };
	for (int i = 0; i < 3; i++) {
		if (!cmdline_get(g_cmdline, N[i].key, v, sizeof(v))) continue;
		u32 n;
		if (!parse_u32(v, &n)) return die("cmdline", "invalid numeric nvidia-smi parameter");
		char c[40];
		struct buf b = BUF_INIT(c);
		b_str(&b, "NVSD_VALUE="); b_u64(&b, n); b_char(&b, '\n'); b_term(&b);
		put(N[i].file, c, 0644);
	}
	return 0;
}


/* ------------------------------------------------------------------ hash
 * NVRC hash.rs self_exe(): "NVRC version=.. sha256=.." in the kernel log so an operator can match a
 * running init to its release digest. NVRC hashes /proc/self/exe; the PID 1 here is Fedora's
 * systemd, so hash the binaries that make up the init (systemd, its executor, this helper).
 * Informational only, exactly as in NVRC (not a security primitive). */
struct sha256 { u32 h[8]; u8 buf[64]; u64 len; u32 fill; };
static const u32 K256[64] = {
	0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,
	0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
	0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,
	0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
	0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,
	0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2 };
#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
static void sha_block(struct sha256 *c, const u8 *p) {
	u32 w[64], a, b, cc, d, e, f, g, h;
	for (int i = 0; i < 16; i++) w[i] = (u32)p[4*i] << 24 | (u32)p[4*i+1] << 16 | (u32)p[4*i+2] << 8 | p[4*i+3];
	for (int i = 16; i < 64; i++) {
		u32 s0 = ROR(w[i-15], 7) ^ ROR(w[i-15], 18) ^ (w[i-15] >> 3);
		u32 s1 = ROR(w[i-2], 17) ^ ROR(w[i-2], 19) ^ (w[i-2] >> 10);
		w[i] = w[i-16] + s0 + w[i-7] + s1;
	}
	a = c->h[0]; b = c->h[1]; cc = c->h[2]; d = c->h[3]; e = c->h[4]; f = c->h[5]; g = c->h[6]; h = c->h[7];
	for (int i = 0; i < 64; i++) {
		u32 S1 = ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25), ch = (e & f) ^ (~e & g);
		u32 t1 = h + S1 + ch + K256[i] + w[i];
		u32 S0 = ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22), mj = (a & b) ^ (a & cc) ^ (b & cc);
		u32 t2 = S0 + mj;
		h = g; g = f; f = e; e = d + t1; d = cc; cc = b; b = a; a = t1 + t2;
	}
	c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d; c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += h;
}
static void sha_init(struct sha256 *c) {
	static const u32 I[8] = { 0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19 };
	for (int i = 0; i < 8; i++) c->h[i] = I[i];
	c->len = 0; c->fill = 0;
}
static void sha_update(struct sha256 *c, const u8 *p, usize n) {
	c->len += n;
	while (n) {
		u32 t = 64 - c->fill; if (t > n) t = (u32)n;
		memcpy(c->buf + c->fill, p, t); c->fill += t; p += t; n -= t;
		if (c->fill == 64) { sha_block(c, c->buf); c->fill = 0; }
	}
}
static void sha_final(struct sha256 *c, char hex[65]) {
	u64 bits = c->len * 8;
	u8 pad = 0x80;
	sha_update(c, &pad, 1);
	u8 z = 0;
	while (c->fill != 56) sha_update(c, &z, 1);
	u8 lb[8];
	for (int i = 0; i < 8; i++) lb[i] = (u8)(bits >> (56 - 8 * i));
	sha_update(c, lb, 8);
	struct buf b = { hex, 0, 65 };
	for (int i = 0; i < 8; i++) b_hex(&b, c->h[i], 8);
	b_term(&b);
}
static int sha_file(const char *path, char hex[65]) {
	static u8 chunk[65536];
	long fd = sys_open(path, O_RDONLY | O_CLOEXEC, 0);
	if (fd < 0) return -1;
	struct sha256 c; sha_init(&c);
	for (;;) {
		long r = sys_read((int)fd, chunk, sizeof(chunk));
		if (r == -EINTR) continue;
		if (r <= 0) break;
		sha_update(&c, chunk, (usize)r);
	}
	sys_close((int)fd);
	sha_final(&c, hex);
	return 0;
}

static int cmd_hash(int argc, char **argv) {
	for (int i = 2; i < argc; i++) {
		char hex[65], l[256];
		if (sha_file(argv[i], hex) < 0) return die("hash", "cannot read file");
		struct buf b = BUF_INIT(l);
		b_str(&b, "<6>nvsystemd sha256="); b_str(&b, hex); b_str(&b, " path="); b_str(&b, argv[i]); b_char(&b, '\n'); b_term(&b);
		write_file("/dev/kmsg", l, b.len, 0, 0);
	}
	return 0;
}

/* ------------------------------------------------------------------ syslog / wait-marker
 * NVRC syslog.rs + kmsg.rs wait_for_marker_after(): NVRC binds /dev/log itself, drains it into
 * /run/syslog.log and blocks until a daemon logs a readiness marker ("Local RPC services
 * initialized" for nvidia-persistenced, "FM starting NvLink Inband" for the fabric manager).
 * systemd has no syslog sink without journald, and no "wait for this text" primitive.
 *
 *   syslog                 run by nvsd-syslog.service; the socket /dev/log is owned by
 *                          nvsd-syslog.socket and arrives as fd 3 (LISTEN_FDS), so datagrams
 *                          sent before this process exists are not lost.
 *   wait-marker TEXT SECS  run as ExecStartPost= of a daemon unit; returns 0 once TEXT is in
 *                          /run/syslog.log (the whole file is scanned first, so a marker that
 *                          arrived early is never missed), 1 on timeout. inotify, no polling.
 */
#define SYSLOG_FILE "/run/syslog.log"

static int cmd_syslog(void) {
	static char m[8192];
	for (;;) {
		long n = sys_read(3, m, sizeof(m) - 2);
		if (n == -EINTR) continue;
		if (n < 0) return die("syslog", "read from /dev/log socket failed");
		/* strip the "<pri>" prefix (NVRC strip_priority) and make it a line */
		char *p = m;
		if (n > 0 && *p == '<') { char *q = p; while (q < m + n && *q != '>') q++; if (q < m + n) p = q + 1; }
		usize len = (usize)(m + n - p);
		while (len && (p[len - 1] == '\n' || p[len - 1] == '\0')) len--;
		p[len++] = '\n';
		if (append_file(SYSLOG_FILE, p, len, 0600) < 0) return die("syslog", "cannot append to " SYSLOG_FILE);
	}
}

static int file_has(const char *path, const char *needle, usize nl) {
	static char buf[65536];
	long fd = sys_open(path, O_RDONLY | O_CLOEXEC, 0);
	if (fd < 0) return 0;
	int hit = 0;
	usize have = 0;
	for (;;) {
		long r = sys_read((int)fd, buf + have, sizeof(buf) - 1 - have);
		if (r == -EINTR) continue;
		if (r <= 0) break;
		have += (usize)r;
		buf[have] = 0;
		if (sfind(buf, needle)) { hit = 1; break; }
		/* keep a tail that could hold the start of a marker split across reads */
		if (have > nl) { memmove(buf, buf + have - nl, nl); have = nl; }
	}
	sys_close((int)fd);
	return hit;
}

static int cmd_wait_marker(int argc, char **argv) {
	if (argc != 4) return die("wait-marker", "usage: wait-marker TEXT SECONDS");
	const char *text = argv[2];
	u32 secs;
	if (!parse_u32(argv[3], &secs)) return die("wait-marker", "bad timeout");
	usize nl = slen(text);
	/* the file may not exist yet: create it so inotify has something to watch (NVRC open_kmsg) */
	write_file(SYSLOG_FILE, "", 0, O_CREAT, 0600);
	long in = sc1(SYS_inotify_init1, O_CLOEXEC);
	if (in < 0) return die("wait-marker", "inotify_init1 failed");
	/* IN_MODIFY | IN_CLOSE_WRITE */
	if (sc3(SYS_inotify_add_watch, in, (long)SYSLOG_FILE, 0x2 | 0x8) < 0) return die("wait-marker", "inotify_add_watch failed");
	struct timespec_ t0, t;
	sys_clock(CLOCK_MONOTONIC, &t0);
	for (;;) {
		if (file_has(SYSLOG_FILE, text, nl)) return 0;
		sys_clock(CLOCK_MONOTONIC, &t);
		i64 left = (i64)secs * 1000 - ((t.sec - t0.sec) * 1000 + (t.nsec - t0.nsec) / 1000000);
		if (left <= 0) return die("wait-marker", "timeout waiting for marker");
		long r = sys_poll_fd((int)in, 1, left > 500 ? 500 : (int)left);
		if (r > 0) { char ev[512]; sys_read((int)in, ev, sizeof(ev)); }
	}
}

/* ------------------------------------------------------------------ detect
 * Replaces NVRC mode.rs detect() (+ the single-GPU test in modprobe.rs). Scans
 * /sys/bus/pci/devices exactly like NVRC (NVIDIA vendor 0x10de: class 0x0680 = NVSwitch,
 * 0x03xx = GPU; Mellanox 0x15b3 with "SW_MNG" in the PCI VPD = NVLink management NIC),
 * applies the same (switches, gpus, sw_mng) decision table, and records the result as
 * marker files that unit conditions test (systemd cannot look inside sysfs files):
 *
 *   mode.cpu | mode.gpu | mode.servicevm-nvl4 | mode.servicevm-nvl5   the NVRC mode
 *   load.nvidia  load.nvidia-uvm   modules NVRC's mode function would modprobe
 *   nvl5                           ib_umad/mlx5_ib/port GUID/nvlsm are needed
 *   fm  + fm.env                   fabric manager runs; NVSD_FM_MODE / NVSD_FM_POLICY
 *   modprobe-nvidia.env            NVSD_PARAMS=NVreg_NvLinkDisable=1 iff exactly one GPU
 *
 * An unmatched topology is NVRC's "unexpected topology" panic: fail fast.
 */
static int vpd_has_sw_mng(const char *dev) {
	static char v[65536];
	char p[300];
	path_join3(p, sizeof(p), "/sys/bus/pci/devices", dev, "vpd");
	long fd = sys_open(p, O_RDONLY | O_CLOEXEC, 0);
	if (fd < 0) return 0;
	usize n = 0;
	for (;;) {
		long r = sys_read((int)fd, v + n, sizeof(v) - n);
		if (r == -EINTR) continue;
		if (r <= 0) break;
		n += (usize)r;
		if (n >= sizeof(v)) break;
	}
	sys_close((int)fd);
	for (usize i = 0; i + 6 <= n; i++)
		if (!memcmp(v + i, "SW_MNG", 6)) return 1;
	return 0;
}

static int count_sw_mng(void) {
	struct dirit d;
	int n = 0;
	if (dir_open(&d, "/sys/bus/pci/devices") < 0) return 0;
	const char *name;
	while ((name = dir_next(&d))) {
		char p[300], v[64];
		path_join3(p, sizeof(p), "/sys/bus/pci/devices", name, "vendor");
		if (read_file(p, v, sizeof(v)) < 0) continue;
		rtrim(v);
		if (seq(v, "0x15b3") && vpd_has_sw_mng(name)) n++;
	}
	dir_close(&d);
	return n;
}

static int cmd_detect(void) {
	const char *PCI = "/sys/bus/pci/devices";
	int sw = pci_count(PCI, "0x10de", "0x0680");
	int gpus = pci_count(PCI, "0x10de", "0x03");
	int mng = count_sw_mng();
	const char *mode, *nvswitch = NULL;
	if (sw == 0 && gpus == 0 && mng == 0) mode = "cpu";
	else if (sw == 0 && mng == 0) mode = "gpu";
	else if (sw == 4 && gpus == 8 && mng == 0) { mode = "gpu"; nvswitch = "nvl4"; }
	else if (sw == 4 && gpus == 0 && mng == 0) { mode = "servicevm-nvl4"; nvswitch = "nvl4"; }
	else if (sw == 0 && gpus == 8 && mng == 4) { mode = "gpu"; nvswitch = "nvl5"; }
	else if (sw == 0 && gpus == 0 && mng == 4) { mode = "servicevm-nvl5"; nvswitch = "nvl5"; }
	else {
		char m[200];
		struct buf b = BUF_INIT(m);
		b_str(&b, "unexpected topology: "); b_u64(&b, (u64)sw); b_str(&b, " NVSWITCH, "); b_u64(&b, (u64)gpus);
		b_str(&b, " GPU, "); b_u64(&b, (u64)mng); b_str(&b, " PCI_SW_MNG - cannot determine mode"); b_term(&b);
		return die("detect", m);
	}
	ensure_run_dir();
	char f[64];
	struct buf b = BUF_INIT(f);
	b_str(&b, "mode."); b_str(&b, mode); b_term(&b);
	put(f, "", 0644);
	int svc = mode[0] == 's';
	int is_gpu = seq(mode, "gpu");
	if (is_gpu || seq(mode, "servicevm-nvl4")) put("load.nvidia", "", 0644);
	if (is_gpu) put("load.nvidia-uvm", "", 0644);
	if (nvswitch) {
		int nvl5 = seq(nvswitch, "nvl5");
		if (nvl5) put("nvl5", "", 0644);
		put("fm", "", 0644);
		/* FABRIC_MODE_SHARED (1) for the service VMs, FABRIC_MODE_FULL (0) for gpu mode;
		 * NVL4 uses "greedy", NVL5 "symmetric" (required for CC on Blackwell) */
		put("fm.env", svc ? (nvl5 ? "NVSD_FM_MODE=1\nNVSD_FM_POLICY=symmetric\n" : "NVSD_FM_MODE=1\nNVSD_FM_POLICY=greedy\n")
				  : (nvl5 ? "NVSD_FM_MODE=0\nNVSD_FM_POLICY=symmetric\n" : "NVSD_FM_MODE=0\nNVSD_FM_POLICY=greedy\n"), 0644);
	}
	if (gpus == 1) put("modprobe-nvidia.env", "NVSD_PARAMS=NVreg_NvLinkDisable=1\n", 0644);
	return 0;
}

/* ------------------------------------------------------------------ lockdown
 * NVRC lockdown.rs disable_modules_loading(): kernel.modules_disabled=1, one way. A missing
 * sysctl (kernel without module support) is fine, any other error is fatal.
 * systemd has no directive that writes a sysctl at a chosen point of the boot (systemd-sysctl
 * would apply at its own fixed time and costs a second large binary). */
static int cmd_lockdown(void) {
	long r = write_file("/proc/sys/kernel/modules_disabled", "1\n", 2, 0, 0);
	if (r == -ENOENT) return 0;
	if (r < 0) return die("lockdown", "cannot disable module loading");
	return 0;
}

/* ------------------------------------------------------------------ uvm-tools
 * NVRC uvm.rs setup_uvm_tools(): mknod /dev/nvidia-uvm-tools c <nvidia-uvm major> 1, mode 0666.
 * The major is dynamic (read from /proc/devices after the module loaded), so neither a
 * static device unit nor systemd-tmpfiles (fixed numbers only) can create the node. The node
 * is never replaced: an existing one is an error (NVRC: mknod EEXIST panics). */
static int cmd_uvm_tools(int argc, char **argv) {
	const char *drv = argc > 2 ? argv[2] : "nvidia-uvm"; /* optional name: used by the boot test only */
	static char dev[16384];
	if (read_file("/proc/devices", dev, sizeof(dev)) < 0) return die("uvm-tools", "cannot read /proc/devices");
	/* NVRC device::character_major: only the "Character devices:" section counts */
	const char *p = sfind(dev, "Character devices:");
	if (!p) return die("uvm-tools", "nvidia-uvm character device major missing or invalid");
	const char *blk = sfind(p, "Block devices:");
	long major = -1;
	p += slen("Character devices:");
	while (*p) {
		while (*p == '\n' || *p == ' ') p++;
		if (blk && p >= blk) break;
		u64 m = parse_u64_loose(p);
		const char *q = p;
		while (*q >= '0' && *q <= '9') q++;
		if (q == p) { while (*p && *p != '\n') p++; continue; }
		while (*q == ' ') q++;
		const char *e = q;
		while (*e && *e != '\n') e++;
		if ((usize)(e - q) == slen(drv) && !memcmp(q, drv, slen(drv))) { major = (long)m; break; }
		p = e;
	}
	if (major <= 0 || major > 4095) return die("uvm-tools", "nvidia-uvm character device major missing or invalid");
	u32 devno = (u32)((major << 8) | 1); /* minor 1 = UVM_TOOLS_MINOR; major < 4096 fits the legacy encoding */
	if (sys_mknod("/dev/nvidia-uvm-tools", 020000 | 0666, devno) < 0) return die("uvm-tools", "cannot create nvidia-uvm-tools");
	if (sys_chmod("/dev/nvidia-uvm-tools", 0666) < 0) return die("uvm-tools", "cannot chmod nvidia-uvm-tools");
	return 0;
}

int main_(int argc, char **argv) {
	if (argc < 2) return die("main", "usage: nvsd-helper <subcommand>");
	read_file("/proc/cmdline", g_cmdline, sizeof(g_cmdline));
	const char *sub = argv[1];
	if (seq(sub, "cmdline")) return cmd_cmdline();
	if (seq(sub, "hash")) return cmd_hash(argc, argv);
	if (seq(sub, "detect")) return cmd_detect();
	if (seq(sub, "lockdown")) return cmd_lockdown();
	if (seq(sub, "uvm-tools")) return cmd_uvm_tools(argc, argv);
	if (seq(sub, "syslog")) return cmd_syslog();
	if (seq(sub, "wait-marker")) return cmd_wait_marker(argc, argv);
	return die("main", "unknown subcommand");
}

void start_c(long *sp) {
	int argc = (int)*sp;
	char **argv = (char **)(sp + 1);
	sys_exit_(main_(argc, argv));
}
__asm__(".text\n.global _start\n_start:\n\txor %ebp,%ebp\n\tmov %rsp,%rdi\n\tand $-16,%rsp\n\tcall start_c\n");
