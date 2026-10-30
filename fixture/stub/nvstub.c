/* SPDX-License-Identifier: Apache-2.0
 *
 * nvstub: SIMULATED NVIDIA userspace for GPU-less testing and benchmarking.
 *
 * One static, libc-free multi-call binary. It is installed (hard-linked) under
 * the names/paths NVRC execs:
 *
 *   /sbin/modprobe  /bin/nvidia-smi  /bin/nvidia-persistenced  /bin/nv-hostengine
 *   /bin/dcgm-exporter  /bin/nvidia-ctk  /bin/nv-fabricmanager  /sbin/nvlsm
 *   /usr/bin/kata-agent
 *
 * Nothing here talks to a GPU. Every stub:
 *   - validates its argv the way the real tool would be invoked by NVRC
 *     (a wrong argv exits 64 and logs BADARGV),
 *   - appends a timestamped (CLOCK_BOOTTIME, microseconds) line to the log
 *     (default /run/nvstub.log),
 *   - sleeps a configurable simulated latency, default 0,
 *   - daemonizes / stays alive where the real tool does,
 *   - can be told to fail.
 *
 * Configuration: /etc/nvstub.conf (whitespace separated key=value, "fixture
 * file") overridden by kernel command line items prefixed "nvstub.".
 *
 *   latency.<tool>[.<sub>]=<ms>     simulated latency (modprobe sub = module,
 *                                   nvidia-smi sub = lmc|lgc|pl|srs)
 *   fail.<tool>[.<sub>]=<exitcode>  fail instead of succeeding
 *   grace_ms=<ms>                   kata-agent stub: wait before reporting
 *   log=<path>                      log file (default /run/nvstub.log)
 *   kmsg_grep=<text>                kata-agent stub: print kernel log records containing <text> as
 *                                   "NVSD-KMSG <message>" (tests)
 *   echo=1                          also print every log line to the console as
 *                                   "NVSTUB <line>" (tests only: costs console time)
 *
 * The kata-agent stub is the end marker: it records its start time, waits the
 * grace period, prints a report (timing, memory, process census, the stub log)
 * to /dev/console and powers the VM off.
 *
 * Daemon model (SIMULATED, chosen to match how NVRC consumes the real tools):
 *   nvidia-persistenced, nv-hostengine, nv-fabricmanager: fork; the parent
 *     exits 0 immediately (the daemon() style); the child "initialises" for the
 *     configured latency, logs "ready" and, for persistenced and the fabric
 *     manager, sends the syslog readiness marker NVRC waits for
 *     ("Local RPC services initialized" / "FM starting NvLink Inband") to
 *     /dev/log, then lives until killed.
 *   dcgm-exporter, nvlsm: stay in the foreground for the whole VM lifetime.
 */
#define MINI_IMPL
#include "../../common/mini.h"

static const char *g_tool;
static char g_conf[4096];
static char g_cmdline[4096];
static char g_log[128] = "/run/nvstub.log";
static char g_joined[1024];
static char **g_envp;
static int g_echo;
static int g_argc;
static char **g_argv;

/* ---- config: cmdline "nvstub.<key>" first, then /etc/nvstub.conf ---- */
static int conf_get(const char *key, char *out, usize cap) {
	char k[96];
	struct buf b = BUF_INIT(k);
	b_str(&b, "nvstub."); b_str(&b, key); b_term(&b);
	if (cmdline_get(g_cmdline, k, out, cap)) return 1;
	return cmdline_get(g_conf, key, out, cap);
}
static i64 conf_num(const char *tool, const char *sub, const char *prefix, i64 def) {
	char key[96], v[32];
	struct buf b = BUF_INIT(key);
	b_str(&b, prefix); b_char(&b, '.'); b_str(&b, tool);
	if (sub) { b_char(&b, '.'); b_str(&b, sub); }
	b_term(&b);
	if (conf_get(key, v, sizeof(v))) return (i64)parse_u64_loose(v);
	if (sub) return conf_num(tool, NULL, prefix, def);
	return def;
}

static void log_event(const char *name, const char *event, const char *detail) {
	char line[1400];
	struct buf b = BUF_INIT(line);
	b_u64(&b, boottime_us()); b_char(&b, ' ');
	b_u64(&b, (u64)sys_getpid()); b_char(&b, ' ');
	b_str(&b, name); b_char(&b, ' ');
	b_str(&b, event);
	if (detail && *detail) { b_char(&b, ' '); b_str(&b, detail); }
	b_char(&b, '\n');
	append_file(g_log, line, b.len, 0644);
	if (g_echo) { /* test aid: mirror the line to the console as "NVSTUB <line>" so failures stay visible */
		char l2[1500];
		struct buf e = BUF_INIT(l2);
		b_str(&e, "NVSTUB "); b_mem(&e, line, b.len); b_term(&e);
		long fd = sys_open("/dev/console", O_WRONLY | 0400, 0);
		if (fd >= 0) { sys_write((int)fd, l2, e.len); sys_close((int)fd); }
	}
}

