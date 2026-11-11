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

extern void proc_name_kdp(task_t task, char * buf, int size);
extern int proc_pid_kdp(task_t task);
extern void IOSleep(unsigned milliseconds);

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
