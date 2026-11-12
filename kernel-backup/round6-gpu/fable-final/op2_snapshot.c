/*
 * OnePlus 2 diagnostic thread snapshots.
 *
 * Captures SpringBoard/backboardd user PCs periodically so a parked thread
 * shows up as a fixed PC+LR pair in ramoops. Armed only when op2_deadline
 * is set (diagnostic runs); op2_threaddump=0 disables.
 *
 * All work runs in a normal kernel thread (sleep locks allowed); nothing
 * here runs in interrupt context. Locking: tasks_threads_lock for the task
 * lookup, task_lock only to collect thread refs, no locks held across
 * state reads or kprintf.
 */

#if defined(ARM64_BOARD_CONFIG_MSM8994)

#include <kern/processor.h>
#include <kern/task.h>
#include <kern/thread.h>
#include <kern/locks.h>
#include <mach/arm/thread_status.h>
#include <mach/message.h>
#include <arm/pmap.h>
#include <vm/vm_map.h>
#include <pexpert/pexpert.h>
#include <kern/timer.h>
#include <kern/clock.h>

extern void proc_name_kdp(task_t task, char * buf, int size);
extern int proc_pid_kdp(task_t task);
extern void IOSleep(unsigned milliseconds);
extern int op2_temp_max, op2_temp_throttled;   /* op2_thermal_thread, below */

void op2_threaddump_start(void);

#define OP2_SNAP_MAX_THREADS 48
#define OP2_SNAP_MAX_SHOTS   8

/* Static VA of the mach_msg trap in this iOS build's libsystem_kernel.
 * Threads parked in mach_msg (x16 == -31) reveal the boot's dyld cache
 * slide: slide = pc - OP2_MACH_MSG_TRAP_STATIC. Logged once per boot
 * so every SNAP/EXIT log is self-symbolicating (no more slide voting). */
#define OP2_MACH_MSG_TRAP_STATIC 0x180a580f4ULL
static boolean_t op2_slide_logged = FALSE;

static int
op2_name_eq(const char *a, const char *b)
{
	while (*a && *a == *b) {
		a++;
		b++;
	}
	return *a == *b;
}

/*
 * Resident-only read of one user u64. Returns FALSE when unmapped or
 * split across a page boundary. Never faults.
 */
static boolean_t
op2_read_u64(vm_map_t map, uint64_t uaddr, uint64_t *out)
{
	if (map == VM_MAP_NULL || uaddr == 0 ||
	    ((uaddr & 0xFFF) + sizeof(uint64_t) > 0x1000)) {
		return FALSE;
	}
	vm_offset_t pa = pmap_extract(map->pmap, (vm_map_offset_t)uaddr);
	if (pa == 0) {
		return FALSE;
	}
	*out = *(volatile uint64_t *)phystokv(pa);
	return TRUE;
}

