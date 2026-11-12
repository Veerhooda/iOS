/*
 * op2bench: how fast is the foundation? (2026-11-12: "home screen slow and Settings lag on the CPU builds too")
 * Runs as /usr/libexec/tzinit (tools/deploy-probe.sh), forks away from launchd, waits for the boot to settle, then
 * measures and writes "BENCH ..." lines to the kernel log (OP2GPU selector 0x7c, as OP2Metal does):
 *   cpu      dependent integer adds per ns (A53 @1.5 GHz: ~1.5 /ns; A57 @1.96: ~2; Apple A8 @1.4: ~1.4 but 6-wide)
 *   ilp      four independent add chains (an in-order dual-issue A53 tops out near 2x cpu)
 *   memcpy   GB/s over 64 MB (LPDDR4 at full speed with A53s: ~3-5 GB/s; a bus left at its floor: well under 1)
 *   lat      ns per load, random chase over 32 KB (L1), 256 KB (L2), 64 MB (DRAM: ~120-180 ns at speed)
 *   wake     us for a round trip between two threads on a condition variable (cross-core: ~10-40 us)
 *   pipe     us for a round trip between two processes over pipes
 *   sleep    how long usleep(1000) and usleep(16000) really take
 *   syscall  ns per getppid()
 *   cpus     which CPU each of 4 spinning threads lands on (all four should be used)
 */
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <CoreFoundation/CoreFoundation.h>

typedef mach_port_t io_object_t, io_service_t, io_connect_t;
extern const mach_port_t kIOMasterPortDefault;
CFMutableDictionaryRef IOServiceMatching(const char *name);
io_service_t IOServiceGetMatchingService(mach_port_t master, CFDictionaryRef matching);
kern_return_t IOServiceOpen(io_service_t s, task_port_t t, uint32_t type, io_connect_t *c);
kern_return_t IOObjectRelease(io_object_t o);
kern_return_t IOConnectCallScalarMethod(io_connect_t c, uint32_t sel, const uint64_t *in, uint32_t inCnt,
                                        uint64_t *out, uint32_t *outCnt);

static io_connect_t gConn;
static void
klog(const char *fmt, ...)
{
	char line[128] = "BENCH ";
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(line + 6, sizeof line - 6, fmt, ap);
	va_end(ap);
	fprintf(stderr, "%s\n", line);
	if (!gConn) {
		io_service_t s = IOServiceGetMatchingService(kIOMasterPortDefault, IOServiceMatching("OP2GPU"));
		if (!s)
			return;
		IOServiceOpen(s, mach_task_self(), 0, &gConn);
		IOObjectRelease(s);
		if (!gConn)
			return;
	}
	uint64_t w[16] = {0};
	size_t n = strlen(line);
	memcpy(w, line, n > 127 ? 127 : n);
	IOConnectCallScalarMethod(gConn, 0x7c, w, 16, NULL, NULL);
}

static double tb_ns;
static uint64_t now(void) { return mach_absolute_time(); }
static double ns(uint64_t t) { return t * tb_ns; }

static double
cpu_chain(void)
{
	volatile uint64_t sink;
	uint64_t a = 1, n = 200000000;
	uint64_t t0 = now();
	for (uint64_t i = 0; i < n; i++)
		__asm__ volatile("add %0, %0, #1" : "+r"(a));
	sink = a;
	(void)sink;
	return n / ns(now() - t0);
}

static double
cpu_ilp(void)
{
	volatile uint64_t sink;
	uint64_t a = 1, b = 2, c = 3, d = 4, n = 100000000;
	uint64_t t0 = now();
	for (uint64_t i = 0; i < n; i++)
		__asm__ volatile("add %0, %0, #1\n add %1, %1, #1\n add %2, %2, #1\n add %3, %3, #1"
		                 : "+r"(a), "+r"(b), "+r"(c), "+r"(d));
	sink = a + b + c + d;
	(void)sink;
	return 4 * n / ns(now() - t0);
}