static void join_argv(int from) {
	struct buf b = BUF_INIT(g_joined);
	b_str(&b, "argv=");
	for (int i = from; i < g_argc; i++) {
		if (i > from) b_char(&b, ' ');
		b_str(&b, g_argv[i]);
	}
	b_term(&b);
}

static void badargv(const char *name, const char *why) {
	char d[1400];
	struct buf b = BUF_INIT(d);
	b_str(&b, why); b_str(&b, " "); b_str(&b, g_joined); b_term(&b);
	log_event(name, "BADARGV", d);
	char m[1400];
	struct buf e = BUF_INIT(m);
	b_str(&e, "nvstub: "); b_str(&e, name); b_str(&e, ": unexpected argv: "); b_str(&e, why);
	b_str(&e, "\n"); b_term(&e);
	sys_write(2, m, e.len);
	sys_exit_(64);
}

static void syslog_send(const char *msg) {
	long s = sc3(SYS_socket, AF_UNIX, SOCK_DGRAM | O_CLOEXEC, 0);
	if (s < 0) return;
	struct { u16 fam; char path[108]; } sa;
	memset(&sa, 0, sizeof(sa));
	sa.fam = AF_UNIX;
	memcpy(sa.path, "/dev/log", 9);
	char m[256];
	struct buf b = BUF_INIT(m);
	b_str(&b, "<14>"); b_str(&b, g_tool); b_str(&b, "[");
	b_u64(&b, (u64)sys_getpid()); b_str(&b, "]: "); b_str(&b, msg); b_term(&b);
	sc6(SYS_sendto, s, (long)m, (long)b.len, 0, (long)&sa, 2 + 9);
	sys_close((int)s);
}

static void wait_forever(void) { for (;;) sc0(SYS_pause); }

static void devnull_stdio(void) {
	long fd = sys_open("/dev/null", O_RDWR, 0);
	if (fd >= 0) {
		sc2(33 /* dup2 */, fd, 0);
		sc2(33, fd, 1);
		sc2(33, fd, 2);
		if (fd > 2) sys_close((int)fd);
	}
}

/* Fork; parent exits (0, or injected failure) immediately; child detaches. */
static void daemonize(const char *name, const char *syslog_marker, i64 latency_ms) {
	i64 f = conf_num(g_tool, NULL, "fail", -1);
	if (f >= 0) {
		log_event(name, "FAIL", "parent-exit-nonzero");
		sys_exit_((int)f);
	}
	long pid = sys_fork();
	if (pid < 0) sys_exit_(70);
	if (pid > 0) {
		sys_exit_(0);
	}
	sys_setsid();
	devnull_stdio();
	log_event(name, "daemon-start", "");
	if (latency_ms > 0) sleep_ms((u64)latency_ms);
	log_event(name, "ready", "");
	if (syslog_marker) syslog_send(syslog_marker);
	wait_forever();
}

static int is_dir(const char *p) {
	long fd = sys_open(p, O_RDONLY | O_DIRECTORY, 0);
	if (fd < 0) return 0;
	sys_close((int)fd);
	return 1;
}
static int exists(const char *p) {
	char st[256];
	return sc4(SYS_newfstatat, AT_FDCWD, (long)p, (long)st, 0) == 0;
}
static void mkdir_p(const char *path) {
	char t[256];
	usize n = slen(path);
	if (n >= sizeof(t)) return;
	memcpy(t, path, n + 1);
	for (usize i = 1; i < n; i++) {
		if (t[i] == '/') { t[i] = 0; sys_mkdir(t, 0755); t[i] = '/'; }
	}
	sys_mkdir(t, 0755);
}