static void
op2_snapshot_task(const char *want, int shot)
{
	task_t task = TASK_NULL;
	task_t t;

	lck_mtx_lock(&tasks_threads_lock);
	queue_iterate(&tasks, t, task_t, tasks) {
		if (!t->active || t->bsd_info == NULL || task_is_a_corpse(t)) {
			continue;
		}
		char comm[32];
		proc_name_kdp(t, comm, sizeof(comm));
		if (op2_name_eq(comm, want)) {
			task_reference(t);
			task = t;
			break;
		}
	}
	lck_mtx_unlock(&tasks_threads_lock);

	if (task == TASK_NULL) {
		kprintf("OP2 SNAP #%d: %s: task not present\n", shot, want);
		return;
	}

	thread_t grabbed[OP2_SNAP_MAX_THREADS];
	int grabbed_count = 0;
	task_lock(task);
	thread_t th;
	queue_iterate(&task->threads, th, thread_t, task_threads) {
		if (grabbed_count >= OP2_SNAP_MAX_THREADS) {
			break;
		}
		thread_reference(th);
		grabbed[grabbed_count++] = th;
	}
	int total = task->thread_count;
	task_unlock(task);

	kprintf("OP2 SNAP #%d: %s: %d thread(s)%s\n", shot, want, total,
	    total > grabbed_count ? " (truncated)" : "");
	vm_map_t map = task->map;
	/*
	 * Dyld image info: per-boot ASLR slides needed to map user PCs
	 * back to static disassembly. Layout (dyld_all_image_infos):
	 * +0x0 version, +0x8 infoArray, +0x50 sharedCacheSlide,
	 * +0x68 sharedCacheBaseAddress. infoArray[0] is the main
	 * executable; its imageLoadAddress gives the executable slide.
	 */
	uint64_t info = task->all_image_info_addr;
	uint64_t ver = 0, slide = 0, base = 0, arr = 0, exec = 0;
	if (info != 0 && map != VM_MAP_NULL) {
		op2_read_u64(map, info, &ver);
		op2_read_u64(map, info + 0x50, &slide);
		op2_read_u64(map, info + 0x68, &base);
		if (op2_read_u64(map, info + 0x8, &arr) && arr != 0) {
			op2_read_u64(map, arr, &exec);
		}
	}
	kprintf("OP2 SNAP #%d: %s pid=%d dyld info=0x%llx ver=%u exec=0x%llx\n",
	    shot, want, proc_pid_kdp(task), info, (uint32_t)ver, exec);
	for (int i = 0; i < grabbed_count; i++) {
		arm_thread_state64_t state;
		mach_msg_type_number_t count = ARM_THREAD_STATE64_COUNT;
		kern_return_t kr = machine_thread_get_state(grabbed[i],
		    ARM_THREAD_STATE64, (thread_state_t)&state, &count);
		if (kr != KERN_SUCCESS) {
			kprintf("OP2 SNAP #%d: %s T%llu: state err %d\n",
			    shot, want, grabbed[i]->thread_id, kr);
			thread_deallocate(grabbed[i]);
			continue;
		}
		if (state.pc == 0 && state.sp == 0 && state.lr == 0) {
			thread_deallocate(grabbed[i]);
			continue;
		}
		/*
		 * Best-effort message header: threads parked in mach_msg
		 * have x0 = user address of the message. Resident-only read
		 * via pmap (no faults); skipped when unmapped or split
		 * across a page boundary.
		 */
		char msgbuf[80];
		msgbuf[0] = '\0';
		if (state.x[0] != 0 && map != VM_MAP_NULL &&
		    ((state.x[0] & 0xFFF) + 24 <= 0x1000)) {
			vm_offset_t pa = pmap_extract(map->pmap, (vm_map_offset_t)state.x[0]);
			if (pa != 0) {
				/*
				 * Userland (LP64) message header layout; the
				 * kernel's mach_msg_header_t uses pointers
				 * and has different field offsets.
				 */
				struct {
					uint32_t bits, size;
					uint32_t remote, local, voucher;
					int32_t id;
				} *uhdr = (void *)phystokv(pa);
				if (uhdr->size >= 24 && uhdr->size <= 8192) {
					snprintf(msgbuf, sizeof(msgbuf),
					    " msg{id=%d sz=%u rmt=0x%x lcl=0x%x}",
					    uhdr->id, uhdr->size,
					    uhdr->remote, uhdr->local);
				}
			}
		}
		/*
		 * Frame-pointer chain: up to 8 saved return addresses.
		 * Follows ascending fp only (stack grows down); stops at
		 * unmapped pages. x16 holds the trap number at svc sites.
		 */
		char stkbuf[340];
		int stklen = 0;
		uint64_t fp = state.fp;
		stkbuf[0] = '\0';
		for (int f = 0; f < 16 && fp != 0; f++) {
			uint64_t prev = 0, ret = 0;
			if (!op2_read_u64(map, fp, &prev) ||
			    !op2_read_u64(map, fp + 8, &ret) || ret == 0) {
				break;
			}
			if (stklen >= (int)sizeof(stkbuf) - 20) {
				break;
			}
			stklen += snprintf(stkbuf + stklen,
			    sizeof(stkbuf) - (size_t)stklen,
			    "%s0x%llx", f ? "," : " stk=", ret);
			if (prev == 0 || prev <= fp) {
				break;
			}
			fp = prev;
		}
		if (!op2_slide_logged && state.x[16] == 0xFFFFFFFFFFFFFFE1ULL &&
		    state.pc > OP2_MACH_MSG_TRAP_STATIC) {
			op2_slide_logged = TRUE;
			kprintf("OP2 SLIDE: 0x%llx (dyld cache slide, from %s T%llu mach_msg pc)\n",
			    state.pc - OP2_MACH_MSG_TRAP_STATIC,
			    want, grabbed[i]->thread_id);
		}
		kprintf("OP2 SNAP #%d: %s T%llu pc=0x%llx lr=0x%llx sp=0x%llx fp=0x%llx "
		    "x0=0x%llx x1=0x%llx x2=0x%llx x3=0x%llx x4=0x%llx x5=0x%llx x16=0x%llx "
		    "tstate=%d susp=%d wev=0x%llx%s%s\n",
		    shot, want, grabbed[i]->thread_id,
		    state.pc, state.lr, state.sp, state.fp,
		    state.x[0], state.x[1], state.x[2],
		    state.x[3], state.x[4], state.x[5], state.x[16],
		    grabbed[i]->state, grabbed[i]->suspend_count,
		    (uint64_t)grabbed[i]->wait_event,
		    msgbuf, stkbuf);
		thread_deallocate(grabbed[i]);
	}
	task_deallocate(task);
}


