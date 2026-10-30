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
#define GPU_EXT_ROOT "/run/kata-extensions/gpu"

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
static int cmd_lockdown(void);
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
	if (seq(mode, "cpu")) { /* nothing will modprobe after this point: apply the lockdown now (see nvsd-lockdown.service) */
		long efd = sys_open(GPU_EXT_ROOT, O_RDONLY | O_DIRECTORY | O_CLOEXEC, 0);
		if (efd >= 0) { sys_close((int)efd); put("kata.env", "KATA_ATTESTER_VARIANT=nvidia\n", 0644); } /* NVRC sets it in every mode */
		if (cmd_lockdown() != 0) return 1;
		put("locked", "", 0644);
		return 0;
	}
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
	/* gpu_extension.rs: with the "gpu" extension mounted, NVIDIA modules load via modprobe --dirname <root>,
	 * nvidia-ctk gets --driver-root/--dev-root/--nvidia-cdi-hook-path, binaries and configs resolve under the
	 * root, and kata-agent gets KATA_ATTESTER_VARIANT=nvidia. Units read these env files. */
	int ext = 0;
	{
		long fd = sys_open(GPU_EXT_ROOT, O_RDONLY | O_DIRECTORY | O_CLOEXEC, 0);
		if (fd >= 0) { sys_close((int)fd); ext = 1; }
	}
	const char *dn = ext ? "NVSD_DIRNAME=--dirname " GPU_EXT_ROOT "\n" : "";
	const char *np = gpus == 1 ? "NVSD_PARAMS=NVreg_NvLinkDisable=1\n" : "";
	if (ext || gpus == 1) {
		char e[200];
		struct buf eb = BUF_INIT(e);
		b_str(&eb, dn); b_str(&eb, np); b_term(&eb);
		put("modprobe-nvidia.env", e, 0644);
	}
	if (ext) {
		put("modprobe-nvidia-uvm.env", dn, 0644);
		put("gpu-ext.env", "NVSD_GPU_ROOT=" GPU_EXT_ROOT "\nNVSD_CTK_ARGS=--driver-root=" GPU_EXT_ROOT " --dev-root=/ --nvidia-cdi-hook-path=" GPU_EXT_ROOT "/bin/nvidia-cdi-hook\n", 0644);
		put("kata.env", "KATA_ATTESTER_VARIANT=nvidia\n", 0644);
	}
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

/* ------------------------------------------------------------------ fm-config
 * NVRC daemon.rs nv_fabricmanager() + config.rs update_config_file(): copy the stock
 * fabricmanager.cfg to the writable /run/fabricmanager.cfg, set FABRIC_MODE and
 * PARTITION_RAIL_POLICY (existing "KEY=" lines are replaced in place, missing keys appended),
 * then chmod 0400. systemd cannot edit a KEY=VALUE file.
 *   fm-config SRC FABRIC_MODE RAIL_POLICY
 * Same line semantics as NVRC: a line is the key's if it starts with "KEY=" after trimming. */
static int cmd_fm_config(int argc, char **argv) {
	if (argc != 5) return die("fm-config", "usage: fm-config SRC FABRIC_MODE RAIL_POLICY");
	static char in[65536], out[65536 + 256];
	long n = read_file(argv[2], in, sizeof(in));
	if (n < 0) return die("fm-config", "cannot read the stock fabricmanager.cfg");
	const char *keys[2] = { "FABRIC_MODE", "PARTITION_RAIL_POLICY" };
	const char *vals[2] = { argv[3], argv[4] };
	int found[2] = { 0, 0 };
	struct buf o = { out, 0, sizeof(out) };
	const char *p = in;
	while (*p) {
		const char *e = p;
		while (*e && *e != '\n') e++;
		const char *le = e;
		if (le > p && le[-1] == '\r') le--; /* Rust str::lines() */
		const char *t = p;
		while (t < le && (*t == ' ' || *t == '\t')) t++;
		int hit = -1;
		for (int k = 0; k < 2; k++) {
			usize kl = slen(keys[k]);
			if ((usize)(le - t) > kl && !memcmp(t, keys[k], kl) && t[kl] == '=') { hit = k; break; }
		}
		if (hit >= 0) {
			found[hit] = 1;
			b_str(&o, keys[hit]); b_char(&o, '='); b_str(&o, vals[hit]);
		} else {
			b_mem(&o, p, (usize)(le - p));
		}
		b_char(&o, '\n');
		p = *e ? e + 1 : e;
	}
	for (int k = 0; k < 2; k++)
		if (!found[k]) { b_str(&o, keys[k]); b_char(&o, '='); b_str(&o, vals[k]); b_char(&o, '\n'); }
	if (o.len + 1 >= o.cap) return die("fm-config", "config too large");
	sys_unlink("/run/fabricmanager.cfg");
	if (write_file("/run/fabricmanager.cfg", out, o.len, O_CREAT | O_TRUNC, 0600) < 0) return die("fm-config", "cannot write /run/fabricmanager.cfg");
	if (sys_chmod("/run/fabricmanager.cfg", 0400) < 0) return die("fm-config", "cannot chmod /run/fabricmanager.cfg");
	return 0;
}