/* ------------------------------------------------------------------ */
static int do_modprobe(void) {
	/* /sbin/modprobe [--dirname DIR] <module> [NVreg_NvLinkDisable=1] */
	int i = 1;
	const char *dirname = NULL;
	if (i < g_argc && seq(g_argv[i], "--dirname")) {
		if (i + 1 >= g_argc) badargv("modprobe", "--dirname needs a value");
		dirname = g_argv[i + 1];
		i += 2;
	}
	if (i >= g_argc) badargv("modprobe", "no module");
	const char *mod = g_argv[i++];
	if (!(seq(mod, "nvidia") || seq(mod, "nvidia-uvm") || seq(mod, "ib_umad") || seq(mod, "mlx5_ib")))
		badargv("modprobe", "unknown module");
	int nvlink_off = 0;
	if (i < g_argc) {
		if (!(seq(mod, "nvidia") && seq(g_argv[i], "NVreg_NvLinkDisable=1") && i + 1 == g_argc))
			badargv("modprobe", "unexpected module parameter");
		nvlink_off = 1;
	}
	if (dirname && !sstarts(mod, "nvidia")) badargv("modprobe", "--dirname only for nvidia modules");
	if (seq(mod, "nvidia")) {
		/* NVRC passes NVreg_NvLinkDisable=1 iff exactly one NVIDIA GPU is present */
		int gpus = pci_count("/sys/bus/pci/devices", "0x10de", "0x03");
		if ((gpus == 1) != nvlink_off) badargv("modprobe", "NvLinkDisable does not match GPU count");
	}
	char d[400];
	struct buf b = BUF_INIT(d);
	b_str(&b, "module="); b_str(&b, mod); b_char(&b, ' '); b_str(&b, g_joined); b_term(&b);
	log_event("modprobe", "start", d);
	i64 lat = conf_num("modprobe", mod, "latency", 0);
	if (lat > 0) sleep_ms((u64)lat);
	i64 f = conf_num("modprobe", mod, "fail", -1);
	if (f >= 0) { log_event("modprobe", "FAIL", mod); return (int)f; }
	log_event("modprobe", "done", mod);
	return 0;
}

static int do_smi(void) {
	/* -lmc N | -lgc N | -pl N | conf-compute -srs 0|1 */
	const char *sub = NULL;
	u32 n;
	if (g_argc == 3 && seq(g_argv[1], "-lmc") && parse_u32(g_argv[2], &n)) sub = "lmc";
	else if (g_argc == 3 && seq(g_argv[1], "-lgc") && parse_u32(g_argv[2], &n)) sub = "lgc";
	else if (g_argc == 3 && seq(g_argv[1], "-pl") && parse_u32(g_argv[2], &n)) sub = "pl";
	else if (g_argc == 4 && seq(g_argv[1], "conf-compute") && seq(g_argv[2], "-srs") &&
		 (seq(g_argv[3], "0") || seq(g_argv[3], "1"))) sub = "srs";
	else badargv("nvidia-smi", "unsupported invocation");
	char nm[40];
	struct buf b = BUF_INIT(nm);
	b_str(&b, "nvidia-smi:"); b_str(&b, sub); b_term(&b);
	log_event(nm, "start", g_joined);
	i64 lat = conf_num("nvidia-smi", sub, "latency", 0);
	if (lat > 0) sleep_ms((u64)lat);
	i64 f = conf_num("nvidia-smi", sub, "fail", -1);
	if (f >= 0) { log_event(nm, "FAIL", ""); return (int)f; }
	log_event(nm, "done", "");
	return 0;
}

static int do_persistenced(void) {
	/* --verbose [--uvm-persistence-mode] ; needs /var/run/nvidia-persistenced */
	if (!(g_argc == 2 || g_argc == 3) || !seq(g_argv[1], "--verbose") ||
	    (g_argc == 3 && !seq(g_argv[2], "--uvm-persistence-mode")))
		badargv("nvidia-persistenced", "unsupported invocation");
	if (!is_dir("/var/run/nvidia-persistenced")) {
		log_event("nvidia-persistenced", "ERROR", "/var/run/nvidia-persistenced missing");
		say(2, "nvidia-persistenced: /var/run/nvidia-persistenced does not exist\n");
		return 1;
	}
	log_event("nvidia-persistenced", "start", g_joined);
	daemonize("nvidia-persistenced", "Local RPC services initialized",
		  conf_num("nvidia-persistenced", NULL, "latency", 0));
	return 0;
}

static int do_hostengine(void) {
	if (!(g_argc == 5 && seq(g_argv[1], "--service-account") && seq(g_argv[2], "nvidia-dcgm") &&
	      seq(g_argv[3], "--home-dir") && seq(g_argv[4], "/tmp")))
		badargv("nv-hostengine", "unsupported invocation");
	log_event("nv-hostengine", "start", g_joined);
	daemonize("nv-hostengine", NULL, conf_num("nv-hostengine", NULL, "latency", 0));
	return 0;
}