/*
 * OP2 TOP: every op2_top seconds (default 30, 0 = off) log the processes
 * that used the most CPU in the interval, as % of ONE core (4 cores = 400%).
 * Reads per-thread user/system timers under task_lock; no allocation.
 */
#define OP2_TOP_MAX 192
struct op2_top_ent {
	int      pid;
	uint64_t total;
	uint64_t delta;
	char     name[16];
};
static struct op2_top_ent op2_top_cur[OP2_TOP_MAX];
static struct op2_top_ent op2_top_prev[OP2_TOP_MAX];
static int op2_top_prev_n = 0;
static uint64_t op2_top_last_blk = 0, op2_top_last_mcmd = 0;
extern uint64_t op2_emmc_blocks_read(void);
extern uint64_t op2_emmc_multi_count(void);
extern void op2_fs_sync(void);
extern uint32_t op2_cpu_mhz_now;

static void
op2_top_sample(uint64_t interval_abs)
{
	int n = 0;
	task_t t;

	lck_mtx_lock(&tasks_threads_lock);
	queue_iterate(&tasks, t, task_t, tasks) {
		if (n >= OP2_TOP_MAX) {
			break;
		}
		if (!t->active) {
			continue;
		}
		uint64_t total;
		thread_t th;
		task_lock(t);
		total = t->total_user_time + t->total_system_time;
		queue_iterate(&t->threads, th, thread_t, task_threads) {
			if (th->state & TH_IDLE) {
				continue;   /* per-CPU idle threads live in kernel_task */
			}
			total += timer_grab(&th->user_timer) + timer_grab(&th->system_timer);
		}
		task_unlock(t);
		struct op2_top_ent *e = &op2_top_cur[n++];
		e->pid = (t->bsd_info != NULL) ? proc_pid_kdp(t) : 0;
		e->total = total;
		e->delta = 0;
		if (t->bsd_info != NULL) {
			proc_name_kdp(t, e->name, sizeof(e->name));
		} else {
			strlcpy(e->name, "kernel_task", sizeof(e->name));
		}
	}
	lck_mtx_unlock(&tasks_threads_lock);

	uint64_t sum = 0;
	for (int i = 0; i < n; i++) {
		struct op2_top_ent *e = &op2_top_cur[i];
		for (int j = 0; j < op2_top_prev_n; j++) {
			if (op2_top_prev[j].pid == e->pid && e->total >= op2_top_prev[j].total) {
				e->delta = e->total - op2_top_prev[j].total;
				break;
			}
		}
		sum += e->delta;
	}

	if (op2_top_prev_n > 0 && interval_abs > 0) {
		char line[256];
		uint64_t blk = op2_emmc_blocks_read();
		uint64_t mcmd = op2_emmc_multi_count();
		uint64_t secs = interval_abs / 19200000ULL;
		if (secs == 0) {
			secs = 1;
		}
		int len = snprintf(line, sizeof(line), "OP2 TOP: %uMHz T%dC%s busy %llu%%/400 emmc %lluKB/s (%llu multi):",
		    op2_cpu_mhz_now, op2_temp_max, op2_temp_throttled ? "!" : "", (sum * 100ULL) / interval_abs,
		    ((blk - op2_top_last_blk) / 2ULL) / secs, mcmd - op2_top_last_mcmd);
		op2_top_last_blk = blk;
		op2_top_last_mcmd = mcmd;
		for (int k = 0; k < 7; k++) {
			int best = -1;
			for (int i = 0; i < n; i++) {
				if (op2_top_cur[i].delta > 0 &&
				    (best < 0 || op2_top_cur[i].delta > op2_top_cur[best].delta)) {
					best = i;
				}
			}
			if (best < 0) {
				break;
			}
			uint64_t pct = (op2_top_cur[best].delta * 100ULL) / interval_abs;
			if (pct == 0) {
				break;
			}
			if (len > 0 && len < (int)sizeof(line)) {
				len += snprintf(line + len, sizeof(line) - len, " %s(%d)=%llu%%",
				    op2_top_cur[best].name, op2_top_cur[best].pid, pct);
			}
			op2_top_cur[best].delta = 0;
		}
		printf("%s\n", line);
	}

	for (int i = 0; i < n; i++) {
		op2_top_prev[i] = op2_top_cur[i];
	}
	op2_top_prev_n = n;
}