/* ------------------------------------------------------------------ ib-guid
 * NVRC infiniband.rs detect_port_guid(): after mlx5_ib is loaded, pick the first (by name) device in
 * /sys/class/infiniband whose port 1 has the subnet manager enabled (cap_mask bit 10 clear) and
 * whose gids/0 holds eight groups; the port GUID is the last four groups, "0x"-prefixed. No GUID,
 * a missing or an empty class directory are fatal (NVRC panics). The result goes to
 * /run/nvsd/port-guid.env for nvlsm and nv-fabricmanager:
 *   NVSD_GUID=0x0002c90300297de1       NVSD_FM_GUID_ARGS=-g 0x0002c90300297de1
 * Reading and sorting sysfs text is something no systemd directive does.
 *   ib-guid [CLASS_DIR]     (the optional directory exists for the boot test) */
static int cmd_ib_guid(int argc, char **argv) {
	const char *root = argc > 2 ? argv[2] : "/sys/class/infiniband";
	struct dirit d;
	if (dir_open(&d, root) < 0) return die("ib-guid", "/sys/class/infiniband not found - mlx5_ib module not loaded");
	static char names[32][64];
	int n = 0;
	const char *e;
	while ((e = dir_next(&d)) && n < 32) {
		usize l = slen(e);
		if (l >= 64) continue;
		memcpy(names[n++], e, l + 1);
	}
	dir_close(&d);
	if (n == 0) return die("ib-guid", "/sys/class/infiniband is empty - mlx5_ib loaded but no IB devices registered");
	for (int i = 1; i < n; i++) { /* sort by name: mlx5_0 before mlx5_1 */
		char t[64];
		int j = i;
		memcpy(t, names[i], 64);
		while (j > 0 && memcmp(names[j - 1], t, 64) > 0) { memcpy(names[j], names[j - 1], 64); j--; }
		memcpy(names[j], t, 64);
	}
	for (int i = 0; i < n; i++) {
		char p[400], v[128];
		char sub[100];
		struct buf sb = BUF_INIT(sub);
		b_str(&sb, names[i]); b_str(&sb, "/ports/1/cap_mask"); b_term(&sb);
		path_join3(p, sizeof(p), root, sub, NULL);
		if (read_file(p, v, sizeof(v)) < 0) continue;
		rtrim(v);
		const char *h = v;
		if (h[0] == '0' && (h[1] == 'x' || h[1] == 'X')) h += 2;
		u32 mask = 0;
		for (; *h; h++) {
			int dgt = (*h >= '0' && *h <= '9') ? *h - '0' : (*h >= 'a' && *h <= 'f') ? *h - 'a' + 10 : (*h >= 'A' && *h <= 'F') ? *h - 'A' + 10 : -1;
			if (dgt < 0) { mask = 0; break; }
			mask = mask * 16 + (u32)dgt;
		}
		if (mask & (1u << 10)) continue; /* SM disabled */
		sb.len = 0;
		b_str(&sb, names[i]); b_str(&sb, "/ports/1/gids/0"); b_term(&sb);
		path_join3(p, sizeof(p), root, sub, NULL);
		if (read_file(p, v, sizeof(v)) < 0) continue;
		rtrim(v);
		/* exactly eight ':'-separated groups */
		int groups = 1;
		const char *g[8];
		g[0] = v;
		for (char *q = v; *q; q++)
			if (*q == ':') { *q = 0; if (groups < 8) g[groups] = q + 1; groups++; }
		if (groups != 8) continue;
		char guid[80];
		struct buf gb = BUF_INIT(guid);
		b_str(&gb, "0x"); b_str(&gb, g[4]); b_str(&gb, g[5]); b_str(&gb, g[6]); b_str(&gb, g[7]); b_term(&gb);
		char env[300];
		struct buf eb = BUF_INIT(env);
		b_str(&eb, "NVSD_GUID="); b_str(&eb, guid); b_str(&eb, "\nNVSD_FM_GUID_ARGS=-g "); b_str(&eb, guid); b_char(&eb, '\n'); b_term(&eb);
		ensure_run_dir();
		if (put("port-guid.env", env, 0644) < 0) return die("ib-guid", "cannot write port-guid.env");
		return 0;
	}
	return die("ib-guid", "nvl5 requires SW_MNG IB device with valid port GUID");
}