static int do_exporter(void) {
	if (!(g_argc == 4 && seq(g_argv[1], "-k") && seq(g_argv[2], "-f")))
		badargv("dcgm-exporter", "unsupported invocation");
	if (!exists(g_argv[3])) {
		log_event("dcgm-exporter", "ERROR", "collectors file missing");
		say(2, "dcgm-exporter: collectors file not found\n");
		return 1;
	}
	log_event("dcgm-exporter", "start", g_joined);
	i64 f = conf_num("dcgm-exporter", NULL, "fail", -1);
	i64 lat = conf_num("dcgm-exporter", NULL, "latency", 0);
	if (lat > 0) sleep_ms((u64)lat);
	if (f >= 0) { log_event("dcgm-exporter", "FAIL", ""); return (int)f; }
	log_event("dcgm-exporter", "ready", "");
	wait_forever();
	return 0;
}

static int do_ctk(void) {
	/* -d cdi generate --output=/var/run/cdi/nvidia.yaml [--driver-root=R --dev-root=/] [--nvidia-cdi-hook-path=P] */
	if (g_argc < 5 || !seq(g_argv[1], "-d") || !seq(g_argv[2], "cdi") || !seq(g_argv[3], "generate") ||
	    !seq(g_argv[4], "--output=/var/run/cdi/nvidia.yaml"))
		badargv("nvidia-ctk", "unsupported invocation");
	int roots = 0;
	for (int i = 5; i < g_argc; i++) {
		if (sstarts(g_argv[i], "--driver-root=")) roots |= 1;
		else if (seq(g_argv[i], "--dev-root=/")) roots |= 2;
		else if (sstarts(g_argv[i], "--nvidia-cdi-hook-path=")) {}
		else badargv("nvidia-ctk", "unknown option");
	}
	if (roots == 1 || roots == 2) badargv("nvidia-ctk", "--driver-root and --dev-root must come together");
	log_event("nvidia-ctk", "start", g_joined);
	i64 lat = conf_num("nvidia-ctk", NULL, "latency", 0);
	if (lat > 0) sleep_ms((u64)lat);
	i64 f = conf_num("nvidia-ctk", NULL, "fail", -1);
	if (f >= 0) { log_event("nvidia-ctk", "FAIL", ""); return (int)f; }
	mkdir_p("/var/run/cdi");
	write_file("/var/run/cdi/nvidia.yaml", "# simulated CDI spec (nvstub)\n", 30, O_CREAT | O_TRUNC, 0644);
	log_event("nvidia-ctk", "done", "");
	return 0;
}

static int cfg_value(const char *cfg, const char *key, char *out, usize cap) {
	usize kl = slen(key);
	const char *p = cfg;
	while (*p) {
		const char *e = p;
		while (*e && *e != '\n') e++;
		if ((usize)(e - p) > kl && sstarts(p, key) && p[kl] == '=') {
			usize vl = (usize)(e - p) - kl - 1;
			if (vl >= cap) vl = cap - 1;
			memcpy(out, p + kl + 1, vl);
			out[vl] = 0;
			return 1;
		}
		p = *e ? e + 1 : e;
	}
	return 0;
}

static int valid_guid(const char *g) {
	if (!(g[0] == '0' && g[1] == 'x') || slen(g) != 18) return 0;
	for (int i = 2; i < 18; i++) {
		char c = g[i];
		if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return 0;
	}
	return 1;
}

static int do_fm(void) {
	/* -c /run/fabricmanager.cfg [-g 0xGUID] */
	if (!(g_argc == 3 || g_argc == 5) || !seq(g_argv[1], "-c") || !seq(g_argv[2], "/run/fabricmanager.cfg") ||
	    (g_argc == 5 && !(seq(g_argv[3], "-g") && valid_guid(g_argv[4]))))
		badargv("nv-fabricmanager", "unsupported invocation");
	static char cfg[8192];
	if (read_file("/run/fabricmanager.cfg", cfg, sizeof(cfg)) < 0) {
		log_event("nv-fabricmanager", "ERROR", "cfg missing");
		return 1;
	}
	char mode[16], rail[32];
	if (!cfg_value(cfg, "FABRIC_MODE", mode, sizeof(mode)) || !cfg_value(cfg, "PARTITION_RAIL_POLICY", rail, sizeof(rail)))
		badargv("nv-fabricmanager", "cfg lacks FABRIC_MODE/PARTITION_RAIL_POLICY");
	char d[500];
	struct buf b = BUF_INIT(d);
	b_str(&b, g_joined); b_str(&b, " FABRIC_MODE="); b_str(&b, mode);
	b_str(&b, " PARTITION_RAIL_POLICY="); b_str(&b, rail); b_term(&b);
	log_event("nv-fabricmanager", "start", d);
	daemonize("nv-fabricmanager", "FM starting NvLink Inband", conf_num("nv-fabricmanager", NULL, "latency", 0));
	return 0;
}