/*
 * THERMAL (2026-11-13): the user reported the phone "superhot"; nothing in this kernel read a temperature. TSENS
 * (msm8994: TYPE2 controller at 0xfc4a8000, 16 sensors, status 0x1044 + 4n: 10-bit code, VALID 0x4000; fuses at
 * 0xfc4bc000 + 0xd0, reference drivers/thermal/msm8974-tsens.c tsens_calib_8994_sensors). Every 2 s: the hottest
 * sensor. Above op2_thot (default 70 C) the A53s drop from 1536 to 768 MHz and LK's voltage (op2_cpu_set_throttle), up below
 * op2_thot - 10. Above op2_tcrit (default 90 C) twice in a row: sync, dump, panic -> reset to TWRP, whose kernel
 * does thermal management. The OP2 TOP line carries "T<max>C" (and a "T!" while throttled).
 * Boot-args: op2_therm=0 off, op2_thot, op2_tcrit.
 */
extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern int op2_cpu_set_throttle(int on);
extern void op2_fs_sync(void);
static volatile uint32_t *op2_tsens, *op2_tsens_fuse;
static int32_t op2_t_slope[16], op2_t_offset[16];   /* x1000, as the Linux driver */
static int op2_t_calibrated;
int op2_temp_max = -1000;                             /* last hottest reading, C (shown on OP2 TOP) */
int op2_temp_throttled;

static const int32_t op2_t_dt_slope[16] = { 2901, 2846, 3200, 3200, 3200, 3200, 3200, 3200, 3200, 3200, 3200, 3200,
	3200, 3200, 3200, 3200 };

