/* SPDX-License-Identifier: Apache-2.0
 *
 * mini.h: a tiny freestanding (no libc) x86_64 Linux syscall layer shared by
 * the fixture stubs (fixture/stub/nvstub.c) and the nvsystemd helper
 * (helper/nvsd-helper.c). Binaries built from it are static, ~10-30 KiB and
 * start in well under a millisecond, which keeps the benchmark honest: the
 * stubs add the same negligible exec cost to both init systems.
 */
#ifndef NVSD_MINI_H
#define NVSD_MINI_H

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long u64;
typedef long i64;
typedef unsigned long usize;
typedef long isize;
#define NULL ((void *)0)

/* ---- raw syscalls ---- */
static inline long sc0(long n) {
	long r;
	__asm__ volatile("syscall" : "=a"(r) : "0"(n) : "rcx", "r11", "memory");
	return r;
}
static inline long sc1(long n, long a) {
	long r;
	__asm__ volatile("syscall" : "=a"(r) : "0"(n), "D"(a) : "rcx", "r11", "memory");
	return r;
}
static inline long sc2(long n, long a, long b) {
	long r;
	__asm__ volatile("syscall" : "=a"(r) : "0"(n), "D"(a), "S"(b) : "rcx", "r11", "memory");
	return r;
}
static inline long sc3(long n, long a, long b, long c) {
	long r;
	__asm__ volatile("syscall" : "=a"(r) : "0"(n), "D"(a), "S"(b), "d"(c) : "rcx", "r11", "memory");
	return r;
}
static inline long sc4(long n, long a, long b, long c, long d) {
	long r;
	register long r10 __asm__("r10") = d;
	__asm__ volatile("syscall" : "=a"(r) : "0"(n), "D"(a), "S"(b), "d"(c), "r"(r10) : "rcx", "r11", "memory");
	return r;
}
static inline long sc5(long n, long a, long b, long c, long d, long e) {
	long r;
	register long r10 __asm__("r10") = d;
	register long r8 __asm__("r8") = e;
	__asm__ volatile("syscall" : "=a"(r) : "0"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8) : "rcx", "r11", "memory");
	return r;
}

static inline long sc6(long n, long a, long b, long c, long d, long e, long f) {
	long r;
	register long r10 __asm__("r10") = d;
	register long r8 __asm__("r8") = e;
	register long r9 __asm__("r9") = f;
	__asm__ volatile("syscall" : "=a"(r) : "0"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9) : "rcx", "r11", "memory");
	return r;
}

#define SYS_read 0
#define SYS_write 1
#define SYS_close 3
#define SYS_poll 7
#define SYS_pause 34
#define SYS_nanosleep 35
#define SYS_getpid 39
#define SYS_socket 41
#define SYS_connect 42
#define SYS_sendto 44
#define SYS_recvfrom 45
#define SYS_fork 57
#define SYS_execve 59
#define SYS_exit 60
#define SYS_wait4 61
#define SYS_kill 62
#define SYS_getrlimit 97
#define SYS_mount 165
#define SYS_reboot 169
#define SYS_sync 162
#define SYS_getdents64 217
#define SYS_clock_gettime 228
#define SYS_exit_group 231
#define SYS_openat 257
#define SYS_mknodat 259
#define SYS_newfstatat 262
#define SYS_mkdirat 258
#define SYS_symlinkat 266
#define SYS_unlinkat 263
#define SYS_fchmodat 268
#define SYS_setsid 112
#define SYS_umask 95
#define SYS_inotify_init1 294
#define SYS_inotify_add_watch 254
#define SYS_ioctl 16
#define SYS_readlinkat 267
#define SYS_syslog 103

#define AT_FDCWD (-100)
#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR 2
#define O_CREAT 0100
#define O_EXCL 0200
#define O_TRUNC 01000
#define O_APPEND 02000
#define O_NONBLOCK 04000
#define O_DIRECTORY 0200000
#define O_CLOEXEC 02000000