static int do_nvlsm(void) {
	/* -F <cfg> -g 0xGUID -f stdout */
	if (!(g_argc == 7 && seq(g_argv[1], "-F") && seq(g_argv[3], "-g") && valid_guid(g_argv[4]) &&
	      seq(g_argv[5], "-f") && seq(g_argv[6], "stdout")))
		badargv("nvlsm", "unsupported invocation");
	log_event("nvlsm", "start", g_joined);
	i64 lat = conf_num("nvlsm", NULL, "latency", 0);
	if (lat > 0) sleep_ms((u64)lat);
	i64 f = conf_num("nvlsm", NULL, "fail", -1);
	if (f >= 0) { log_event("nvlsm", "FAIL", ""); return (int)f; }
	log_event("nvlsm", "ready", "");
	wait_forever();
	return 0;
}

/* ---------------------------- kata-agent ---------------------------- */
static int out_fd = 1;

static void emit(const char *s) { sys_write(out_fd, s, slen(s)); }

static char g_grep[48];
static int g_kmsg_emit;
static void kmsg_match(const char *msg) {
	if (!g_kmsg_emit || !g_grep[0] || !sfind(msg, g_grep) || sfind(msg, "ommand line")) return;
	emit("NVSD-KMSG ");
	const char *e = msg; while (*e && *e != '\n') e++;
	sys_write(out_fd, msg, (usize)(e - msg));
	emit("\n");
}

static u64 init_ts_us(void) {
	long fd = sys_open("/dev/kmsg", O_RDONLY | O_NONBLOCK | O_CLOEXEC, 0);
	if (fd < 0) return 0;
	static char rec[8192];
	u64 ts = 0;
	for (;;) {
		long n = sys_read((int)fd, rec, sizeof(rec) - 1);
		if (n < 0) { if (n == -EINTR) continue; break; }
		if (n == 0) break;
		rec[n] = 0;
		const char *semi = rec;
		while (*semi && *semi != ';') semi++;
		if (!*semi) continue;
		kmsg_match(semi + 1);
		if (!ts && sstarts(semi + 1, "Run /") && sfind(semi + 1, "as init process")) {
			/* "pri,seq,ts_us,flags;msg" */
			const char *p = rec;
			int commas = 0;
			while (*p && commas < 2) { if (*p == ',') commas++; p++; }
			ts = parse_u64_loose(p);
		}
	}
	sys_close((int)fd);
	return ts;
}

static u64 meminfo_kb(const char *text, const char *key) {
	const char *p = sfind(text, key);
	if (!p) return 0;
	p += slen(key);
	while (*p == ' ' || *p == ':' || *p == '\t') p++;
	return parse_u64_loose(p);
}

/* parse /proc/<pid>/stat into fields */
struct pstat { u64 ppid, utime, stime, cutime, cstime, threads, vsize, rss_pages; char comm[32]; };
static int read_pstat(const char *pid, struct pstat *ps) {
	char path[64], t[1024];
	struct buf pb = BUF_INIT(path);
	b_str(&pb, "/proc/"); b_str(&pb, pid); b_str(&pb, "/stat"); b_term(&pb);
	if (read_file(path, t, sizeof(t)) <= 0) return 0;
	const char *lp = t; while (*lp && *lp != '(') lp++;
	const char *rp = t + slen(t); while (rp > t && *rp != ')') rp--;
	if (!*lp || rp <= lp) return 0;
	usize cl = (usize)(rp - lp - 1); if (cl > 31) cl = 31;
	memcpy(ps->comm, lp + 1, cl); ps->comm[cl] = 0;
	const char *p = rp + 2; /* state */
	u64 f[24]; int idx = 0;
	/* skip state */
	while (*p && *p != ' ') p++;
	while (*p == ' ') p++;
	while (*p && idx < 22) {
		f[idx++] = parse_u64_loose(p);
		while (*p && *p != ' ') p++;
		while (*p == ' ') p++;
	}
	/* after state: ppid(0) pgrp sess tty tpgid flags minflt cminflt majflt cmajflt utime(10) stime(11) cutime(12) cstime(13) prio nice threads(16) itreal start vsize(19) rss(20) */
	ps->ppid = f[0]; ps->utime = f[10]; ps->stime = f[11]; ps->cutime = f[12]; ps->cstime = f[13];
	ps->threads = f[16]; ps->vsize = f[19]; ps->rss_pages = f[20];
	return 1;
}