static void
op2_tsens_calibrate(void)
{
	uint32_t d0 = op2_tsens_fuse[0xd0 / 4], d1 = op2_tsens_fuse[0xd4 / 4], d2 = op2_tsens_fuse[0xd8 / 4];
	uint32_t redun = (op2_tsens_fuse[0x464 / 4] & 0xe0000000u) >> 29;
	uint32_t mode = (d2 & 0x700000u) >> 20;
	int32_t point[16], base0 = 0, base1 = 0;
	if (redun != 3 && mode == 3) {
		base0 = (int32_t)(d0 & 0x3ff);
		base1 = (int32_t)((d0 & 0xffc00) >> 10);
		point[0] = (d0 >> 20) & 0xf; point[1] = (d0 >> 24) & 0xf; point[2] = (d0 >> 28) & 0xf;
		for (int i = 0; i < 8; i++)
			point[3 + i] = (d1 >> (4 * i)) & 0xf;
		for (int i = 0; i < 5; i++)
			point[11 + i] = (d2 >> (4 * i)) & 0xf;
		op2_t_calibrated = 1;
	} else {
		for (int i = 0; i < 16; i++)
			point[i] = 532;
	}
	for (int i = 0; i < 16; i++) {
		op2_t_slope[i] = op2_t_calibrated ? (base1 - base0) * 1000 / 90 : op2_t_dt_slope[i];
		if (op2_t_slope[i] <= 0)
			op2_t_slope[i] = op2_t_dt_slope[i];
		op2_t_offset[i] = (base0 + point[i]) * 1000 - 30 * op2_t_slope[i];
	}
	printf("OP2 THERM: TSENS fuses %08x %08x %08x redun %u mode %u: %s (base %d/%d)\n", d0, d1, d2, redun, mode,
	    op2_t_calibrated ? "two-point calibrated" : "uncalibrated defaults", base0, base1);
}

/* one sensor in C, or -1000 when it never reads valid */
static int
op2_tsens_read(int n)
{
	for (int tries = 0; tries < 3; tries++) {
		uint32_t v = op2_tsens[(0x1044 + 4 * n) / 4];
		if (v & 0x4000) {
			int32_t code = (int32_t)(v & 0x3ff);
			int32_t num = code * 1000 - op2_t_offset[n];
			return (num >= 0 ? num + op2_t_slope[n] / 2 : num - op2_t_slope[n] / 2) / op2_t_slope[n];
		}
	}
	return -1000;
}

static void
op2_thermal_thread(__unused void *param, __unused wait_result_t wr)
{
	uint32_t hot = 70, crit = 90;   /* stock msm_thermal starts limiting the A53s at 60 C (sensor 7) */
	PE_parse_boot_argn("op2_thot", &hot, sizeof(hot));
	PE_parse_boot_argn("op2_tcrit", &crit, sizeof(crit));
	op2_tsens = (volatile uint32_t *)ml_io_map(0xfc4a8000, 0x2000);
	op2_tsens_fuse = (volatile uint32_t *)ml_io_map(0xfc4bc000, 0x1000);
	if (!op2_tsens || !op2_tsens_fuse) {
		printf("OP2 THERM: cannot map TSENS\n");
		return;
	}
	op2_tsens_calibrate();
	int crit_count = 0;
	for (uint32_t tick = 0;; tick++) {
		int t[16], mx = -1000, valid = 0;
		for (int i = 0; i < 16; i++) {
			t[i] = op2_tsens_read(i);
			if (t[i] > -1000) {
				valid++;
				if (t[i] > mx)
					mx = t[i];
			}
		}
		op2_temp_max = mx;
		if (tick == 0 || (tick % 150) == 0) {   /* all sixteen at start and every 5 min */
			printf("OP2 THERM: %d valid; C: %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d\n", valid, t[0], t[1],
			    t[2], t[3], t[4], t[5], t[6], t[7], t[8], t[9], t[10], t[11], t[12], t[13], t[14], t[15]);
		}
		if (valid > 0 && mx > -40 && mx < 150) {   /* plausible readings only drive decisions */
			if (!op2_temp_throttled && mx >= (int)hot) {
				op2_temp_throttled = 1;
				int mhz = op2_cpu_set_throttle(1);
				printf("OP2 THERM: %d C >= %u: CPU throttled to ~%d MHz\n", mx, hot, mhz);
			} else if (op2_temp_throttled && mx <= (int)hot - 10) {   /* 60 C with the default */
				op2_temp_throttled = 0;
				int mhz = op2_cpu_set_throttle(0);
				printf("OP2 THERM: %d C: CPU back to ~%d MHz\n", mx, mhz);
			}
			crit_count = mx >= (int)crit ? crit_count + 1 : 0;
			if (crit_count >= 2) {
				printf("OP2 THERM: %d C >= %u twice: syncing and resetting to recovery\n", mx, crit);
				op2_fs_sync();
				IOSleep(2000);
				panic("OP2: thermal cutoff at %d C\n", mx);
			}
		}
		IOSleep(2000);
	}
}