#define CLOCK_MONOTONIC 1
#define CLOCK_BOOTTIME 7

#define EINTR 4
#define EAGAIN 11
#define ENOENT 2
#define EEXIST 17

#define AF_UNIX 1
#define SOCK_DGRAM 2

struct timespec_ { i64 sec; i64 nsec; };

static inline long sys_read(int fd, void *b, usize n) { return sc3(SYS_read, fd, (long)b, n); }
static inline long sys_write(int fd, const void *b, usize n) { return sc3(SYS_write, fd, (long)b, n); }
static inline long sys_close(int fd) { return sc1(SYS_close, fd); }
static inline long sys_open(const char *p, int fl, int mode) { return sc4(SYS_openat, AT_FDCWD, (long)p, fl, mode); }
static inline long sys_mkdir(const char *p, int mode) { return sc3(SYS_mkdirat, AT_FDCWD, (long)p, mode); }
static inline long sys_unlink(const char *p) { return sc3(SYS_unlinkat, AT_FDCWD, (long)p, 0); }
static inline long sys_symlink(const char *target, const char *p) { return sc3(SYS_symlinkat, (long)target, AT_FDCWD, (long)p); }
static inline long sys_chmod(const char *p, int mode) { return sc3(SYS_fchmodat, AT_FDCWD, (long)p, mode); }
static inline long sys_getpid(void) { return sc0(SYS_getpid); }
static inline long sys_fork(void) { return sc0(SYS_fork); }
static inline long sys_setsid(void) { return sc0(SYS_setsid); }
static inline long sys_wait4(long pid, int *st) { return sc4(SYS_wait4, pid, (long)st, 0, 0); }
static inline long sys_execve(const char *p, char *const *argv, char *const *envp) { return sc3(SYS_execve, (long)p, (long)argv, (long)envp); }
static inline long sys_clock(int clk, struct timespec_ *ts) { return sc2(SYS_clock_gettime, clk, (long)ts); }
static inline long sys_mount(const char *src, const char *tgt, const char *fs, unsigned long fl, const void *data) {
	return sc5(SYS_mount, (long)src, (long)tgt, (long)fs, fl, (long)data);
}
static inline long sys_mknod(const char *p, u32 mode, u32 dev) { return sc4(SYS_mknodat, AT_FDCWD, (long)p, mode, dev); }
static inline long sys_getdents(int fd, void *b, usize n) { return sc3(SYS_getdents64, fd, (long)b, n); }
static inline long sys_poll_fd(int fd, short events, int timeout_ms) {
	struct { int fd; short events; short revents; } p = { fd, events, 0 };
	long r = sc3(SYS_poll, (long)&p, 1, timeout_ms);
	return r < 0 ? r : (r == 0 ? 0 : p.revents);
}
static inline void sys_exit_(int c) { for (;;) sc1(SYS_exit_group, c); }

static inline u64 boottime_us(void) {
	struct timespec_ ts;
	sys_clock(CLOCK_BOOTTIME, &ts);
	return (u64)ts.sec * 1000000ull + (u64)ts.nsec / 1000;
}
static inline void sleep_ms(u64 ms) {
	struct timespec_ ts = { (i64)(ms / 1000), (i64)(ms % 1000) * 1000000 };
	while (sc2(SYS_nanosleep, (long)&ts, (long)&ts) == -EINTR) {}
}

/* ---- memory/string helpers (gcc may emit calls to mem*) ---- */
void *memcpy(void *d, const void *s, usize n);
void *memset(void *d, int c, usize n);
int memcmp(const void *a, const void *b, usize n);
void *memmove(void *d, const void *s, usize n);

