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

int main_(int argc, char **argv) {
	if (argc < 2) return die("main", "usage: nvsd-helper <subcommand>");
	read_file("/proc/cmdline", g_cmdline, sizeof(g_cmdline));
	const char *sub = argv[1];
	if (seq(sub, "cmdline")) return cmd_cmdline();
	return die("main", "unknown subcommand");
}

void start_c(long *sp) {
	int argc = (int)*sp;
	char **argv = (char **)(sp + 1);
	sys_exit_(main_(argc, argv));
}
__asm__(".text\n.global _start\n_start:\n\txor %ebp,%ebp\n\tmov %rsp,%rdi\n\tand $-16,%rsp\n\tcall start_c\n");