static uint32_t op2_top_interval = 30;

static void __attribute__((noreturn))
op2_top_thread(void *param, wait_result_t wr)
{
	(void)param;
	(void)wr;
	uint64_t last = mach_absolute_time();
	uint32_t deadline = 0;
	int synced = 0;
	PE_parse_boot_argn("op2_deadline", &deadline, sizeof(deadline));
	op2_top_sample(0);
	for (;;) {
		IOSleep(op2_top_interval * 1000);
		uint64_t now = mach_absolute_time();
		op2_top_sample(now - last);
		last = now;
		/* Two syncs in the last ~40 s before the deadline reset. */
		if (deadline > 60) {
			uint64_t secs = now / 19200000ULL;
			if (synced == 0 && secs + 45 >= deadline) {
				op2_fs_sync();
				synced = 1;
			} else if (synced == 1 && secs + 15 >= deadline) {
				op2_fs_sync();
				synced = 2;
			}
		}
	}
}

static uint32_t op2_snap_deadline = 0;

static void __attribute__((noreturn))
op2_threaddump_thread(void *param, wait_result_t wr)
{
	(void)param;
	(void)wr;
	/*
	 * The ramoops ring only keeps ~60-90s, so anchor the shots to
	 * the end of the run: first shot 240s before the deadline, then
	 * every 30s. Short runs fall back to starting at 30s.
	 */
	uint32_t first = 30;
	if (op2_snap_deadline > 270) {
		first = op2_snap_deadline - 240;
	}
	IOSleep(first * 1000);
	for (int shot = 1; shot <= OP2_SNAP_MAX_SHOTS; shot++) {
		op2_snapshot_task("SpringBoard", shot);
		op2_snapshot_task("backboardd", shot);
		op2_snapshot_task("mobileassetd", shot);
		op2_snapshot_task("Setup", shot);
		op2_snapshot_task("budd", shot);
		op2_snapshot_task("mobileactivation", shot);
		IOSleep(30000);
	}
	/* Coverage done; sleep quietly (wedge state is static by then). */
	for (;;) {
		IOSleep(3600000);
	}
}

void
op2_threaddump_start(void)
{
	static boolean_t started = FALSE;
	uint32_t deadline = 0;
	int enable = 1;

	if (started) {
		return;
	}
	started = TRUE;
	uint32_t therm = 1;
	PE_parse_boot_argn("op2_therm", &therm, sizeof(therm));
	if (therm) {
		thread_t thth;
		if (kernel_thread_start(op2_thermal_thread, NULL, &thth) == KERN_SUCCESS) {
			thread_deallocate(thth);
		}
	}
	PE_parse_boot_argn("op2_top", &op2_top_interval, sizeof(op2_top_interval));
	if (op2_top_interval != 0) {
		thread_t topth;
		if (kernel_thread_start(op2_top_thread, NULL, &topth) == KERN_SUCCESS) {
			thread_deallocate(topth);
			printf("OP2: CPU top sampler every %us\n", op2_top_interval);
		}
	}
	PE_parse_boot_argn("op2_deadline", &deadline, sizeof(deadline));
	PE_parse_boot_argn("op2_threaddump", &enable, sizeof(enable));
	if (deadline == 0 || !enable) {
		return;
	}
	op2_snap_deadline = deadline;
	thread_t th;
	if (kernel_thread_start(op2_threaddump_thread, NULL, &th) == KERN_SUCCESS) {
		thread_deallocate(th);
		kprintf("OP2: thread-dump snapshots armed (deadline %us)\n", deadline);
	}
}

#endif /* defined(ARM64_BOARD_CONFIG_MSM8994) */