/* ------------------------------------------------------------------ ext-plan
 * NVRC guest_extension_image.rs mount_all(), the planning half: cold-plugged composable image
 * extensions are virtio-blk devices whose serial is "extension-<name>"; their dm-verity
 * parameters arrive on the measured kernel command line as
 *   kata.extension.<name>.verity_params=root_hash=..,salt=..,data_blocks=..,data_block_size=..,hash_block_size=..
 * NVRC fails closed: a device without parameters, parameters without a device, a bad name, a
 * missing or malformed field, or a missing data/hash partition is fatal. This subcommand does
 * all of that checking and writes /run/nvsd/ext-<name>.env with the veritysetup argument list
 * (NVSD_VERITY_ARGS) for nvsd-extension@<name>.service; the mount itself is a mount unit.
 * systemd can neither enumerate devices by serial nor split a key=a,b=c value.
 *
 * Difference from NVRC: systemd cannot instantiate units dynamically, so only extension names
 * that have a mount unit in the image (shipped: gpu) are accepted; any other name is fatal. */
static int ext_name_ok(const char *n) {
	if (!*n) return 0;
	int alldots = 1;
	for (const char *c = n; *c; c++) {
		if (*c != '.') alldots = 0;
		if (!((*c >= '0' && *c <= '9') || (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || *c == '_' || *c == '-' || *c == '.')) return 0;
	}
	return !alldots;
}

static int ext_field(const char *list, const char *key, char *out, usize cap) {
	usize kl = slen(key);
	const char *p = list;
	int found = 0;
	while (*p) {
		const char *e = p;
		while (*e && *e != ',') e++;
		if ((usize)(e - p) > kl && !memcmp(p, key, kl) && p[kl] == '=') {
			usize vl = (usize)(e - p) - kl - 1;
			if (vl >= cap) vl = cap - 1;
			memcpy(out, p + kl + 1, vl);
			out[vl] = 0;
			found = 1;
		}
		p = *e ? e + 1 : e;
	}
	return found;
}

static int cmd_ext_plan(void) {
	enum { MAXE = 8 };
	static char pn[MAXE][64], pv[MAXE][512], dn[MAXE][64], dd[MAXE][32];
	int np = 0, nd = 0;
	/* parameters, in command-line order */
	const char *p = g_cmdline;
	while (*p) {
		while (*p == ' ' || *p == '\n' || *p == '\t') p++;
		const char *w = p;
		while (*p && *p != ' ' && *p != '\n' && *p != '\t') p++;
		usize wl = (usize)(p - w);
		const char *eq = w;
		while (eq < w + wl && *eq != '=') eq++;
		if (eq == w + wl) continue;
		usize kl = (usize)(eq - w);
		if (kl > 15 + 14 && !memcmp(w, "kata.extension.", 15) && !memcmp(w + kl - 14, ".verity_params", 14)) {
			usize nl = kl - 15 - 14;
			if (np >= MAXE || nl >= 64) return die("ext-plan", "too many or too long extension parameters");
			memcpy(pn[np], w + 15, nl); pn[np][nl] = 0;
			usize vl = wl - kl - 1;
			if (vl >= 512) return die("ext-plan", "verity_params too long");
			memcpy(pv[np], eq + 1, vl); pv[np][vl] = 0;
			np++;
		}
	}
	/* devices */
	struct dirit d;
	if (dir_open(&d, "/sys/block") == 0) {
		const char *e;
		while ((e = dir_next(&d))) {
			char sp[300], ser[96];
			path_join3(sp, sizeof(sp), "/sys/block", e, "serial");
			if (read_file(sp, ser, sizeof(ser)) < 0) continue;
			rtrim(ser);
			if (!sstarts(ser, "extension-")) continue;
			const char *name = ser + 10;
			if (!ext_name_ok(name)) return die("ext-plan", "invalid extension name");
			if (nd >= MAXE || slen(e) >= 32) return die("ext-plan", "too many extension devices");
			memcpy(dn[nd], name, slen(name) + 1);
			memcpy(dd[nd], e, slen(e) + 1);
			nd++;
		}
		dir_close(&d);
	}
	for (int i = 0; i < np; i++) {
		int ok = 0;
		for (int j = 0; j < nd; j++) if (seq(pn[i], dn[j])) ok = 1;
		if (!ok) return die("ext-plan", "verity params on cmdline but no extension device");
	}
	ensure_run_dir();
	for (int j = 0; j < nd; j++) {
		int k = -1;
		for (int i = 0; i < np; i++) if (seq(pn[i], dn[j])) k = i; /* NVRC uses the first match */
		for (int i = np - 1; i >= 0; i--) if (seq(pn[i], dn[j])) k = i;
		if (k < 0) return die("ext-plan", "extension device present but no verity params; refusing unverified mount");
		char root_hash[256], salt[256], db[24], dbs[24], hbs[24];
		if (!ext_field(pv[k], "root_hash", root_hash, sizeof(root_hash))) return die("ext-plan", "verity_params missing root_hash");
		if (!ext_field(pv[k], "salt", salt, sizeof(salt))) return die("ext-plan", "verity_params missing salt");
		u32 n1, n2, n3;
		if (!ext_field(pv[k], "data_blocks", db, sizeof(db)) || !parse_u32(db, &n1) || !n1) return die("ext-plan", "verity_params missing or invalid data_blocks");
		if (!ext_field(pv[k], "data_block_size", dbs, sizeof(dbs)) || !parse_u32(dbs, &n2) || !n2) return die("ext-plan", "verity_params missing or invalid data_block_size");
		if (!ext_field(pv[k], "hash_block_size", hbs, sizeof(hbs)) || !parse_u32(hbs, &n3) || !n3) return die("ext-plan", "verity_params missing or invalid hash_block_size");
		/* only characters that survive systemd's EnvironmentFile + word splitting */
		for (const char *c = root_hash; *c; c++) if (!((*c >= '0' && *c <= '9') || (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z'))) return die("ext-plan", "invalid root_hash");
		for (const char *c = salt; *c; c++) if (!((*c >= '0' && *c <= '9') || (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z'))) return die("ext-plan", "invalid salt");
		/* mount unit must exist for this name */
		char mu[200];
		struct buf mb = BUF_INIT(mu);
		b_str(&mb, "/etc/systemd/system/run-kata\\x2dextensions-"); b_str(&mb, dn[j]); b_str(&mb, ".mount"); b_term(&mb);
		char probe[8];
		if (read_file(mu, probe, sizeof(probe)) < 0) return die("ext-plan", "no mount unit for this extension name in the image");
		/* partitions: data = partition 1, hash = partition 2, from sysfs */
		char dir[200], data[64] = "", hash[64] = "";
		struct buf db2 = BUF_INIT(dir);
		b_str(&db2, "/sys/block/"); b_str(&db2, dd[j]); b_term(&db2);
		struct dirit pd;
		if (dir_open(&pd, dir) < 0) return die("ext-plan", "cannot read extension device");
		const char *pe;
		while ((pe = dir_next(&pd))) {
			char pp[400], num[16];
			path_join3(pp, sizeof(pp), dir, pe, "partition");
			if (read_file(pp, num, sizeof(num)) < 0) continue;
			rtrim(num);
			struct buf nb = { seq(num, "1") ? data : hash, 0, 64 };
			if (!seq(num, "1") && !seq(num, "2")) continue;
			b_str(&nb, "/dev/"); b_str(&nb, pe); b_term(&nb);
		}
		dir_close(&pd);
		if (!data[0]) return die("ext-plan", "extension device: missing data partition (1)");
		if (!hash[0]) return die("ext-plan", "extension device: missing hash partition (2)");
		char env[1400], fn[96];
		struct buf eb = BUF_INIT(env);
		b_str(&eb, "NVSD_VERITY_ARGS=open --no-superblock --hash sha256 --data-block-size "); b_u64(&eb, n2);
		b_str(&eb, " --hash-block-size "); b_u64(&eb, n3); b_str(&eb, " --data-blocks "); b_u64(&eb, n1);
		b_str(&eb, " --salt "); b_str(&eb, salt); b_char(&eb, ' '); b_str(&eb, data);
		b_str(&eb, " extension-"); b_str(&eb, dn[j]); b_char(&eb, ' '); b_str(&eb, hash); b_char(&eb, ' '); b_str(&eb, root_hash); b_char(&eb, '\n'); b_term(&eb);
		struct buf fb = BUF_INIT(fn);
		b_str(&fb, "ext-"); b_str(&fb, dn[j]); b_str(&fb, ".env"); b_term(&fb);
		if (put(fn, env, 0644) < 0) return die("ext-plan", "cannot write extension plan");
		/* the mount unit uses this symlink as What=: systemd adds an implicit dependency on a .device unit for
		 * What=/dev/..., which only udev (absent here) ever activates, so the unit would wait for its timeout */
		char lnk[96], tgt[96];
		struct buf lb = BUF_INIT(lnk), tb = BUF_INIT(tgt);
		b_str(&lb, RUN "/ext-"); b_str(&lb, dn[j]); b_str(&lb, ".dev"); b_term(&lb);
		b_str(&tb, "/dev/mapper/extension-"); b_str(&tb, dn[j]); b_term(&tb);
		sys_unlink(lnk);
		if (sys_symlink(tgt, lnk) < 0) return die("ext-plan", "cannot create the device symlink");
	}
	return 0;
}

/* ------------------------------------------------------------------ poweroff
 * NVRC lockdown.rs panic hook: message to /dev/kmsg, sync(), reboot(RB_POWER_OFF). Started by
 * OnFailure=/OnSuccess= of every unit. (FailureAction=poweroff-immediate does the same but sleeps
 * 5 s first, see overlay/etc/systemd/system/nvsd-poweroff.service. The MONITOR_UNIT variables that
 * would name the failed unit are not set when several units trigger it at once, as happens when a
 * failure cascades through the dependency chain; systemd's own log line names the unit.) */
static int cmd_poweroff(void) {
	static const char m[] = "<2>nvsystemd: boot unit failed or kata-agent exited, powering off\n";
	write_file("/dev/kmsg", m, sizeof(m) - 1, 0, 0);
	sc0(SYS_sync);
	sc4(SYS_reboot, 0xfee1dead, 672274793, 0x4321FEDC, 0); /* LINUX_REBOOT_CMD_POWER_OFF */
	return die("poweroff", "reboot(POWER_OFF) returned");
}

/* ------------------------------------------------------------------ init
 * The NVRC prologue in one process: mount::setup() extras, kmsg/kernel parameters, mode detection.
 * (nvsd-init.service; see its comment for why this is one unit.) */
#define MS_NOSUID_ 2
#define MS_NODEV_ 4
#define MS_NOEXEC_ 8
#define MS_RELATIME_ (1ul << 21)
static int cmd_init(void) {
	/* mount.rs setup(): /tmp tmpfs MS_NOSUID|MS_NODEV|MS_RELATIME */
	if (sys_mount("tmpfs", "/tmp", "tmpfs", MS_NOSUID_ | MS_NODEV_ | MS_RELATIME_, NULL) < 0) return die("init", "mount tmpfs on /tmp failed");
	/* mount_optional(configfs): only if the kernel has it and the target exists */
	static char fs[4096];
	read_file("/proc/filesystems", fs, sizeof(fs));
	long cfd = sys_open("/sys/kernel/config", O_RDONLY | O_DIRECTORY | O_CLOEXEC, 0);
	if (cfd >= 0) {
		sys_close((int)cfd);
		const char *p = fs;
		int have = 0;
		while (*p) {
			const char *e = p;
			while (*e && *e != '\n') e++;
			const char *w = e;
			while (w > p && w[-1] != ' ' && w[-1] != '\t') w--;
			if ((usize)(e - w) == 8 && !memcmp(w, "configfs", 8)) have = 1;
			p = *e ? e + 1 : e;
		}
		if (have && sys_mount("configfs", "/sys/kernel/config", "configfs", MS_NOSUID_ | MS_NODEV_ | MS_NOEXEC_ | MS_RELATIME_, NULL) < 0)
			return die("init", "mount configfs failed");
	}
	int r = cmd_cmdline();
	if (r) return r;
	return cmd_detect();
}

int main_(int argc, char **argv) {
	if (argc < 2) return die("main", "usage: nvsd-helper <subcommand>");
	read_file("/proc/cmdline", g_cmdline, sizeof(g_cmdline));
	const char *sub = argv[1];
	if (seq(sub, "cmdline")) return cmd_cmdline();
	if (seq(sub, "hash")) return cmd_hash(argc, argv);
	if (seq(sub, "init")) return cmd_init();
	if (seq(sub, "detect")) return cmd_detect();
	if (seq(sub, "lockdown")) return cmd_lockdown();
	if (seq(sub, "poweroff")) return cmd_poweroff();
	if (seq(sub, "ext-plan")) return cmd_ext_plan();
	if (seq(sub, "ib-guid")) return cmd_ib_guid(argc, argv);
	if (seq(sub, "fm-config")) return cmd_fm_config(argc, argv);
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