static double
memcpy_gbs(void)
{
	size_t sz = 64u << 20;
	char *a = malloc(sz), *b = malloc(sz);
	memset(a, 1, sz);
	memset(b, 2, sz);
	memcpy(b, a, sz);
	uint64_t t0 = now();
	for (int i = 0; i < 4; i++)
		memcpy(i & 1 ? a : b, i & 1 ? b : a, sz);
	double t = ns(now() - t0);
	volatile char keep = a[sz / 3] + b[sz / 7];   /* the copies are used: the compiler may not drop them */
	(void)keep;
	free(a);
	free(b);
	return 4.0 * sz / t;   /* bytes per ns = GB/s */
}

static double
chase_ns(size_t bytes)
{
	size_t n = bytes / sizeof(void *);
	void **p = malloc(n * sizeof(void *));
	size_t *perm = malloc(n * sizeof(size_t));
	for (size_t i = 0; i < n; i++)
		perm[i] = i;
	srand(7);
	for (size_t i = n - 1; i > 0; i--) {
		size_t j = ((size_t)rand() << 16 ^ (size_t)rand()) % (i + 1), t = perm[i];
		perm[i] = perm[j];
		perm[j] = t;
	}
	for (size_t i = 0; i < n; i++)
		p[perm[i]] = &p[perm[(i + 1) % n]];
	void **q = (void **)p[perm[0]];
	size_t steps = 4000000;
	for (size_t i = 0; i < n; i++)
		q = (void **)*q;
	uint64_t t0 = now();
	for (size_t i = 0; i < steps; i++)
		q = (void **)*q;
	double t = ns(now() - t0) / steps;
	if (!q)
		klog("unreachable");
	free(p);
	free(perm);
	return t;
}

static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static int turn, rounds_left;

static void *
pong(void *arg)
{
	(void)arg;
	pthread_mutex_lock(&m);
	while (rounds_left > 0) {
		while (turn != 1 && rounds_left > 0)
			pthread_cond_wait(&cv, &m);
		turn = 0;
		pthread_cond_broadcast(&cv);
	}
	pthread_mutex_unlock(&m);
	return NULL;
}

static double
wake_us(int rounds)
{
	pthread_t t;
	rounds_left = rounds;
	turn = 0;
	pthread_create(&t, NULL, pong, NULL);
	usleep(10000);
	uint64_t t0 = now();
	pthread_mutex_lock(&m);
	for (int i = 0; i < rounds; i++) {
		turn = 1;
		pthread_cond_broadcast(&cv);
		while (turn != 0)
			pthread_cond_wait(&cv, &m);
	}
	rounds_left = 0;
	pthread_cond_broadcast(&cv);
	pthread_mutex_unlock(&m);
	double us = ns(now() - t0) / rounds / 1000;
	pthread_join(t, NULL);
	return us;
}

static double
pipe_us(int rounds)
{
	int ab[2], ba[2];
	if (pipe(ab) || pipe(ba))
		return -1;
	pid_t pid = fork();
	char c = 0;
	if (pid == 0) {
		for (int i = 0; i < rounds; i++) {
			if (read(ab[0], &c, 1) != 1)
				break;
			if (write(ba[1], &c, 1) != 1)
				break;
		}
		_exit(0);
	}
	uint64_t t0 = now();
	for (int i = 0; i < rounds; i++) {
		if (write(ab[1], &c, 1) != 1 || read(ba[0], &c, 1) != 1)
			return -2;
	}
	return ns(now() - t0) / rounds / 1000;
}

static void
sleep_check(useconds_t us, int reps)
{
	double mn = 1e18, mx = 0, sum = 0;
	for (int i = 0; i < reps; i++) {
		uint64_t t0 = now();
		usleep(us);
		double t = ns(now() - t0) / 1000;
		sum += t;
		if (t < mn) mn = t;
		if (t > mx) mx = t;
	}
	klog("sleep %u us: avg %.0f min %.0f max %.0f us", us, sum / reps, mn, mx);
}

static double
syscall_ns(void)
{
	int n = 200000;
	uint64_t t0 = now();
	for (int i = 0; i < n; i++)
		getppid();
	return ns(now() - t0) / n;
}

static volatile int spin_stop;
static void *
spin(void *arg)
{
	(void)arg;
	while (!spin_stop)
		;
	return NULL;
}