static void rollup_kb(const char *pid, u64 *rss, u64 *pss) {
	char path[64], t[1536];
	struct buf pb = BUF_INIT(path);
	*rss = *pss = 0;
	b_str(&pb, "/proc/"); b_str(&pb, pid); b_str(&pb, "/smaps_rollup"); b_term(&pb);
	if (read_file(path, t, sizeof(t)) <= 0) return;
	*rss = meminfo_kb(t, "Rss:");
	*pss = meminfo_kb(t, "Pss:");
}

static u64 run_ns(const char *pid) {
	char path[64], t[128];
	struct buf pb = BUF_INIT(path);
	b_str(&pb, "/proc/"); b_str(&pb, pid); b_str(&pb, "/schedstat"); b_term(&pb);
	if (read_file(path, t, sizeof(t)) <= 0) return 0;
	return parse_u64_loose(t);
}

static void exe_of(const char *pid, char *out, usize cap) {
	char path[64];
	struct buf pb = BUF_INIT(path);
	b_str(&pb, "/proc/"); b_str(&pb, pid); b_str(&pb, "/exe"); b_term(&pb);
	long n = sc4(SYS_readlinkat, AT_FDCWD, (long)path, (long)out, cap - 1);
	out[n > 0 ? n : 0] = 0;
}

static void census(const char *label) {
	struct dirit d;
	if (dir_open(&d, "/proc") < 0) return;
	const char *name;
	u64 nproc = 0, nthreads = 0, nuser = 0, nuser_threads = 0;
	while ((name = dir_next(&d))) {
		if (name[0] < '0' || name[0] > '9') continue;
		struct pstat ps;
		if (!read_pstat(name, &ps)) continue;
		int kthread = ps.vsize == 0;
		nproc++; nthreads += ps.threads;
		if (!kthread) { nuser++; nuser_threads += ps.threads; } else continue;
		char l[500], exe[96];
		u64 rss, pss;
		struct buf b = BUF_INIT(l);
		rollup_kb(name, &rss, &pss);
		exe_of(name, exe, sizeof(exe));
		b_str(&b, "NVSD-PROC "); b_str(&b, label);
		b_str(&b, " pid="); b_str(&b, name);
		b_str(&b, " ppid="); b_u64(&b, ps.ppid);
		b_str(&b, " comm="); b_str(&b, ps.comm);
		b_str(&b, " exe="); b_str(&b, exe[0] ? exe : "?");
		b_str(&b, " threads="); b_u64(&b, ps.threads);
		b_str(&b, " utime_ticks="); b_u64(&b, ps.utime);
		b_str(&b, " stime_ticks="); b_u64(&b, ps.stime);
		b_str(&b, " cutime_ticks="); b_u64(&b, ps.cutime);
		b_str(&b, " cstime_ticks="); b_u64(&b, ps.cstime);
		b_str(&b, " run_ns="); b_u64(&b, run_ns(name));
		b_str(&b, " rss_kb="); b_u64(&b, rss);
		b_str(&b, " pss_kb="); b_u64(&b, pss);
		b_str(&b, "\n"); b_term(&b);
		emit(l);
	}
	dir_close(&d);
	char l[300];
	struct buf b = BUF_INIT(l);
	b_str(&b, "NVSD-CENSUS "); b_str(&b, label);
	b_str(&b, " procs="); b_u64(&b, nproc); b_str(&b, " threads="); b_u64(&b, nthreads);
	b_str(&b, " kthreads="); b_u64(&b, nproc - nuser);
	b_str(&b, " user_procs="); b_u64(&b, nuser); b_str(&b, " user_threads="); b_u64(&b, nuser_threads);
	b_str(&b, "\n"); b_term(&b);
	emit(l);
}

static void mem_line(const char *label) {
	static char mi[4096];
	read_file("/proc/meminfo", mi, sizeof(mi));
	u64 tot = meminfo_kb(mi, "MemTotal"), av = meminfo_kb(mi, "MemAvailable"), fr = meminfo_kb(mi, "MemFree");
	char l[300];
	struct buf b = BUF_INIT(l);
	b_str(&b, "NVSD-MEM "); b_str(&b, label);
	b_str(&b, " total_kb="); b_u64(&b, tot);
	b_str(&b, " available_kb="); b_u64(&b, av);
	b_str(&b, " free_kb="); b_u64(&b, fr);
	b_str(&b, " used_kb="); b_u64(&b, tot - av);
	b_str(&b, "\n"); b_term(&b);
	emit(l);
}