#ifdef MINI_IMPL
void *memcpy(void *d, const void *s, usize n) {
	u8 *dd = d; const u8 *ss = s;
	while (n--) *dd++ = *ss++;
	return d;
}
void *memmove(void *d, const void *s, usize n) {
	u8 *dd = d; const u8 *ss = s;
	if (dd < ss) { while (n--) *dd++ = *ss++; }
	else { dd += n; ss += n; while (n--) *--dd = *--ss; }
	return d;
}
void *memset(void *d, int c, usize n) {
	u8 *dd = d;
	while (n--) *dd++ = (u8)c;
	return d;
}
int memcmp(const void *a, const void *b, usize n) {
	const u8 *x = a, *y = b;
	for (; n; n--, x++, y++) if (*x != *y) return *x - *y;
	return 0;
}
#endif

static inline usize slen(const char *s) { usize n = 0; while (s[n]) n++; return n; }
static inline int seq(const char *a, const char *b) {
	while (*a && *a == *b) { a++; b++; }
	return *a == *b;
}
static inline int sstarts(const char *s, const char *p) {
	while (*p) { if (*s++ != *p++) return 0; }
	return 1;
}
static inline const char *sfind(const char *h, const char *n) {
	usize nl = slen(n);
	if (!nl) return h;
	for (; *h; h++) if (*h == *n && sstarts(h, n)) return h;
	return NULL;
}
static inline const char *sbase(const char *p) {
	const char *b = p;
	for (; *p; p++) if (*p == '/') b = p + 1;
	return b;
}

/* ---- tiny output buffer ---- */
struct buf { char *p; usize len, cap; };
#define BUF_INIT(arr) { (arr), 0, sizeof(arr) }
static inline void b_char(struct buf *b, char c) { if (b->len + 1 < b->cap) b->p[b->len++] = c; }
static inline void b_mem(struct buf *b, const char *s, usize n) { while (n--) b_char(b, *s++); }
static inline void b_str(struct buf *b, const char *s) { while (*s) b_char(b, *s++); }
static inline void b_u64(struct buf *b, u64 v) {
	char t[24]; int n = 0;
	if (!v) t[n++] = '0';
	while (v) { t[n++] = '0' + v % 10; v /= 10; }
	while (n) b_char(b, t[--n]);
}
static inline void b_hex(struct buf *b, u64 v, int digits) {
	static const char H[] = "0123456789abcdef";
	for (int i = digits - 1; i >= 0; i--) b_char(b, H[(v >> (4 * i)) & 15]);
}
static inline void b_term(struct buf *b) { b->p[b->len < b->cap ? b->len : b->cap - 1] = 0; }

/* ---- parse helpers ---- */
/* returns 1 on success; strict decimal u32 (no sign, no empty, no overflow) like Rust's "u32::from_str" */
static inline int parse_u32(const char *s, u32 *out) {
	u64 v = 0;
	if (*s == '+') s++;
	if (!*s) return 0;
	for (; *s; s++) {
		if (*s < '0' || *s > '9') return 0;
		v = v * 10 + (u64)(*s - '0');
		if (v > 0xffffffffull) return 0;
	}
	*out = (u32)v;
	return 1;
}
static inline u64 parse_u64_loose(const char *s) {
	u64 v = 0;
	while (*s >= '0' && *s <= '9') v = v * 10 + (u64)(*s++ - '0');
	return v;
}

/* ---- file helpers ---- */
/* read a whole (small) file; returns bytes read or -errno; NUL-terminates */
static inline long read_file(const char *path, char *dst, usize cap) {
	long fd = sys_open(path, O_RDONLY | O_CLOEXEC, 0);
	if (fd < 0) return fd;
	usize n = 0;
	for (;;) {
		long r = sys_read((int)fd, dst + n, cap - 1 - n);
		if (r == -EINTR) continue;
		if (r <= 0) break;
		n += (usize)r;
		if (n >= cap - 1) break;
	}
	sys_close((int)fd);
	dst[n] = 0;
	return (long)n;
}
static inline long write_file(const char *path, const char *data, usize n, int extra_flags, int mode) {
	long fd = sys_open(path, O_WRONLY | O_CLOEXEC | extra_flags, mode);
	if (fd < 0) return fd;
	long r = sys_write((int)fd, data, n);
	sys_close((int)fd);
	return r;
}
static inline long append_file(const char *path, const char *data, usize n, int mode) {
	return write_file(path, data, n, O_CREAT | O_APPEND, mode);
}
static inline void say(int fd, const char *s) { sys_write(fd, s, slen(s)); }