static void
parallel_cpu(void)
{
	/* four spinners: total throughput and whether all CPUs work (an idle CPU shows as no speedup) */
	pthread_t t[4];
	spin_stop = 0;
	for (int i = 0; i < 3; i++)
		pthread_create(&t[i], NULL, spin, NULL);
	usleep(20000);
	double one = cpu_chain();
	spin_stop = 1;
	for (int i = 0; i < 3; i++)
		pthread_join(t[i], NULL);
	klog("cpu with 3 spinners alongside: %.2f adds/ns (a CPU of its own should give the same as alone)", one);
}

/* storage: what a page fault and an app launch pay. The shared cache, past what boot has touched, uncached. */
static void
disk_check(void)
{
	const char *path = "/System/Library/Caches/com.apple.dyld/dyld_shared_cache_arm64";
	int fd = open(path, O_RDONLY);
	if (fd < 0) {
		klog("disk: cannot open the shared cache");
		return;
	}
	fcntl(fd, F_NOCACHE, 1);
	struct stat st;
	fstat(fd, &st);
	size_t chunk = 1u << 20;
	char *buf = malloc(chunk);
	off_t base = st.st_size / 2;   /* middle of 1.2 GB: unlikely to be resident */
	uint64_t t0 = now();
	size_t total = 0;
	for (int i = 0; i < 24; i++) {
		ssize_t r = pread(fd, buf, chunk, base + (off_t)i * chunk);
		if (r <= 0)
			break;
		total += (size_t)r;
	}
	double t = ns(now() - t0);
	klog("disk sequential 1 MB reads: %.1f MB/s (%zu MB)", total / t * 1000.0, total >> 20);
	srand(11);
	double mx = 0, sum = 0;
	int n = 64;
	for (int i = 0; i < n; i++) {
		off_t o = ((off_t)(((uint64_t)rand() << 16) ^ (uint64_t)rand()) % (st.st_size / 4096)) * 4096;
		uint64_t a = now();
		if (pread(fd, buf, 4096, o) != 4096)
			break;
		double us = ns(now() - a) / 1000;
		sum += us;
		if (us > mx)
			mx = us;
	}
	klog("disk random 4 KB reads: avg %.0f us, max %.0f us", sum / n, mx);
	t0 = now();
	total = 0;
	for (int i = 0; i < 256; i++) {
		ssize_t r = pread(fd, buf, 16384, base / 2 + (off_t)i * 16384 * 4);
		if (r <= 0)
			break;
		total += (size_t)r;
	}
	t = ns(now() - t0);
	klog("disk strided 16 KB reads: %.1f MB/s, %.0f us each", total / t * 1000.0, t / 256 / 1000);
	free(buf);
	close(fd);
}

static void
run(const char *phase)
{
	klog("%s: start", phase);
	klog("cpu %.2f adds/ns, ilp %.2f adds/ns", cpu_chain(), cpu_ilp());
	klog("memcpy %.2f GB/s", memcpy_gbs());
	klog("lat L1 %.1f ns, L2 %.1f ns, DRAM %.1f ns", chase_ns(16 << 10), chase_ns(256 << 10), chase_ns(64 << 20));
	klog("wake (threads, condvar) %.1f us round trip", wake_us(2000));
	klog("pipe (processes) %.1f us round trip", pipe_us(2000));
	klog("syscall getppid %.0f ns", syscall_ns());
	disk_check();
	sleep_check(1000, 50);
	sleep_check(16000, 20);
	parallel_cpu();
	klog("%s: done", phase);
}

int
main(void)
{
	mach_timebase_info_data_t tb;
	mach_timebase_info(&tb);
	tb_ns = (double)tb.numer / tb.denom;
	int ncpu = 0;
	size_t l = sizeof ncpu;
	sysctlbyname("hw.ncpu", &ncpu, &l, NULL, 0);
	klog("op2bench pid %d, timebase %u/%u, %d cpus", getpid(), tb.numer, tb.denom, ncpu);
	pid_t p = fork();
	if (p != 0)
		return 0;   /* launchd's boot task ends here; the child measures once the boot has settled */
	setsid();
	sleep(150);
	run("settled");
	sleep(120);
	run("idle");
	return 0;
}