static void facts(void) {
	/* mount table: one line per mount (mountpoint fstype per-mount-options) */
	static char mi[16384];
	if (read_file("/proc/self/mountinfo", mi, sizeof(mi)) > 0) {
		const char *p = mi;
		while (*p) {
			const char *e = p;
			while (*e && *e != '\n') e++;
			/* fields: id parent maj:min root mountpoint opts [optional...] - fstype source superopts */
			const char *f[8]; int nf = 0; const char *q = p;
			while (q < e && nf < 6) { f[nf++] = q; while (q < e && *q != ' ') q++; while (q < e && *q == ' ') q++; }
			const char *dash = p;
			while (dash + 3 < e && !(dash[0] == ' ' && dash[1] == '-' && dash[2] == ' ')) dash++;
			if (nf == 6 && dash + 3 < e) {
				char l[300];
				struct buf b = BUF_INIT(l);
				b_str(&b, "NVSD-MOUNT ");
				const char *x = f[4]; while (x < e && *x != ' ') b_char(&b, *x++);
				b_char(&b, ' ');
				x = dash + 3; while (x < e && *x != ' ') b_char(&b, *x++);
				b_char(&b, ' ');
				x = f[5]; while (x < e && *x != ' ') b_char(&b, *x++);
				b_char(&b, '\n'); b_term(&b);
				emit(l);
			}
			p = *e ? e + 1 : e;
		}
	}
	static const struct { const char *key, *path; } F[] = {
		{ "lo_flags", "/sys/class/net/lo/flags" },
		{ "rmem_default", "/proc/sys/net/core/rmem_default" },
		{ "rmem_max", "/proc/sys/net/core/rmem_max" },
		{ "wmem_default", "/proc/sys/net/core/wmem_default" },
		{ "wmem_max", "/proc/sys/net/core/wmem_max" },
		{ "printk_devkmsg", "/proc/sys/kernel/printk_devkmsg" },
		{ "modules_disabled", "/proc/sys/kernel/modules_disabled" },
	};
	char l[600];
	struct buf b = BUF_INIT(l);
	b_str(&b, "NVSD-FACT");
	for (usize i = 0; i < sizeof(F) / sizeof(F[0]); i++) {
		char v[64] = "?";
		read_file(F[i].path, v, sizeof(v)); rtrim(v);
		/* printk_devkmsg reads like "on [ratelimit] off": keep the bracketed word */
		if (seq(F[i].key, "printk_devkmsg")) {
			char *o = v; while (*o && *o != '[') o++;
			if (*o) { o++; char *c = o; while (*c && *c != ']') c++; *c = 0; memmove(v, o, slen(o) + 1); }
		}
		b_char(&b, ' '); b_str(&b, F[i].key); b_char(&b, '='); b_str(&b, v);
	}
	b_str(&b, " dev_log="); b_str(&b, exists("/dev/log") ? "1" : "0");
	b_str(&b, "\n"); b_term(&b);
	emit(l);
}

static const char *env_get(const char *k) {
	usize kl = slen(k);
	for (char **e = g_envp; e && *e; e++)
		if (sstarts(*e, k) && (*e)[kl] == '=') return *e + kl + 1;
	return NULL;
}