/* fixture-style cmdline lookup: returns pointer into buf at the value of key=VALUE (NUL-terminated copy in out) */
static inline int cmdline_get(const char *cmdline, const char *key, char *out, usize cap) {
	usize kl = slen(key);
	int found = 0;
	const char *p = cmdline;
	while (*p) {
		while (*p == ' ' || *p == '\n' || *p == '\t') p++;
		const char *w = p;
		while (*p && *p != ' ' && *p != '\n' && *p != '\t') p++;
		usize wl = (usize)(p - w);
		if (wl > kl && w[kl] == '=' && sstarts(w, key)) {
			usize vl = wl - kl - 1;
			if (vl >= cap) vl = cap - 1;
			memcpy(out, w + kl + 1, vl);
			out[vl] = 0;
			found = 1; /* last wins */
		}
	}
	return found;
}

/* ---- directory iteration (getdents64) ---- */
struct dirit { int fd; long n, off; char buf[4096]; };
static inline int dir_open(struct dirit *d, const char *path) {
	long fd = sys_open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC, 0);
	if (fd < 0) return (int)fd;
	d->fd = (int)fd; d->n = 0; d->off = 0;
	return 0;
}
/* returns next entry name (skips . and ..) or NULL */
static inline const char *dir_next(struct dirit *d) {
	for (;;) {
		if (d->off >= d->n) {
			d->n = sys_getdents(d->fd, d->buf, sizeof(d->buf));
			d->off = 0;
			if (d->n <= 0) return NULL;
		}
		u8 *e = (u8 *)d->buf + d->off;
		u16 reclen = *(u16 *)(e + 16);
		const char *name = (const char *)(e + 19);
		d->off += reclen;
		if (seq(name, ".") || seq(name, "..")) continue;
		return name;
	}
}
static inline void dir_close(struct dirit *d) { sys_close(d->fd); }

/* trim trailing whitespace in place */
static inline void rtrim(char *s) {
	usize n = slen(s);
	while (n && (s[n - 1] == '\n' || s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r')) s[--n] = 0;
}
/* join path pieces into out */
static inline void path_join3(char *out, usize cap, const char *a, const char *b, const char *c) {
	struct buf o = { out, 0, cap };
	b_str(&o, a); b_char(&o, '/'); b_str(&o, b);
	if (c) { b_char(&o, '/'); b_str(&o, c); }
	b_term(&o);
}

/* Count PCI devices under <root> (e.g. /sys/bus/pci/devices) with vendor == vendor_str
 * (exact, after trim) and class starting with class_prefix (NULL = any).
 * Mirrors NVRC's mode.rs / modprobe.rs scans. */
static inline int pci_count(const char *root, const char *vendor_str, const char *class_prefix) {
	struct dirit d;
	int n = 0;
	if (dir_open(&d, root) < 0) return 0;
	const char *name;
	while ((name = dir_next(&d))) {
		char path[300], v[64], c[64];
		path_join3(path, sizeof(path), root, name, "vendor");
		if (read_file(path, v, sizeof(v)) < 0) continue;
		rtrim(v);
		if (!seq(v, vendor_str)) continue;
		if (class_prefix) {
			path_join3(path, sizeof(path), root, name, "class");
			if (read_file(path, c, sizeof(c)) < 0) continue;
			rtrim(c);
			if (!sstarts(c, class_prefix)) continue;
		}
		n++;
	}
	dir_close(&d);
	return n;
}

#endif