static int do_kata(void) {
	u64 t_start = boottime_us();
	long fd = sys_open("/dev/console", O_WRONLY | 0400 /* O_NOCTTY */, 0);
	if (fd >= 0) out_fd = (int)fd;
	log_event("kata-agent", "start", g_joined);
	{
		char v[48];
		if (conf_get("kmsg_grep", v, sizeof(v))) memcpy(g_grep, v, slen(v) + 1);
	}
	u64 init_ts = init_ts_us();

	/* everything the real kata-agent environment would care about, sampled immediately */
	static char t[1024];
	char l[600];
	struct buf b = BUF_INIT(l);
	long n = read_file("/proc/1/stat", t, sizeof(t));
	struct pstat p1; memset(&p1, 0, sizeof(p1));
	if (n > 0) read_pstat("1", &p1);
	b_str(&b, "NVSD-KATA start_boottime_us="); b_u64(&b, t_start);
	b_str(&b, " init_ts_us="); b_u64(&b, init_ts);
	b_str(&b, " pid="); b_u64(&b, (u64)sys_getpid());
	b_str(&b, " pid1_comm="); b_str(&b, p1.comm);
	b_str(&b, " pid1_utime_ticks="); b_u64(&b, p1.utime);
	b_str(&b, " pid1_stime_ticks="); b_u64(&b, p1.stime);
	b_str(&b, " pid1_cutime_ticks="); b_u64(&b, p1.cutime);
	b_str(&b, " pid1_cstime_ticks="); b_u64(&b, p1.cstime);
	b_str(&b, " pid1_run_ns="); b_u64(&b, run_ns("1"));
	b_str(&b, "\n"); b_term(&b);
	emit(l);

	char oom[32] = "?", md[8] = "?";
	read_file("/proc/self/oom_score_adj", oom, sizeof(oom)); rtrim(oom);
	read_file("/proc/sys/kernel/modules_disabled", md, sizeof(md)); rtrim(md);
	struct { u64 cur, max; } rl = { 0, 0 };
	sc2(SYS_getrlimit, 7, (long)&rl);
	const char *av = env_get("KATA_ATTESTER_VARIANT");
	b.len = 0;
	b_str(&b, "NVSD-KATA-ENV oom_score_adj="); b_str(&b, oom);
	b_str(&b, " nofile_cur="); b_u64(&b, rl.cur);
	b_str(&b, " nofile_max="); b_u64(&b, rl.max);
	b_str(&b, " modules_disabled="); b_str(&b, md);
	b_str(&b, " attester_variant="); b_str(&b, av ? av : "unset");
	b_str(&b, "\n"); b_term(&b);
	emit(l);
	mem_line("at-start");
	census("at-start");
	facts();

	i64 grace = 200;
	{
		char v[16];
		if (conf_get("grace_ms", v, sizeof(v))) grace = (i64)parse_u64_loose(v);
	}
	if (grace > 0) sleep_ms((u64)grace);
	mem_line("after-grace");
	census("after-grace");
	g_kmsg_emit = 1;
	init_ts_us(); /* second pass over the kernel log only to print kmsg_grep matches */

	/* dump the stub log so the host can assert on ordering and argv */
	emit("NVSD-LOG-BEGIN\n");
	static char lg[32768];
	read_file(g_log, lg, sizeof(lg));
	emit(lg);
	emit("NVSD-LOG-END\n");
	emit("NVSD-REPORT-END\n");
	sc0(SYS_sync);
	/* LINUX_REBOOT_CMD_POWER_OFF */
	sc4(SYS_reboot, 0xfee1dead, 672274793, 0x4321FEDC, 0);
	sleep_ms(1000);
	return 0;
}

static int real_main(int argc, char **argv) {
	g_argc = argc; g_argv = argv;
	g_tool = sbase(argv[0]);
	if (seq(g_tool, "nvstub") && argc > 1) { g_argc--; g_argv++; g_tool = sbase(g_argv[0]); }
	read_file("/etc/nvstub.conf", g_conf, sizeof(g_conf));
	read_file("/proc/cmdline", g_cmdline, sizeof(g_cmdline));
	{
		char v[128];
		if (conf_get("log", v, sizeof(v)) && v[0] == '/') memcpy(g_log, v, slen(v) + 1);
	}
	{
		char v[8];
		g_echo = conf_get("echo", v, sizeof(v)) && v[0] == '1';
	}
	join_argv(1);
	if (seq(g_tool, "modprobe")) return do_modprobe();
	if (seq(g_tool, "nvidia-smi")) return do_smi();
	if (seq(g_tool, "nvidia-persistenced")) return do_persistenced();
	if (seq(g_tool, "nv-hostengine")) return do_hostengine();
	if (seq(g_tool, "dcgm-exporter")) return do_exporter();
	if (seq(g_tool, "nvidia-ctk")) return do_ctk();
	if (seq(g_tool, "nv-fabricmanager")) return do_fm();
	if (seq(g_tool, "nvlsm")) return do_nvlsm();
	if (seq(g_tool, "kata-agent")) return do_kata();
	say(2, "nvstub: unknown tool name\n");
	return 127;
}

void start_c(long *sp) {
	int argc = (int)*sp;
	char **argv = (char **)(sp + 1);
	g_envp = argv + argc + 1;
	sys_exit_(real_main(argc, argv));
}
__asm__(".text\n.global _start\n_start:\n\txor %ebp,%ebp\n\tmov %rsp,%rdi\n\tand $-16,%rsp\n\tcall start_c\n");
