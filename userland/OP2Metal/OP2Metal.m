/*
 * OP2Metal: Apple Metal Driver Bundle implementation for OnePlus 2 (Snapdragon 810 / Adreno 430).
 *
 * Provides userspace Metal driver interface compliant with Apple's Metal.framework
 * and IOAccelerator.framework architecture on iOS 12.5.7 (16H81).
 */

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/message.h>
#import <objc/runtime.h>
#include <sys/sysctl.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <errno.h>
#include <IOSurface/IOSurfaceRef.h>
#include "op2_metallib.h"
#include "op2mtl.h"   /* userland/op2mtl: the GPU driver core this shim maps Metal onto (G5b) */

#ifndef MTLHazardTrackingModeDefault
#define MTLHazardTrackingModeDefault 0
#endif

#define OP2_RETURNS_RETAINED __attribute__((ns_returns_retained))
#define OP2_INIT_METHOD __attribute__((ns_returns_retained))

/* IOKit Declarations */
typedef mach_port_t io_object_t;
typedef io_object_t io_service_t;
typedef io_object_t io_connect_t;
extern const mach_port_t kIOMasterPortDefault;
CFMutableDictionaryRef IOServiceMatching(const char *name);
io_service_t IOServiceGetMatchingService(mach_port_t masterPort, CFDictionaryRef matching);
kern_return_t IOServiceOpen(io_service_t service, task_port_t owningTask, uint32_t type, io_connect_t *connect);
kern_return_t IOServiceClose(io_connect_t connect);
kern_return_t IOObjectRelease(io_object_t object);
kern_return_t IOConnectCallScalarMethod(io_connect_t connect, uint32_t selector,
    const uint64_t *input, uint32_t inputCnt,
    uint64_t *output, uint32_t *outputCnt);

static Class gBaseDevice;
static unsigned int gAcceleratorPort;
static id gDeviceInstance;
static BOOL gDeviceAdded = NO;

static void kernelLog(const char *line);

/*
 * G5a trace: every line goes to the kernel log (OP2GPU kOP2Log -> ramoops; NSLog never reaches it), as
 * "M <process>: <message>". Deduplicated per process on the message with its digits dropped (pointers,
 * counters), so each distinct call shape shows once; at most 480 distinct lines per process.
 */
static void
op2_log(NSString *format, ...)
{
	va_list args;
	va_start(args, format);
	NSString *msg = [[NSString alloc] initWithFormat:format arguments:args];
	va_end(args);
	const char *m = [msg UTF8String];
	uint32_t h = 2166136261u;
	for (const char *c = m; *c; c++) {
		if ((*c >= '0' && *c <= '9') || *c == 'x') continue;
		h = (h ^ (uint8_t)*c) * 16777619u;
	}
	static uint32_t seen[480];
	static unsigned n;
	for (unsigned i = 0; i < n; i++)
		if (seen[i] == h) return;
	if (n >= 480) return;
	seen[n++] = h;
	char line[128];
	snprintf(line, sizeof line, "M %s: %s", getprogname(), m);
	kernelLog(line);
}

/*
 * G5b trace lines whose numbers matter (pipeline descriptors, bindings, constant buffer bytes): only an exact
 * repeat is dropped, at most 700 lines per process (backboardd's 119 pipelines used up 360 in round g5b-6).
 */
static void
op2_trace(const char *fmt, ...)
{
	static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
	static uint32_t seen[700];
	static unsigned n;
	char line[128];
	int k = snprintf(line, sizeof line, "M %s: ", getprogname());
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(line + k, sizeof line - (size_t)k, fmt, ap);
	va_end(ap);
	uint32_t h = 2166136261u;
	for (const char *c = line; *c; c++)
		h = (h ^ (uint8_t)*c) * 16777619u;
	pthread_mutex_lock(&lock);
	BOOL fresh = n < 700;
	for (unsigned i = 0; fresh && i < n; i++)
		if (seen[i] == h)
			fresh = NO;
	if (fresh)
		seen[n++] = h;
	pthread_mutex_unlock(&lock);
	if (fresh)
		kernelLog(line);
}

/*
 * ---- G5b: the GPU behind the shim ----
 * /usr/local/lib/op2mtl.dylib (Mesa fd4 + the AIR front end behind a C API, userland/op2mtl) is dlopened the
 * first time a process makes a Metal object. If it is absent (its rootfs slot is all zeros), of another API
 * version, or the GPU does not open, gMtl stays NULL and every object below is today's CPU stub.
 */
#define OP2MTL_CALLS(X) \
	X(unsigned, version, (void)) \
	X(const char *, last_error, (void)) \
	X(void, set_log, (void (*)(const char *))) \
	X(op2mtl_device *, device_open, (void)) \
	X(const char *, device_name, (op2mtl_device *)) \
	X(op2mtl_library *, library_new, (op2mtl_device *, const void *, size_t)) \
	X(void, library_free, (op2mtl_library *)) \
	X(op2mtl_pipeline *, pipeline_new, (op2mtl_device *, const struct op2mtl_pipeline_desc *)) \
	X(void, pipeline_free, (op2mtl_pipeline *)) \
	X(op2mtl_buffer *, buffer_new, (op2mtl_device *, size_t)) \
	X(void *, buffer_contents, (op2mtl_buffer *)) \
	X(void, buffer_free, (op2mtl_buffer *)) \
	X(op2mtl_texture *, texture_new, (op2mtl_device *, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t)) \
	X(op2mtl_texture *, texture_from_iosurface, (op2mtl_device *, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t)) \
	X(bool, texture_replace, (op2mtl_texture *, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, const void *, size_t)) \
	X(bool, texture_read, (op2mtl_texture *, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, void *, size_t)) \
	X(void, texture_free, (op2mtl_texture *)) \
	X(op2mtl_sampler *, sampler_new, (op2mtl_device *, const struct op2mtl_sampler_desc *)) \
	X(void, sampler_free, (op2mtl_sampler *)) \
	X(bool, pass_begin, (op2mtl_device *, const struct op2mtl_pass_attachment *)) \
	X(void, set_pipeline, (op2mtl_device *, op2mtl_pipeline *)) \
	X(void, set_viewport, (op2mtl_device *, double, double, double, double, double, double)) \
	X(void, set_scissor, (op2mtl_device *, uint32_t, uint32_t, uint32_t, uint32_t)) \
	X(void, set_buffer, (op2mtl_device *, unsigned, unsigned, op2mtl_buffer *, size_t)) \
	X(void, set_buffer_offset, (op2mtl_device *, unsigned, unsigned, size_t)) \
	X(void, set_bytes, (op2mtl_device *, unsigned, unsigned, const void *, size_t)) \
	X(void, set_texture, (op2mtl_device *, unsigned, unsigned, op2mtl_texture *)) \
	X(void, set_sampler, (op2mtl_device *, unsigned, unsigned, op2mtl_sampler *)) \
	X(bool, draw, (op2mtl_device *, uint32_t, uint32_t, uint32_t, uint32_t)) \
	X(bool, draw_indexed, (op2mtl_device *, uint32_t, uint32_t, uint32_t, op2mtl_buffer *, size_t, uint32_t)) \
	X(void, pass_end, (op2mtl_device *)) \
	X(uint64_t, commit, (op2mtl_device *)) \
	X(bool, wait, (op2mtl_device *, uint64_t, uint64_t))

#define X(ret, name, args) ret (*name) args;
static struct { OP2MTL_CALLS(X) } M;
#undef X
static op2mtl_device *gMtl;   /* NULL: no GPU, the CPU stubs */
/*
 * op2mtl encodes straight into one Gallium context: a render pass must not interleave with another thread's
 * pass, nor a commit land in the middle of one. Held from renderCommandEncoder to endEncoding, and around
 * commit + wait. The draw and commit counters below are kept under it too.
 */
static pthread_mutex_t gPassLock = PTHREAD_MUTEX_INITIALIZER;
static void gpuCommit(void);
static void *watchdogThread(void *arg);

static void
mtlLogSink(const char *line)
{
	op2_trace("L %.100s", line);
}

static void
mtlOpen(void)
{
	void *h = dlopen("/usr/local/lib/op2mtl.dylib", RTLD_NOW | RTLD_LOCAL);
	if (!h) {
		const char *e = dlerror();
		op2_trace("G op2mtl not loaded (CPU stubs): %.80s", e ? e : "?");
		return;
	}
#define X(ret, name, args) \
	M.name = (ret (*) args)dlsym(h, "op2mtl_" #name); \
	if (!M.name) { op2_trace("G op2mtl lacks op2mtl_%s (CPU stubs)", #name); return; }
	OP2MTL_CALLS(X)
#undef X
	if (M.version() != OP2MTL_VERSION) {
		op2_trace("G op2mtl API %u, this shim wants %u (CPU stubs)", M.version(), OP2MTL_VERSION);
		return;
	}
	M.set_log(mtlLogSink);
	op2mtl_device *d = M.device_open();
	if (!d) {
		op2_trace("G op2mtl device: %.90s (CPU stubs)", M.last_error());
		return;
	}
	gMtl = d;
	op2_trace("G GPU rendering through op2mtl (%.60s)", M.device_name(d));
	if (!strcmp(getprogname(), "backboardd"))   /* the last boot's frame captures are not this boot's */
		for (unsigned n = 0; n < 8; n++) {
			char path[64];
			snprintf(path, sizeof path, "/private/var/tmp/op2cap-%u.bgra", n);
			unlink(path);
		}
	/* where the code is in this process, so a crash's pc can be found in the unstripped builds (atos -l) */
	Dl_info a, b;
	if (dladdr((const void *)M.version, &a) && dladdr((const void *)&mtlOpen, &b))
		op2_trace("G images: op2mtl.dylib at %p, OP2Metal at %p", a.dli_fbase, b.dli_fbase);
	pthread_t w;
	if (pthread_create(&w, NULL, watchdogThread, NULL) == 0)
		pthread_detach(w);
}

/* the GPU device, opened on first use; NULL when this process renders with the CPU stubs */
static op2mtl_device *
mtl(void)
{
	static dispatch_once_t once;
	dispatch_once(&once, ^{ mtlOpen(); });
	return gMtl;
}

static double
msSince(uint64_t t0)
{
	static mach_timebase_info_data_t tb;
	if (!tb.denom)
		mach_timebase_info(&tb);
	return (double)(mach_absolute_time() - t0) * tb.numer / tb.denom / 1e6;
}

/* the first n bytes as hex, for the binding trace */
static const char *
hexOf(const void *p, size_t n, char *out, size_t cap)
{
	static const char hx[] = "0123456789abcdef";
	size_t k = 0;
	for (size_t i = 0; p && i < n && k + 2 < cap; i++) {
		uint8_t b = ((const uint8_t *)p)[i];
		out[k++] = hx[b >> 4];
		out[k++] = hx[b & 15];
	}
	out[k] = 0;
	return out;
}

/*
 * G5b stuck-watchdog. Round g5b-4: backboardd spun at 200% CPU after making its render target, before its first
 * GPU commit, for the rest of the boot, and nothing said where. A GPU operation (pass, commit, pipeline, texture)
 * marks itself busy; if one is still busy after 20 s, or no GPU commit has happened 30 s after the last pipeline
 * was made, the threads using CPU are sampled: pc, lr and the frame-pointer chain, as image+offset for atos.
 * Stacks are copied while each thread is suspended and only resolved (dladdr, logging) after it is resumed, so a
 * thread suspended inside dyld or malloc cannot deadlock the sampler.
 */
static volatile uint64_t gBusySince, gLastPipelineAt;
static const char *volatile gBusyWhat;
static volatile unsigned gCommitsSeen;

static void
busyBegin(const char *what)
{
	gBusyWhat = what;
	gBusySince = mach_absolute_time();
}

static void
busyEnd(void)
{
	gBusySince = 0;
}

static void
addrText(uint64_t a, char *out, size_t cap)
{
	Dl_info i;
	if (a && dladdr((const void *)a, &i) && i.dli_fbase) {
		const char *n = i.dli_fname ? strrchr(i.dli_fname, '/') : NULL;
		n = n ? n + 1 : (i.dli_fname ? i.dli_fname : "?");
		snprintf(out, cap, "%.12s+%llx", n, (unsigned long long)(a - (uint64_t)i.dli_fbase));
	} else {
		snprintf(out, cap, "%llx", (unsigned long long)a);
	}
}

#define W_THREADS 8
#define W_FRAMES 14

static void
sampleThreads(const char *why)
{
	char line[128];
	snprintf(line, sizeof line, "M %s: W %s", getprogname(), why);
	kernelLog(line);
	thread_act_array_t th;
	mach_msg_type_number_t n;
	if (task_threads(mach_task_self(), &th, &n) != KERN_SUCCESS)
		return;
	thread_t me = mach_thread_self();
	uint64_t pcs[W_THREADS][W_FRAMES];
	unsigned np[W_THREADS], idx[W_THREADS], cpu[W_THREADS], nt = 0;
	for (unsigned i = 0; i < n; i++) {
		struct thread_basic_info bi;
		mach_msg_type_number_t bc = THREAD_BASIC_INFO_COUNT;
		if (th[i] != me && nt < W_THREADS &&
		    thread_info(th[i], THREAD_BASIC_INFO, (thread_info_t)&bi, &bc) == KERN_SUCCESS &&
		    bi.cpu_usage > TH_USAGE_SCALE / 4 && thread_suspend(th[i]) == KERN_SUCCESS) {
			arm_thread_state64_t st;
			mach_msg_type_number_t sc = ARM_THREAD_STATE64_COUNT;
			unsigned k = 0;
			if (thread_get_state(th[i], ARM_THREAD_STATE64, (thread_state_t)&st, &sc) == KERN_SUCCESS) {
				pcs[nt][k++] = st.__pc;
				pcs[nt][k++] = st.__lr;
				uint64_t fp = st.__fp;
				while (k < W_FRAMES && fp && !(fp & 7)) {
					uint64_t fr[2];
					vm_size_t got = 0;
					if (vm_read_overwrite(mach_task_self(), (vm_address_t)fp, sizeof fr, (vm_address_t)fr, &got) !=
					        KERN_SUCCESS || got != sizeof fr)
						break;
					if (fr[1])
						pcs[nt][k++] = fr[1];
					if (fr[0] <= fp)
						break;
					fp = fr[0];
				}
			}
			thread_resume(th[i]);
			np[nt] = k;
			idx[nt] = i;
			cpu[nt] = (unsigned)bi.cpu_usage / 10;
			nt++;
		}
		mach_port_deallocate(mach_task_self(), th[i]);
	}
	mach_port_deallocate(mach_task_self(), me);
	vm_deallocate(mach_task_self(), (vm_address_t)th, n * sizeof th[0]);

	for (unsigned t = 0; t < nt; t++) {
		size_t k = 0;
		for (unsigned j = 0; j < np[t]; j++) {
			char a[48];
			addrText(pcs[t][j], a, sizeof a);
			if (k == 0)
				k = (size_t)snprintf(line, sizeof line, "M %s: W t%u %u%%%s", getprogname(), idx[t], cpu[t],
				                     j ? " .." : "");
			k += (size_t)snprintf(line + k, sizeof line - k, " %s", a);
			if (k > 88 || j + 1 == np[t]) {
				kernelLog(line);
				k = 0;
			}
		}
	}
	if (!nt) {
		snprintf(line, sizeof line, "M %s: W no thread is using CPU", getprogname());
		kernelLog(line);
	}
}

static void *
watchdogThread(__unused void *arg)
{
	unsigned samples = 0;
	BOOL idleSampled = NO;
	while (samples < 3) {
		sleep(5);
		uint64_t t = gBusySince, lp = gLastPipelineAt;
		char why[100];
		if (t && msSince(t) > 20000) {
			snprintf(why, sizeof why, "stuck %.0f s in %s", msSince(t) / 1000, gBusyWhat ? gBusyWhat : "?");
			sampleThreads(why);
			samples++;
			sleep(30);
		} else if (!idleSampled && lp && !gCommitsSeen && msSince(lp) > 30000) {
			snprintf(why, sizeof why, "no GPU commit %.0f s after the last pipeline", msSince(lp) / 1000);
			sampleThreads(why);
			idleSampled = YES;
			samples++;
		}
	}
	return NULL;
}

/* Structs and architectures */
struct MTLTargetDeviceArch {
	uint64_t target;
	uint32_t version;
	const char *name;
};

static struct MTLTargetDeviceArch gTargetArch = {
	.target = 0,
	.version = 0,
	.name = "Adreno430"
};

static const struct MTLTargetDeviceArch *
deviceTargetDeviceInfo(__unused id self, __unused SEL _cmd)
{
	return &gTargetArch;
}

/* Protocol registration and dynamic selector forwarding */
static Protocol *gProtocols[128];
static unsigned gProtocolCount = 0;

static id
deviceMethodSignature(id self, SEL _cmd, SEL selector)
{
	(void)self;
	(void)_cmd;
	for (unsigned i = 0; i < gProtocolCount; i++) {
		struct objc_method_description desc = protocol_getMethodDescription(gProtocols[i], selector, NO, YES);
		if (desc.name == NULL) {
			desc = protocol_getMethodDescription(gProtocols[i], selector, YES, YES);
		}
		if (desc.name != NULL && desc.types != NULL) {
			return [NSMethodSignature signatureWithObjCTypes:desc.types];
		}
	}
	return [NSMethodSignature signatureWithObjCTypes:"v@:"];
}

static void
deviceForwardInvocation(id self, SEL _cmd, NSInvocation *invocation)
{
	SEL sel = [invocation selector];
	NSMethodSignature *sig = [invocation methodSignature];
	NSUInteger retLen = [sig methodReturnLength];

	op2_log(@"unimpl -[%s %s] ret %lu", object_getClassName(self), sel_getName(sel), (unsigned long)retLen);

	if (retLen > 0) {
		void *zeroBuffer = calloc(1, retLen);
		[invocation setReturnValue:zeroBuffer];
		free(zeroBuffer);
	}
}

/* Forward declarations */
static Class gQueueClass;
static Class gBufferClass;
static Class gLibraryClass;
static Class gFunctionClass;
static Class gCommandBufferClass;
static Class gBlitEncoderClass;
static Class gRenderEncoderClass;
static Class gTextureClass;
static Class gPipelineClass;

/* Internal ivar access */
static void **
stateOf(__unsafe_unretained id self)
{
	Ivar ivar = class_getInstanceVariable(object_getClass(self), "_op2State");
	if (ivar == NULL) {
		return NULL;
	}
	return (void **)((uint8_t *)(__bridge void *)self + ivar_getOffset(ivar));
}

static void
deallocWith(__unsafe_unretained id self, SEL _cmd, __unused Class cls, void (*freeCb)(void *))
{
	void **slot = stateOf(self);
	if (slot != NULL && *slot != NULL) {
		if (freeCb) {
			freeCb(*slot);
		}
		*slot = NULL;
	}
	struct objc_super sup = { self, objc_getClass("NSObject") };
	void (*send)(struct objc_super *, SEL) = (void (*)(struct objc_super *, SEL))objc_msgSendSuper;
	send(&sup, _cmd);
}

static id
objectDevice(__unused id self, __unused SEL _cmd)
{
	return gDeviceInstance;
}

/* ---- Buffers ---- */
struct op2_buffer {
	void *bytes;
	NSUInteger length;
	NSUInteger options;
	void (^deallocator)(void *, NSUInteger);
	NSString *label;
	uint64_t gpuAddress;
	uint32_t purgeableState;
	op2mtl_buffer *gpu;   /* G5b: the GPU buffer whose CPU mapping `bytes` is; NULL = plain memory */
};

static void
bufferFree(void *state)
{
	struct op2_buffer *b = (struct op2_buffer *)state;
	if (b->gpu) {
		M.buffer_free(b->gpu);
	} else if (b->deallocator) {
		b->deallocator(b->bytes, b->length);
	} else if (b->bytes) {
		free(b->bytes);
	}
	b->label = nil;
	free(b);
}

static void
bufferDealloc(__unsafe_unretained id self, SEL _cmd)
{
	deallocWith(self, _cmd, gBufferClass, bufferFree);
}

static struct op2_buffer *
bufferOf(id self)
{
	void **slot = stateOf(self);
	return slot != NULL ? (struct op2_buffer *)*slot : NULL;
}

static void *
bufferContents(id self, __unused SEL _cmd)
{
	struct op2_buffer *b = bufferOf(self);
	return b != NULL ? b->bytes : NULL;
}

static NSUInteger
bufferLength(id self, __unused SEL _cmd)
{
	struct op2_buffer *b = bufferOf(self);
	return b != NULL ? b->length : 0;
}

static NSUInteger
bufferOptions(id self, __unused SEL _cmd)
{
	struct op2_buffer *b = bufferOf(self);
	return b != NULL ? b->options : 0;
}

static NSUInteger
bufferStorageMode(id self, __unused SEL _cmd)
{
	struct op2_buffer *b = bufferOf(self);
	return b != NULL ? ((b->options >> MTLResourceStorageModeShift) & 0xf) : MTLStorageModeShared;
}

static NSUInteger
bufferCacheMode(id self, __unused SEL _cmd)
{
	struct op2_buffer *b = bufferOf(self);
	return b != NULL ? ((b->options >> MTLResourceCPUCacheModeShift) & 0xf) : MTLCPUCacheModeDefaultCache;
}

static NSUInteger
bufferHazardMode(id self, __unused SEL _cmd)
{
	struct op2_buffer *b = bufferOf(self);
	return b != NULL ? ((b->options >> MTLResourceHazardTrackingModeShift) & 0x3) : MTLHazardTrackingModeDefault;
}

static NSUInteger
bufferAllocatedSize(id self, __unused SEL _cmd)
{
	struct op2_buffer *b = bufferOf(self);
	return b != NULL ? b->length : 0;
}

static void
bufferDidModify(__unused id self, __unused SEL _cmd, __unused NSRange range)
{
}

static uint64_t
bufferGPUAddress(id self, __unused SEL _cmd)
{
	struct op2_buffer *b = bufferOf(self);
	return b != NULL ? b->gpuAddress : 0;
}

static NSUInteger
bufferPurgeable(id self, __unused SEL _cmd, NSUInteger state)
{
	struct op2_buffer *b = bufferOf(self);
	if (b == NULL) {
		return MTLPurgeableStateNonVolatile;
	}
	NSUInteger prev = b->purgeableState;
	if (state != MTLPurgeableStateKeepCurrent) {
		b->purgeableState = (uint32_t)state;
	}
	return prev;
}

OP2_RETURNS_RETAINED static id
deviceNewBufferBytes(id self, SEL _cmd, const void *bytes, NSUInteger length, NSUInteger options)
{
	id buf = ((id (*)(id, SEL, NSUInteger, NSUInteger))objc_msgSend)(self, sel_registerName("newBufferWithLength:options:"), length, options);
	void *dest = bufferContents(buf, NULL);
	if (dest && bytes && length) {
		memcpy(dest, bytes, length);
	}
	return buf;
}

OP2_RETURNS_RETAINED static id
deviceNewBufferNoCopy(__unused id self, __unused SEL _cmd, void *pointer, NSUInteger length, NSUInteger options, void (^deallocator)(void *, NSUInteger))
{
	if (gBufferClass == Nil || pointer == NULL) {
		return nil;
	}
	id bufObj = [[gBufferClass alloc] init];
	struct op2_buffer *buf = calloc(1, sizeof(*buf));
	void **slot = stateOf(bufObj);
	if (buf && slot) {
		buf->bytes = pointer;
		buf->length = length;
		buf->options = options;
		buf->deallocator = [deallocator copy];
		buf->purgeableState = MTLPurgeableStateNonVolatile;
		*slot = buf;
		return bufObj;
	}
	free(buf);
	return nil;
}

OP2_RETURNS_RETAINED static id
deviceNewBuffer(id self, SEL _cmd, NSUInteger length, NSUInteger options)
{
	/* G5b: a GPU buffer; `contents` is its own CPU mapping, so what CA writes there is what the GPU reads */
	op2mtl_device *dev = mtl();
	if (dev) {
		op2mtl_buffer *g = M.buffer_new(dev, length);
		void *map = g ? M.buffer_contents(g) : NULL;
		if (map) {
			memset(map, 0, length);
			id buf = deviceNewBufferNoCopy(self, _cmd, map, length, options, NULL);
			struct op2_buffer *b = buf ? bufferOf(buf) : NULL;
			if (b) {
				b->gpu = g;
				return buf;
			}
		}
		if (g)
			M.buffer_free(g);
		op2_log(@"G buffer of %lu bytes not on the GPU: %s", (unsigned long)length, M.last_error());
	}
	void *mem = calloc(1, length ? length : 1);
	if (mem == NULL) {
		return nil;
	}
	id buf = deviceNewBufferNoCopy(self, _cmd, mem, length, options, NULL);
	if (buf == nil) {
		free(mem);
	}
	return buf;
}

/* ---- Command Queue ---- */
static id
queueDevice(__unused id self, __unused SEL _cmd)
{
	return gDeviceInstance;
}

static id
queueLabel(__unused id self, __unused SEL _cmd)
{
	return @"OP2MetalQueue";
}

static void
queueSetLabel(__unused id self, __unused SEL _cmd, __unused NSString *label)
{
}

static void
queueSetGPUPriority(__unused id self, __unused SEL _cmd, __unused unsigned long priority)
{
}

static void
queueSetGPUPriorityOffset(__unused id self, __unused SEL _cmd, __unused unsigned long priority, __unused unsigned long offset)
{
}

static unsigned long
queueGetGPUPriority(__unused id self, __unused SEL _cmd)
{
	return 1;
}

static void
queueSetBackgroundGPUPriority(__unused id self, __unused SEL _cmd, __unused unsigned long priority)
{
}

static void
queueSetBackgroundGPUPriorityOffset(__unused id self, __unused SEL _cmd, __unused unsigned long priority, __unused unsigned long offset)
{
}

static unsigned long
queueGetBackgroundGPUPriority(__unused id self, __unused SEL _cmd)
{
	return 1;
}

static void
queueSetDispatchQueue(__unused id self, __unused SEL _cmd, __unused dispatch_queue_t queue)
{
}

static void
queueFinish(__unused id self, __unused SEL _cmd)
{
}

/* ---- Command Buffer ---- */
struct op2_cmd_buf {
	id queue;
	id presentedDrawable;
	unsigned long status;
	NSMutableArray *handlers;   /* completed / scheduled handlers, run once the GPU is done (G5b) */
	BOOL work;                  /* a render pass of this buffer went to the GPU */
};

static void
cmdBufFree(void *state)
{
	struct op2_cmd_buf *cb = (struct op2_cmd_buf *)state;
	cb->queue = nil;
	cb->presentedDrawable = nil;
	cb->handlers = nil;
	free(cb);
}

static void
queueDealloc(__unsafe_unretained id self, SEL _cmd)
{
	struct objc_super sup = { self, objc_getClass("NSObject") };
	void (*send)(struct objc_super *, SEL) = (void (*)(struct objc_super *, SEL))objc_msgSendSuper;
	send(&sup, _cmd);
}

static void
commandBufferDealloc(__unsafe_unretained id self, SEL _cmd)
{
	deallocWith(self, _cmd, gCommandBufferClass, cmdBufFree);
}

static id
queueCommandBuffer(id self, __unused SEL _cmd)
{
	id cb = [[gCommandBufferClass alloc] init];
	struct op2_cmd_buf *state = calloc(1, sizeof(*state));
	void **slot = stateOf(cb);
	if (state && slot) {
		state->queue = self;
		state->status = MTLCommandBufferStatusNotEnqueued;
		*slot = state;
	}
	return cb;
}

OP2_RETURNS_RETAINED static id
deviceNewQueue(id self, __unused SEL _cmd)
{
	gDeviceInstance = self;
	if (gQueueClass == Nil) {
		return nil;
	}
	id queue = [[gQueueClass alloc] init];
	op2_log(@"newCommandQueue: queue instance %p created", (__bridge void *)queue);
	return queue;
}

OP2_RETURNS_RETAINED static id
deviceNewQueueMax(id self, SEL _cmd, __unused unsigned long maxBuffers)
{
	return deviceNewQueue(self, _cmd);
}

static unsigned long
commandBufferStatus(id self, __unused SEL _cmd)
{
	void **slot = stateOf(self);
	struct op2_cmd_buf *cb = slot ? (struct op2_cmd_buf *)*slot : NULL;
	return cb ? cb->status : MTLCommandBufferStatusCompleted;
}

static id
commandBufferError(__unused id self, __unused SEL _cmd)
{
	return nil;
}

static void
commandBufferCommit(id self, __unused SEL _cmd)
{
	void **slot = stateOf(self);
	struct op2_cmd_buf *cb = slot ? (struct op2_cmd_buf *)*slot : NULL;
	if (cb) {
		cb->status = MTLCommandBufferStatusCommitted;
		if (cb->presentedDrawable != nil) {
			static uint32_t sLogCommitCount = 0;
			if (++sLogCommitCount <= 3) {
				op2_log(@"Presenting drawable %p at commit (#%u)", cb->presentedDrawable, sLogCommitCount);
			}
			if ([cb->presentedDrawable respondsToSelector:@selector(present)]) {
				[cb->presentedDrawable present];
			}
			/* Kick display panel scanout via kOP2Show */
			io_connect_t connect = 0;
			io_service_t service = IOServiceGetMatchingService(kIOMasterPortDefault, IOServiceMatching("IOAcceleratorES"));
			if (!service) {
				service = IOServiceGetMatchingService(kIOMasterPortDefault, IOServiceMatching("IOAccelerator"));
			}
			if (service) {
				if (IOServiceOpen(service, mach_task_self(), 0, &connect) == KERN_SUCCESS && connect) {
					IOConnectCallScalarMethod(connect, 0x7f /* kOP2Show */, NULL, 0, NULL, NULL);
					IOServiceClose(connect);
				}
				IOObjectRelease(service);
			}
		}
		if (gMtl && cb->work)
			gpuCommit();
		cb->status = MTLCommandBufferStatusCompleted;
		NSArray *handlers = cb->handlers;
		cb->handlers = nil;
		for (void (^h)(id) in handlers)
			h(self);
	}
}

static void
commandBufferEnqueue(id self, __unused SEL _cmd)
{
	void **slot = stateOf(self);
	struct op2_cmd_buf *cb = slot ? (struct op2_cmd_buf *)*slot : NULL;
	if (cb) {
		cb->status = MTLCommandBufferStatusEnqueued;
	}
}

static void
commandBufferWait(id self, __unused SEL _cmd)
{
	void **slot = stateOf(self);
	struct op2_cmd_buf *cb = slot ? (struct op2_cmd_buf *)*slot : NULL;
	if (cb) {
		cb->status = MTLCommandBufferStatusCompleted;
	}
}

/* G5b: a handler runs when the buffer is done (commit waits for the GPU), or at once if it already is */
static void
commandBufferAddHandler(id self, __unused SEL _cmd, void (^handler)(id))
{
	if (!handler)
		return;
	void **slot = stateOf(self);
	struct op2_cmd_buf *cb = slot ? (struct op2_cmd_buf *)*slot : NULL;
	if (!cb || cb->status == MTLCommandBufferStatusCompleted) {
		handler(self);
		return;
	}
	if (!cb->handlers)
		cb->handlers = [NSMutableArray array];
	[cb->handlers addObject:[handler copy]];
}

static void
commandBufferPresentDrawable(id self, __unused SEL _cmd, id drawable)
{
	static uint32_t sLogPresentCount = 0;
	if (++sLogPresentCount <= 3) {
		op2_log(@"presentDrawable: %p (#%u)", drawable, sLogPresentCount);
	}
	void **slot = stateOf(self);
	struct op2_cmd_buf *cb = slot ? (struct op2_cmd_buf *)*slot : NULL;
	if (cb) {
		cb->presentedDrawable = drawable;
	}
}

static void
commandBufferPresentDrawableAtTime(id self, SEL _cmd, id drawable, __unused double presentationTime)
{
	commandBufferPresentDrawable(self, _cmd, drawable);
}

static void
commandBufferPresentDrawableAfterDuration(id self, SEL _cmd, id drawable, __unused double duration)
{
	commandBufferPresentDrawable(self, _cmd, drawable);
}

static id
commandBufferBlitEncoder(__unused id self, __unused SEL _cmd)
{
	return [[gBlitEncoderClass alloc] init];
}

/* the render encoder (G5b) is with the pipelines, after the texture and function code it uses */

/* ---- Blit Command Encoder ---- */
static void
blitCopyBuffer(__unused id self, __unused SEL _cmd, id src, unsigned long srcOff, id dst, unsigned long dstOff, unsigned long size)
{
	void *s = bufferContents(src, NULL);
	void *d = bufferContents(dst, NULL);
	if (s && d && size) {
		memcpy((char *)d + dstOff, (const char *)s + srcOff, size);
	}
}

static void
blitFillBuffer(__unused id self, __unused SEL _cmd, id buf, NSRange range, uint8_t val)
{
	void *d = bufferContents(buf, NULL);
	if (d && range.length) {
		memset((char *)d + range.location, val, range.length);
	}
}

static void
blitEndEncoding(id self, SEL _cmd)
{
	struct objc_super sup = { self, class_getSuperclass(gBlitEncoderClass) };
	if (class_respondsToSelector(class_getSuperclass(gBlitEncoderClass), _cmd)) {
		((void (*)(struct objc_super *, SEL))objc_msgSendSuper)(&sup, _cmd);
	}
}

/* ---- Textures ---- */
struct op2_texture {
	id device;
	id descriptor;
	NSUInteger textureType;
	NSUInteger pixelFormat;
	NSUInteger width;
	NSUInteger height;
	NSUInteger usage;
	void *iosurface;
	NSUInteger plane;
	uint64_t uniqueID;
	BOOL isDrawable;
	NSUInteger allocatedSize;
	void *bytes;
	op2mtl_texture *gpu;     /* G5b: the GPU texture; NULL = the CPU copy in `bytes` (or nothing) */
	NSUInteger levels, storageMode;
	/* newTextureWithBytesNoCopy (CA's layer images): CA's own memory, copied to `gpu` when made and again the
	 * first time the texture is bound after each commit (CA may change the bytes; "no copy" means it can) */
	const void *src;
	NSUInteger srcLen, srcBpr;
	void (^srcDealloc)(void *, NSUInteger);
	uint64_t srcUploaded;    /* gFrameGen of the last copy */
};
static volatile uint64_t gFrameGen = 1;   /* bumped by every GPU commit */

/*
 * Frame capture (docs/FABLE-REVIEW.md C2, the userland half): what the GPU drew, without a camera. The last
 * IOSurface-backed colour target a render pass used is the screen; after a commit, at most once every 40 s and only
 * if something was drawn since, it is read back and written at half size to /private/var/tmp/op2cap-<n>.bgra
 * (n = 0..7, round robin; a 16-byte header "OP2C", width, height, commit number, then BGRA rows).
 * tools/op2cap-pull.sh reads them from TWRP and makes PNGs.
 */
static pthread_mutex_t gCapLock = PTHREAD_MUTEX_INITIALIZER;
static op2mtl_texture *gCapTex;
static unsigned gCapW, gCapH, gCapN, gCapCommit;
static uint64_t gCapLast;

static void
captureForget(op2mtl_texture *t)
{
	pthread_mutex_lock(&gCapLock);
	if (gCapTex == t)
		gCapTex = NULL;
	pthread_mutex_unlock(&gCapLock);
}

static void
captureNote(op2mtl_texture *t, unsigned w, unsigned h)
{
	pthread_mutex_lock(&gCapLock);
	gCapTex = t;
	gCapW = w;
	gCapH = h;
	pthread_mutex_unlock(&gCapLock);
}

static void
textureFree(void *state)
{
	struct op2_texture *t = (struct op2_texture *)state;
	if (t) {
		if (t->gpu) {
			captureForget(t->gpu);
			M.texture_free(t->gpu);
		}
		if (t->iosurface) {
			CFRelease(t->iosurface);
		}
		if (t->srcDealloc) {
			t->srcDealloc((void *)t->src, t->srcLen);
		}
		t->srcDealloc = nil;
		if (t->bytes) {
			free(t->bytes);
		}
		t->device = nil;
		t->descriptor = nil;
		free(t);
	}
}

static void
textureDealloc(__unsafe_unretained id self, SEL _cmd)
{
	deallocWith(self, _cmd, gTextureClass, textureFree);
}

static struct op2_texture *
textureOf(id self)
{
	void **slot = stateOf(self);
	return slot ? (struct op2_texture *)*slot : NULL;
}

static id
textureDevice(id self, __unused SEL _cmd)
{
	struct op2_texture *t = textureOf(self);
	return t ? t->device : nil;
}

static NSUInteger
textureType(id self, __unused SEL _cmd)
{
	struct op2_texture *t = textureOf(self);
	return t ? t->textureType : 2 /* MTLTextureType2D */;
}

static NSUInteger
texturePixelFormat(id self, __unused SEL _cmd)
{
	struct op2_texture *t = textureOf(self);
	return t ? t->pixelFormat : 80 /* MTLPixelFormatBGRA8Unorm */;
}

static NSUInteger
textureWidth(id self, __unused SEL _cmd)
{
	struct op2_texture *t = textureOf(self);
	return t ? t->width : 1080;
}

static NSUInteger
textureHeight(id self, __unused SEL _cmd)
{
	struct op2_texture *t = textureOf(self);
	return t ? t->height : 1920;
}

static NSUInteger
textureDepth(__unused id self, __unused SEL _cmd)
{
	return 1;
}

static NSUInteger
textureMipmapLevelCount(id self, __unused SEL _cmd)
{
	struct op2_texture *t = textureOf(self);
	return t && t->levels ? t->levels : 1;
}

static NSUInteger
textureStorageMode(id self, __unused SEL _cmd)
{
	struct op2_texture *t = textureOf(self);
	return t ? t->storageMode : MTLStorageModeShared;
}

static size_t
bytesPerPixel(NSUInteger fmt)
{
	switch (fmt) {   /* MTLPixelFormat (FABLE-REVIEW A7: A8 and the wide formats used to be counted as 4) */
	case 1: case 10: case 11: case 12: case 13: case 14: return 1;   /* A8, R8 */
	case 20: case 22: case 23: case 24: case 25:                     /* R16 */
	case 30: case 31: case 32: case 33: case 34:                     /* RG8 */
	case 40: case 41: case 42: case 43: return 2;                    /* packed 16-bit */
	case 103: case 104: case 105: case 110: case 112: case 113: case 114: case 115: return 8;   /* RG32, RGBA16 */
	case 123: case 124: case 125: return 16;                         /* RGBA32 */
	default: return 4;
	}
}

/* a format's block: 1x1 and bytesPerPixel for plain formats; ASTC (MTLPixelFormat 186..200 sRGB, 204..218 LDR,
 * the iOS 12.4 SDK's values) is 16 bytes a block (round g5b-8: CA's wallpaper is ASTC 4x4 LDR) */
static void
formatBlock(NSUInteger fmt, unsigned *bw, unsigned *bh, unsigned *bytes)
{
	static const uint8_t astc[15][2] = { { 4, 4 }, { 5, 4 }, { 5, 5 }, { 6, 5 }, { 6, 6 }, { 0, 0 }, { 8, 5 }, { 8, 6 },
	                                     { 8, 8 }, { 10, 5 }, { 10, 6 }, { 10, 8 }, { 10, 10 }, { 12, 10 }, { 12, 12 } };
	NSUInteger k = fmt >= 204 && fmt <= 218 ? fmt - 204 : fmt >= 186 && fmt <= 200 ? fmt - 186 : 99;
	if (k < 15 && astc[k][0]) {
		*bw = astc[k][0];
		*bh = astc[k][1];
		*bytes = 16;
	} else {
		*bw = *bh = 1;
		*bytes = (unsigned)bytesPerPixel(fmt);
	}
}

/* replaceRegion:mipmapLevel:withBytes:bytesPerRow: */
static void
textureReplace(id self, __unused SEL _cmd, MTLRegion r, NSUInteger level, const void *bytes, NSUInteger bpr)
{
	struct op2_texture *t = textureOf(self);
	if (!t || !bytes || !r.size.width || !r.size.height)
		return;
	if (t->gpu) {
		/* round fable-20: status-bar layers drew as opaque black boxes: what alpha does CA hand us? The corner and
		 * centre pixel of the first uploads of 4-byte textures */
		static unsigned sTraced;
		if (sTraced < 24 && bytesPerPixel(t->pixelFormat) == 4 && level == 0 && r.size.width >= 8 &&
		    r.size.height >= 8) {
			sTraced++;
			const uint8_t *c0 = (const uint8_t *)bytes;
			const uint8_t *cm = (const uint8_t *)bytes + (r.size.height / 2) * bpr + (r.size.width / 2) * 4;
			op2_trace("R %lux%lu fmt %lu bpr %lu tex %lux%lu: corner %02x%02x%02x%02x centre %02x%02x%02x%02x",
			          (unsigned long)r.size.width, (unsigned long)r.size.height, (unsigned long)t->pixelFormat,
			          (unsigned long)bpr, (unsigned long)t->width, (unsigned long)t->height, c0[0], c0[1], c0[2],
			          c0[3], cm[0], cm[1], cm[2], cm[3]);
		}
		if (!M.texture_replace(t->gpu, (uint32_t)level, (uint32_t)r.origin.x, (uint32_t)r.origin.y,
		                       (uint32_t)r.size.width, (uint32_t)r.size.height, bytes, bpr))
			op2_log(@"G replaceRegion: %s", M.last_error());
		return;
	}
	if (!t->bytes || level || r.origin.x >= t->width)
		return;
	size_t px = bytesPerPixel(t->pixelFormat), row = t->width * px;
	size_t n = MIN(r.size.width, t->width - r.origin.x) * px;
	for (NSUInteger y = 0; y < r.size.height && r.origin.y + y < t->height; y++)
		memcpy((uint8_t *)t->bytes + (r.origin.y + y) * row + r.origin.x * px,
		       (const uint8_t *)bytes + y * bpr, n);
}

/* replaceRegion:mipmapLevel:slice:withBytes:bytesPerRow:bytesPerImage: (2D only: slice 0) */
static void
textureReplaceSlice(id self, SEL _cmd, MTLRegion r, NSUInteger level, NSUInteger slice, const void *bytes,
                    NSUInteger bpr, __unused NSUInteger bpi)
{
	if (slice == 0)
		textureReplace(self, _cmd, r, level, bytes, bpr);
}

/* getBytes:bytesPerRow:fromRegion:mipmapLevel: */
static void
textureGetBytes(id self, __unused SEL _cmd, void *bytes, NSUInteger bpr, MTLRegion r, NSUInteger level)
{
	struct op2_texture *t = textureOf(self);
	if (!t || !bytes)
		return;
	if (t->gpu) {
		if (!M.texture_read(t->gpu, (uint32_t)level, (uint32_t)r.origin.x, (uint32_t)r.origin.y,
		                    (uint32_t)r.size.width, (uint32_t)r.size.height, bytes, bpr))
			op2_log(@"G getBytes: %s", M.last_error());
		return;
	}
	if (!t->bytes || level || r.origin.x >= t->width)
		return;
	size_t px = bytesPerPixel(t->pixelFormat), row = t->width * px;
	size_t n = MIN(r.size.width, t->width - r.origin.x) * px;
	for (NSUInteger y = 0; y < r.size.height && r.origin.y + y < t->height; y++)
		memcpy((uint8_t *)bytes + y * bpr,
		       (const uint8_t *)t->bytes + (r.origin.y + y) * row + r.origin.x * px, n);
}

static NSUInteger
textureSampleCount(__unused id self, __unused SEL _cmd)
{
	return 1;
}

static NSUInteger
textureArrayLength(__unused id self, __unused SEL _cmd)
{
	return 1;
}

static NSUInteger
textureUsage(id self, __unused SEL _cmd)
{
	struct op2_texture *t = textureOf(self);
	return t ? t->usage : (0x01 | 0x04);
}

static void *
textureIOSurface(id self, __unused SEL _cmd)
{
	struct op2_texture *t = textureOf(self);
	return t ? t->iosurface : NULL;
}

static NSUInteger
textureIOSurfacePlane(id self, __unused SEL _cmd)
{
	struct op2_texture *t = textureOf(self);
	return t ? t->plane : 0;
}

static BOOL
textureIsDrawable(id self, __unused SEL _cmd)
{
	struct op2_texture *t = textureOf(self);
	return t ? t->isDrawable : NO;
}

static BOOL
textureIsCompressed(__unused id self, __unused SEL _cmd)
{
	return NO;
}

static NSInteger
textureCompressionFeedback(__unused id self, __unused SEL _cmd)
{
	return 0;
}

static uint64_t
textureUniqueID(id self, __unused SEL _cmd)
{
	struct op2_texture *t = textureOf(self);
	return t ? t->uniqueID : 1001ULL;
}

static NSUInteger
textureAllocatedSize(id self, __unused SEL _cmd)
{
	struct op2_texture *t = textureOf(self);
	return t ? t->allocatedSize : (1080 * 1920 * 4);
}

static uint64_t
textureGPUAddress(__unused id self, __unused SEL _cmd)
{
	return 0;
}

static unsigned int
textureSwizzleKey(__unused id self, __unused SEL _cmd)
{
	return 0;
}

static NSUInteger
textureNumFaces(__unused id self, __unused SEL _cmd)
{
	return 1;
}

static NSUInteger
textureRotation(__unused id self, __unused SEL _cmd)
{
	return 0;
}

OP2_RETURNS_RETAINED static id deviceNewTexture(id self, SEL _cmd, id descriptor);

OP2_RETURNS_RETAINED static id
deviceNewTextureIOSurface(id self, SEL _cmd, id descriptor, void *surface, NSUInteger plane)
{
	op2_log(@"newTextureWithDescriptor:iosurface:%p plane:%lu called", surface, (unsigned long)plane);
	if (!surface) {
		/* SpringBoard asks for 162x162 and 2592x2592 render targets this way (round g5b-2): with no surface
		 * it is an ordinary texture, so make one (on the GPU) instead of a stub that cannot be drawn into */
		op2_trace("G newTextureWithDescriptor:iosurface: NULL surface, %lux%lu fmt %lu usage %lx: a plain texture",
		          (unsigned long)[descriptor width], (unsigned long)[descriptor height],
		          (unsigned long)[descriptor pixelFormat], (unsigned long)[descriptor usage]);
		return deviceNewTexture(self, _cmd, descriptor);
	}
	if (gTextureClass == Nil) {
		return nil;
	}
	id tex = [[gTextureClass alloc] init];
	struct op2_texture *t = calloc(1, sizeof(*t));
	void **slot = stateOf(tex);
	if (!t || !slot) {
		free(t);
		return nil;
	}
	static uint64_t gNextTexID = 1000;
	t->device = self;
	t->descriptor = descriptor;
	t->textureType = [descriptor respondsToSelector:@selector(textureType)] ? [descriptor textureType] : 2;
	t->pixelFormat = [descriptor respondsToSelector:@selector(pixelFormat)] ? [descriptor pixelFormat] : 80;
	t->width = [descriptor respondsToSelector:@selector(width)] ? [descriptor width] : 1080;
	t->height = [descriptor respondsToSelector:@selector(height)] ? [descriptor height] : 1920;
	t->usage = [descriptor respondsToSelector:@selector(usage)] ? [descriptor usage] : (0x01 | 0x04);
	t->iosurface = surface ? (void *)CFRetain(surface) : NULL;   /* the texture keeps its surface alive */
	t->plane = plane;
	t->uniqueID = ++gNextTexID;
	t->isDrawable = YES;
	t->allocatedSize = t->width * t->height * 4;
	t->levels = 1;
	*slot = t;
	/* G5b: the kernel imports the surface's own pages (kOP2Surface), so GPU draws land in it directly */
	op2mtl_device *dev = mtl();
	if (dev && surface && plane == 0) {
		IOSurfaceRef s = (IOSurfaceRef)surface;
		busyBegin("texture_from_iosurface");
		t->gpu = M.texture_from_iosurface(dev, IOSurfaceGetID(s), (uint32_t)t->pixelFormat, (uint32_t)t->width,
		                                  (uint32_t)t->height, (uint32_t)IOSurfaceGetBytesPerRow(s));
		busyEnd();
		if (!t->gpu)
			op2_log(@"G iosurface texture not on the GPU: %s", M.last_error());
	}
	op2_log(@"Created IOSurface texture %p (%lux%lu, fmt %lu, id %llu)",
	        tex, (unsigned long)t->width, (unsigned long)t->height, (unsigned long)t->pixelFormat, (unsigned long long)t->uniqueID);
	return tex;
}

OP2_RETURNS_RETAINED static id
deviceNewTexture(id self, __unused SEL _cmd, id descriptor)
{
	if (gTextureClass == Nil) {
		return nil;
	}
	id tex = [[gTextureClass alloc] init];
	struct op2_texture *t = calloc(1, sizeof(*t));
	void **slot = stateOf(tex);
	if (!t || !slot) {
		free(t);
		return nil;
	}
	static uint64_t gNextTexID = 2000;
	t->device = self;
	t->descriptor = descriptor;
	t->textureType = [descriptor respondsToSelector:@selector(textureType)] ? [descriptor textureType] : 2;
	t->pixelFormat = [descriptor respondsToSelector:@selector(pixelFormat)] ? [descriptor pixelFormat] : 80;
	t->width = [descriptor respondsToSelector:@selector(width)] ? [descriptor width] : 1080;
	t->height = [descriptor respondsToSelector:@selector(height)] ? [descriptor height] : 1920;
	t->usage = [descriptor respondsToSelector:@selector(usage)] ? [descriptor usage] : (0x01 | 0x04);
	t->iosurface = NULL;
	t->plane = 0;
	t->uniqueID = ++gNextTexID;
	t->isDrawable = NO;
	t->allocatedSize = t->width * t->height * 4;
	t->levels = [descriptor respondsToSelector:@selector(mipmapLevelCount)] ? [descriptor mipmapLevelCount] : 1;
	t->storageMode = [descriptor respondsToSelector:@selector(storageMode)] ? [descriptor storageMode] : 0;
	*slot = t;
	/* G5b: 2D textures on the GPU; anything else keeps the CPU copy */
	op2mtl_device *dev = mtl();
	if (dev && t->textureType == 2 /* MTLTextureType2D */) {
		t->gpu = M.texture_new(dev, (uint32_t)t->pixelFormat, (uint32_t)t->width, (uint32_t)t->height,
		                       (uint32_t)t->levels, (uint32_t)t->usage);
		if (!t->gpu)
			op2_log(@"G texture not on the GPU: %s", M.last_error());
	} else if (dev) {
		op2_log(@"G texture type %lu not on the GPU", (unsigned long)t->textureType);
	}
	if (!t->gpu)
		t->bytes = calloc(1, t->allocatedSize);
	return tex;
}

/* copy a bytes-no-copy texture's memory to its GPU texture */
static void
textureUploadSource(struct op2_texture *t)
{
	if (!t->gpu || !t->src)
		return;
	if (!M.texture_replace(t->gpu, 0, 0, 0, (uint32_t)t->width, (uint32_t)t->height, t->src, t->srcBpr))
		op2_log(@"G bytes texture upload: %s", M.last_error());
	t->srcUploaded = gFrameGen;
}

/*
 * newTextureWithBytesNoCopy:length:descriptor:deallocator: (round g5b-7: backboardd's layer images come this
 * way; returning nil left everything textured black). The GPU cannot sample CA's memory in place, so this is a GPU
 * texture plus that memory, copied as textureUploadSource says. Rows: length / height (the selector has no
 * bytesPerRow), at least width * bytes per pixel.
 */
OP2_RETURNS_RETAINED static id
deviceNewTextureBytesNoCopy(id self, SEL _cmd, void *bytes, NSUInteger length, id descriptor,
                            void (^deallocator)(void *, NSUInteger))
{
	id tex = deviceNewTexture(self, _cmd, descriptor);
	struct op2_texture *t = tex ? textureOf(tex) : NULL;
	if (!t) {
		if (deallocator)
			deallocator(bytes, length);
		return tex;
	}
	unsigned bw, bh, bb;
	formatBlock(t->pixelFormat, &bw, &bh, &bb);
	NSUInteger rows = (t->height + bh - 1) / bh, minBpr = (t->width + bw - 1) / bw * bb;
	/* packed rows when the length is that plus less than a row of padding (round g5b-9: CA's ASTC wallpaper is
	 * 648 rows of 10368 bytes in 6721536, and length / rows = 10372 sheared every row); otherwise the length's
	 * own row size */
	NSUInteger bpr = rows ? length / rows : 0;
	if (bpr < minBpr || (minBpr * rows <= length && length - minBpr * rows < minBpr))
		bpr = minBpr;
	t->src = bytes;
	t->srcLen = length;
	t->srcBpr = bpr;
	t->srcDealloc = [deallocator copy];
	if (t->gpu && bytes && (uint64_t)bpr * rows <= length) {
		textureUploadSource(t);
	} else if (t->bytes && bytes) {
		memcpy(t->bytes, bytes, MIN(length, t->allocatedSize));   /* CPU stub: a snapshot */
		t->src = NULL;
	} else {
		t->src = NULL;
	}
	op2_trace("T bytes texture %lux%lu fmt %lu len %lu bpr %lu %s", (unsigned long)t->width,
	          (unsigned long)t->height, (unsigned long)t->pixelFormat, (unsigned long)length, (unsigned long)bpr,
	          t->gpu ? "gpu" : "cpu");
	return tex;
}

/* ---- Render Pipeline State (made with the encoder, G5b) ---- */
static NSUInteger
pipelineMaxTotalThreads(__unused id self, __unused SEL _cmd)
{
	return 1024;
}

static BOOL
pipelineThreadgroupMatchesTile(__unused id self, __unused SEL _cmd)
{
	return YES;
}

static NSUInteger
pipelineImageblockSampleLength(__unused id self, __unused SEL _cmd)
{
	return 0;
}

/* ---- Libraries & Functions ---- */
struct op2_library {
	NSData *data;
	struct op2_metallib info;
	struct op2_metallib_function *functions;
	NSArray *names;
	NSString *label;
	op2mtl_library *gpu;   /* G5b: made on first pipeline; gpuTried so a failure is not retried */
	BOOL gpuTried;
};

struct op2_function {
	id library;
	struct op2_metallib_function f;
	NSString *name;
	NSString *label;
	struct op2mtl_constant fc[OP2MTL_MAX_FC];   /* its MTLFunctionConstantValues, by index */
	unsigned nfc;
};

static void
libraryFree(void *state)
{
	struct op2_library *lib = (struct op2_library *)state;
	if (lib->gpu)
		M.library_free(lib->gpu);
	free(lib->functions);
	lib->data = nil;
	lib->names = nil;
	lib->label = nil;
	free(lib);
}

static void
libraryDealloc(__unsafe_unretained id self, SEL _cmd)
{
	deallocWith(self, _cmd, gLibraryClass, libraryFree);
}

static struct op2_library *
libraryOf(id self)
{
	void **slot = stateOf(self);
	return slot != NULL ? (struct op2_library *)*slot : NULL;
}

static id
libraryFunctionNames(id self, __unused SEL _cmd)
{
	struct op2_library *lib = libraryOf(self);
	return lib != NULL ? lib->names : [NSArray array];
}

OP2_RETURNS_RETAINED static id
libraryNewFunction(id self, __unused SEL _cmd, NSString *name)
{
	struct op2_library *lib = libraryOf(self);
	if (lib == NULL || gFunctionClass == Nil) {
		return nil;
	}
	const char *needle = [name UTF8String];
	size_t nlen = strlen(needle);

	for (uint32_t i = 0; i < lib->info.count; i++) {
		if (lib->functions[i].name_length == nlen &&
		    strncmp(lib->functions[i].name, needle, nlen) == 0) {
			id fn = [[gFunctionClass alloc] init];
			struct op2_function *f = calloc(1, sizeof(*f));
			void **slot = stateOf(fn);
			if (f && slot) {
				f->library = self;
				f->f = lib->functions[i];
				f->name = [name copy];
				*slot = f;
				return fn;
			}
			free(f);
			return nil;
		}
	}
	op2_log(@"G newFunction %@: not in library %@", name, lib->label);
	return nil;
}

/* one line (<= 127 chars) to the kernel log, as 16 scalars of 8 bytes */
static void
kernelLog(const char *line)
{
	static io_connect_t conn;
	if (!conn) {
		io_service_t s = IOServiceGetMatchingService(kIOMasterPortDefault, IOServiceMatching("OP2GPU"));
		if (!s) return;
		IOServiceOpen(s, mach_task_self(), 0, &conn);
		IOObjectRelease(s);
		if (!conn) return;
	}
	uint64_t w[16] = {0};
	size_t n = strlen(line);
	if (n > 127) n = 127;
	memcpy(w, line, n);
	IOConnectCallScalarMethod(conn, 0x7c, w, 16, NULL, NULL);
}

/*
 * Function constants (G5b): the setters of MTLFunctionConstantValues are hooked to keep each value on the
 * object (an associated NSMutableData of op2mtl_constant), and a function made with them copies them.
 * Values set by name cannot be matched to an index here: they are logged.
 */
static char kFCKey;
static IMP gFCSetIndex, gFCSetRange, gFCSetName;
static struct op2_function *functionOf(id self);

static unsigned
fcSize(NSUInteger t)
{
	if (t >= 3 && t <= 6) return 4 * (unsigned)(t - 2);           /* float..float4 */
	if (t >= 16 && t <= 19) return 2 * (unsigned)(t - 15);        /* half..half4 */
	if (t >= 29 && t <= 36) return 4 * (unsigned)((t - 29) % 4 + 1);   /* int/uint vectors */
	if (t >= 37 && t <= 44) return 2 * (unsigned)((t - 37) % 4 + 1);   /* short/ushort */
	if (t >= 45 && t <= 56) return (unsigned)((t - 45) % 4 + 1);       /* char/uchar/bool */
	return 4;
}

static void
fcStore(id obj, const void *v, NSUInteger type, NSUInteger index)
{
	if (!obj || !v)
		return;
	NSMutableData *d = objc_getAssociatedObject(obj, &kFCKey);
	if (!d) {
		d = [NSMutableData data];
		objc_setAssociatedObject(obj, &kFCKey, d, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
	}
	struct op2mtl_constant c;
	memset(&c, 0, sizeof c);
	c.index = (uint32_t)index;
	unsigned sz = fcSize(type);
	if (sz > sizeof c.bits) {
		op2_log(@"G function constant %lu: type %lu is %u bytes, only 8 kept", (unsigned long)index,
		        (unsigned long)type, sz);
		sz = sizeof c.bits;
	}
	memcpy(&c.bits, v, sz);
	struct op2mtl_constant *a = d.mutableBytes;
	NSUInteger n = d.length / sizeof c;
	for (NSUInteger i = 0; i < n; i++)
		if (a[i].index == c.index) {
			a[i] = c;
			return;
		}
	[d appendBytes:&c length:sizeof c];
}

static void
fcSetIndex(id self, SEL cmd, const void *v, NSUInteger type, NSUInteger index)
{
	fcStore(self, v, type, index);
	((void (*)(id, SEL, const void *, NSUInteger, NSUInteger))gFCSetIndex)(self, cmd, v, type, index);
}

static void
fcSetRange(id self, SEL cmd, const void *v, NSUInteger type, NSRange r)
{
	unsigned sz = fcSize(type);
	for (NSUInteger i = 0; v && i < r.length; i++)
		fcStore(self, (const uint8_t *)v + i * sz, type, r.location + i);
	((void (*)(id, SEL, const void *, NSUInteger, NSRange))gFCSetRange)(self, cmd, v, type, r);
}

static void
fcSetName(id self, SEL cmd, const void *v, NSUInteger type, NSString *name)
{
	op2_log(@"G function constant set by name %@: not passed to the GPU", name);
	((void (*)(id, SEL, const void *, NSUInteger, NSString *))gFCSetName)(self, cmd, v, type, name);
}

static void
installFCHooks(void)
{
	Class c = objc_getClass("MTLFunctionConstantValues");
	if (!c)
		return;
	/* +alloc may hand out a private subclass that overrides the setters: hook the class of a real one */
	id probe = [[c alloc] init];
	Class k = probe ? [probe class] : c;
	gFCSetIndex = class_replaceMethod(k, sel_registerName("setConstantValue:type:atIndex:"), (IMP)fcSetIndex, "v@:^vQQ");
	gFCSetRange = class_replaceMethod(k, sel_registerName("setConstantValues:type:withRange:"), (IMP)fcSetRange,
	                                  "v@:^vQ{_NSRange=QQ}");
	gFCSetName = class_replaceMethod(k, sel_registerName("setConstantValue:type:withName:"), (IMP)fcSetName, "v@:^vQ@");
	if (!gFCSetIndex || !gFCSetRange || !gFCSetName)
		op2_log(@"G function constant hooks: index %d range %d name %d", gFCSetIndex != NULL,
		        gFCSetRange != NULL, gFCSetName != NULL);
}

OP2_RETURNS_RETAINED static id
libraryNewFunctionConstants(id self, SEL _cmd, NSString *name, id constants, __unused NSError **error)
{
	id fn = libraryNewFunction(self, _cmd, name);
	struct op2_function *f = fn ? functionOf(fn) : NULL;
	NSData *d = constants ? objc_getAssociatedObject(constants, &kFCKey) : nil;
	if (f && d) {
		NSUInteger n = MIN(d.length / sizeof(struct op2mtl_constant), (NSUInteger)OP2MTL_MAX_FC);
		memcpy(f->fc, d.bytes, n * sizeof(struct op2mtl_constant));
		f->nfc = (unsigned)n;
	}
	op2_log(@"newFunction %@ constants %s (%u kept)", name, constants ? "yes" : "no", f ? f->nfc : 0);
	return fn;
}

#ifndef MTLLibraryTypeExecutable
#define MTLLibraryTypeExecutable 0
#endif

static unsigned long
libraryType(__unused id self, __unused SEL _cmd)
{
	return MTLLibraryTypeExecutable;
}

static void
functionFree(void *state)
{
	struct op2_function *f = (struct op2_function *)state;
	f->library = nil;
	f->name = nil;
	f->label = nil;
	free(f);
}

static void
functionDealloc(__unsafe_unretained id self, SEL _cmd)
{
	deallocWith(self, _cmd, gFunctionClass, functionFree);
}

static struct op2_function *
functionOf(id self)
{
	void **slot = stateOf(self);
	return slot != NULL ? (struct op2_function *)*slot : NULL;
}

static id
functionName(id self, __unused SEL _cmd)
{
	struct op2_function *f = functionOf(self);
	return f != NULL ? f->name : nil;
}

static unsigned long
functionType(id self, __unused SEL _cmd)
{
	struct op2_function *f = functionOf(self);
	return f != NULL ? f->f.type : 0;
}

OP2_RETURNS_RETAINED static id
libraryMake(NSData *data, NSString *label, NSError **error)
{
	const char *why = NULL;
	const uint8_t *thin = NULL;
	size_t thin_size = 0;
	struct op2_metallib info;
	struct op2_metallib_function *funcs = NULL;

	if (op2_metallib_thin([data bytes], [data length], &thin, &thin_size, &why) != 0) {
		if (error) {
			*error = [NSError errorWithDomain:@"OP2MetalLibraryDomain" code:1 userInfo:@{NSLocalizedDescriptionKey: [NSString stringWithUTF8String:why ? why : "thin failed"]}];
		}
		return nil;
	}

	if (op2_metallib_parse(thin, thin_size, &info, NULL, 0, &why) != 0) {
		if (error) {
			*error = [NSError errorWithDomain:@"OP2MetalLibraryDomain" code:2 userInfo:@{NSLocalizedDescriptionKey: [NSString stringWithUTF8String:why ? why : "parse failed"]}];
		}
		return nil;
	}

	funcs = calloc(info.count ? info.count : 1, sizeof(*funcs));
	if (funcs == NULL || op2_metallib_parse(thin, thin_size, &info, funcs, info.count, &why) != 0) {
		free(funcs);
		return nil;
	}

	NSMutableArray *names = [NSMutableArray arrayWithCapacity:info.count];
	for (uint32_t i = 0; i < info.count; i++) {
		NSString *n = [[NSString alloc] initWithBytes:funcs[i].name length:funcs[i].name_length encoding:NSUTF8StringEncoding];
		if (n) {
			[names addObject:n];
		}
	}

	id libObj = [[gLibraryClass alloc] init];
	struct op2_library *lib = calloc(1, sizeof(*lib));
	void **slot = stateOf(libObj);
	if (lib && slot) {
		lib->data = data;
		lib->info = info;
		lib->functions = funcs;
		lib->names = [names copy];
		lib->label = [label copy];
		*slot = lib;
		return libObj;
	}
	free(funcs);
	free(lib);
	return nil;
}

OP2_RETURNS_RETAINED static id
deviceNewLibraryURL(id self, __unused SEL _cmd, NSURL *url, NSError **error)
{
	gDeviceInstance = self;
	NSData *data = [NSData dataWithContentsOfURL:url options:0 error:error];
	if (data == nil) {
		return nil;
	}
	return libraryMake(data, [url path], error);
}

OP2_RETURNS_RETAINED static id
deviceNewLibraryFile(id self, SEL _cmd, NSString *path, NSError **error)
{
	return deviceNewLibraryURL(self, _cmd, [NSURL fileURLWithPath:path], error);
}

OP2_RETURNS_RETAINED static id
deviceNewLibraryData(id self, __unused SEL _cmd, NSData *data, NSError **error)
{
	gDeviceInstance = self;
	return libraryMake(data, @"data", error);
}

OP2_RETURNS_RETAINED static id
deviceNewDefaultLibraryBundle(id self, SEL _cmd, NSBundle *bundle, NSError **error)
{
	NSURL *url = [bundle URLForResource:@"default" withExtension:@"metallib"];
	if (url == nil) {
		if (error) {
			*error = [NSError errorWithDomain:@"OP2MetalLibraryDomain" code:404 userInfo:@{NSLocalizedDescriptionKey: @"default.metallib not found"}];
		}
		return nil;
	}
	return deviceNewLibraryURL(self, _cmd, url, error);
}

OP2_RETURNS_RETAINED static id
deviceNewDefaultLibrary(id self, SEL _cmd)
{
	return deviceNewDefaultLibraryBundle(self, _cmd, [NSBundle mainBundle], NULL);
}

/* ================================ G5b: samplers, pipelines, render passes ================================ */

static Class gSamplerClass;

/* ---- Sampler states ---- */
static void
samplerFree(void *state)
{
	if (state)
		M.sampler_free((op2mtl_sampler *)state);
}

static void
samplerDealloc(__unsafe_unretained id self, SEL _cmd)
{
	deallocWith(self, _cmd, gSamplerClass, samplerFree);
}

static op2mtl_sampler *
samplerOf(id s)
{
	void **slot = s ? stateOf(s) : NULL;
	return slot ? (op2mtl_sampler *)*slot : NULL;
}

OP2_RETURNS_RETAINED static id
deviceNewSampler(__unused id self, __unused SEL _cmd, MTLSamplerDescriptor *d)
{
	if (gSamplerClass == Nil)
		return nil;
	id s = [[gSamplerClass alloc] init];
	void **slot = stateOf(s);
	op2mtl_device *dev = mtl();
	if (dev && d && slot) {
		struct op2mtl_sampler_desc sd = {
			.min_filter = (uint32_t)d.minFilter, .mag_filter = (uint32_t)d.magFilter,
			.mip_filter = (uint32_t)d.mipFilter, .s = (uint32_t)d.sAddressMode,
			.t = (uint32_t)d.tAddressMode, .r = (uint32_t)d.rAddressMode,
			.normalized = d.normalizedCoordinates, .lod_min = d.lodMinClamp, .lod_max = d.lodMaxClamp,
		};
		*slot = M.sampler_new(dev, &sd);
		op2_trace("Smp min %u mag %u mip %u wrap %u/%u/%u norm %d", sd.min_filter, sd.mag_filter, sd.mip_filter,
		          sd.s, sd.t, sd.r, sd.normalized);
	}
	return s;
}

/* ---- Render pipeline states ---- */
struct op2_pipeline {
	op2mtl_pipeline *gpu;   /* NULL: did not compile (logged); draws with it are skipped */
	char v[64], f[64];      /* function names, for the log */
	unsigned serial;
};

static void
pipelineFree(void *state)
{
	struct op2_pipeline *p = (struct op2_pipeline *)state;
	if (p->gpu)
		M.pipeline_free(p->gpu);
	free(p);
}

static void
pipelineDealloc(__unsafe_unretained id self, SEL _cmd)
{
	deallocWith(self, _cmd, gPipelineClass, pipelineFree);
}

static struct op2_pipeline *
pipelineOf(id p)
{
	void **slot = p ? stateOf(p) : NULL;
	return slot ? (struct op2_pipeline *)*slot : NULL;
}

/* the library on the GPU side, made once (a failure is logged once and not retried) */
static op2mtl_library *
libraryGPU(struct op2_library *lib)
{
	static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
	pthread_mutex_lock(&lock);
	if (!lib->gpuTried) {
		lib->gpuTried = YES;
		lib->gpu = M.library_new(gMtl, lib->data.bytes, lib->data.length);
		if (!lib->gpu)
			op2_log(@"G library %@ not on the GPU: %s", lib->label, M.last_error());
	}
	pthread_mutex_unlock(&lock);
	return lib->gpu;
}

static BOOL
fillFunction(struct op2mtl_function *out, id fn, char *name, size_t cap)
{
	struct op2_function *f = fn ? functionOf(fn) : NULL;
	struct op2_library *lib = f ? libraryOf(f->library) : NULL;
	op2mtl_library *gl = lib ? libraryGPU(lib) : NULL;
	if (!gl)
		return NO;
	strlcpy(name, [f->name UTF8String], cap);
	out->library = gl;
	out->name = name;
	out->num_constants = f->nfc;
	memcpy(out->constants, f->fc, f->nfc * sizeof f->fc[0]);
	return YES;
}

/* the constants as "i=value" pairs, the ones CA's uber shaders are specialised by */
static void
constantsText(const struct op2mtl_function *fn, char *out, size_t cap)
{
	size_t k = 0;
	out[0] = 0;
	for (unsigned i = 0; i < fn->num_constants && k + 1 < cap; i++)
		k += (size_t)snprintf(out + k, cap - k, "%s%u=%llx", i ? "," : "", fn->constants[i].index,
		                      (unsigned long long)fn->constants[i].bits);
}

OP2_RETURNS_RETAINED static id
deviceNewRenderPipelineState(__unused id self, __unused SEL _cmd, MTLRenderPipelineDescriptor *pd,
                             __unused NSError **error)
{
	if (gPipelineClass == Nil)
		return nil;
	id pipeline = [[gPipelineClass alloc] init];
	struct op2_pipeline *p = calloc(1, sizeof *p);
	void **slot = stateOf(pipeline);
	if (!p || !slot) {
		free(p);
		return pipeline;
	}
	*slot = p;
	static unsigned serial;
	p->serial = ++serial;
	op2mtl_device *dev = mtl();
	if (!dev || !pd)
		return pipeline;

	struct op2mtl_pipeline_desc *d = calloc(1, sizeof *d);
	if (!d)
		return pipeline;
	if (!fillFunction(&d->vertex, pd.vertexFunction, p->v, sizeof p->v) ||
	    !fillFunction(&d->fragment, pd.fragmentFunction, p->f, sizeof p->f)) {
		op2_log(@"G pipeline #%u: vertex %@ / fragment %@ not on the GPU", p->serial, pd.vertexFunction.name,
		        pd.fragmentFunction.name);
		free(d);
		return pipeline;
	}
	MTLVertexDescriptor *vd = pd.vertexDescriptor;
	char attrs[100] = "";
	size_t ak = 0;
	for (unsigned i = 0; vd && i < OP2MTL_MAX_ATTRIBS; i++) {
		MTLVertexAttributeDescriptor *a = vd.attributes[i];
		d->attributes[i] = (struct op2mtl_vertex_attribute){ (uint32_t)a.format, (uint32_t)a.offset,
		                                                     (uint32_t)a.bufferIndex };
		if (a.format && ak + 1 < sizeof attrs)
			ak += (size_t)snprintf(attrs + ak, sizeof attrs - ak, " %u:%lu@%lu/%lu", i, (unsigned long)a.format,
			                       (unsigned long)a.offset, (unsigned long)a.bufferIndex);
	}
	for (unsigned i = 0; vd && i < OP2MTL_MAX_BUFFERS; i++) {
		MTLVertexBufferLayoutDescriptor *l = vd.layouts[i];
		d->layouts[i] = (struct op2mtl_vertex_layout){ (uint32_t)l.stride, (uint32_t)l.stepFunction,
		                                               (uint32_t)l.stepRate };
	}
	if (vd && ak + 1 < sizeof attrs)   /* the stride of buffer 0, where CA's vertices are */
		ak += (size_t)snprintf(attrs + ak, sizeof attrs - ak, " stride %u", d->layouts[0].stride);
	for (unsigned i = 0; i < OP2MTL_MAX_COLOR; i++) {
		MTLRenderPipelineColorAttachmentDescriptor *c = pd.colorAttachments[i];
		d->color[i] = (struct op2mtl_color_attachment){
			.pixel_format = (uint32_t)c.pixelFormat, .blending = c.blendingEnabled,
			.rgb_op = (uint32_t)c.rgbBlendOperation, .alpha_op = (uint32_t)c.alphaBlendOperation,
			.src_rgb = (uint32_t)c.sourceRGBBlendFactor, .dst_rgb = (uint32_t)c.destinationRGBBlendFactor,
			.src_alpha = (uint32_t)c.sourceAlphaBlendFactor, .dst_alpha = (uint32_t)c.destinationAlphaBlendFactor,
			.write_mask = (uint32_t)c.writeMask,
		};
	}
	d->sample_count = pd.sampleCount ? (uint32_t)pd.sampleCount : 1;

	uint64_t t0 = mach_absolute_time();
	busyBegin("pipeline_new");
	p->gpu = M.pipeline_new(dev, d);
	busyEnd();
	gLastPipelineAt = mach_absolute_time();
	double ms = msSince(t0);

	/* what CA asked for, so a wrong colour can be traced to its pipeline */
	char fc[100];
	constantsText(&d->fragment, fc, sizeof fc);
	const struct op2mtl_color_attachment *c0 = &d->color[0];
	/* one line a pipeline (the functions only when they are not CA's uber pair), the attributes when they change */
	BOOL uber = !strcmp(p->v, "ca_uber_vertex_lp0") && !strcmp(p->f, "ca_uber_fragment_lp0_cp0_fo0");
	if (!uber)
		op2_trace("P%u %s + %s", p->serial, p->v, p->f);
	op2_trace("P%u %s %.0fms fmt %u bl %u %u/%u %u,%u,%u,%u wm %x fc %s", p->serial, p->gpu ? "ok" : "FAILED", ms,
	          c0->pixel_format, c0->blending, c0->rgb_op, c0->alpha_op, c0->src_rgb, c0->dst_rgb, c0->src_alpha,
	          c0->dst_alpha, c0->write_mask, fc);
	static char lastAttrs[100];
	if (ak && strcmp(attrs, lastAttrs)) {
		strlcpy(lastAttrs, attrs, sizeof lastAttrs);
		op2_trace("P%u attrs%s", p->serial, attrs);
	}
	if (!p->gpu)
		op2_trace("P%u error: %.100s", p->serial, M.last_error());
	free(d);
	return pipeline;
}

/* ---- Render command encoders ---- */
struct op2_encoder {
	BOOL gpu;                 /* a pass is open on the GPU; every call below is a no-op otherwise */
	BOOL drawable;            /* the bound pipeline compiled */
	__unsafe_unretained id pipeline;
	__unsafe_unretained id buf[2][OP2MTL_MAX_BUFFERS];   /* stage 0 vertex, 1 fragment: for offsets + trace */
	NSUInteger off[2][OP2MTL_MAX_BUFFERS];
};

/* one whole frame logged, draw by draw (round fable-22: icons and dock missing under metal=2): the first commit
 * that starts 60 s after the first GPU commit; at most 160 draws */
static volatile int gFrameTrace;   /* 0 not yet, 1 tracing this commit, 2 done */
static unsigned gFrameTraceN;
static __unsafe_unretained id gTraceTex[2][4];

/* per process, under gPassLock */
static struct {
	unsigned commits, draws, failed, skipped, reports, traced;
	double waitMs, maxMs;
} gStat;

static void
encoderClose(struct op2_encoder *e)
{
	if (e && e->gpu) {
		M.pass_end(gMtl);
		busyEnd();
		e->gpu = NO;
		pthread_mutex_unlock(&gPassLock);
	}
}

static void
encoderFree(void *state)
{
	encoderClose((struct op2_encoder *)state);   /* an encoder dropped without endEncoding must not hold the lock */
	free(state);
}

static void
encoderDealloc(__unsafe_unretained id self, SEL _cmd)
{
	deallocWith(self, _cmd, gRenderEncoderClass, encoderFree);
}

static struct op2_encoder *
encOf(id self)
{
	void **slot = stateOf(self);
	struct op2_encoder *e = slot ? (struct op2_encoder *)*slot : NULL;
	return e && e->gpu ? e : NULL;
}

static id
commandBufferRenderEncoder(id self, __unused SEL _cmd, MTLRenderPassDescriptor *rpd)
{
	if (gRenderEncoderClass == Nil)
		return nil;
	id enc = [[gRenderEncoderClass alloc] init];
	struct op2_encoder *e = calloc(1, sizeof *e);
	void **slot = stateOf(enc);
	if (!e || !slot) {
		free(e);
		return enc;
	}
	*slot = e;
	if (!gMtl || !rpd)
		return enc;

	struct op2mtl_pass_attachment att[OP2MTL_MAX_COLOR];
	memset(att, 0, sizeof att);
	BOOL any = NO;
	for (unsigned i = 0; i < OP2MTL_MAX_COLOR; i++) {
		MTLRenderPassColorAttachmentDescriptor *a = rpd.colorAttachments[i];
		id tex = a.texture;
		if (!tex)
			continue;
		struct op2_texture *t = textureOf(tex);
		if (!t || !t->gpu) {
			op2_log(@"G pass skipped: colour %u (fmt %lu) is not a GPU texture", i, t ? (unsigned long)t->pixelFormat : 0UL);
			return enc;
		}
		MTLClearColor cc = a.clearColor;
		att[i] = (struct op2mtl_pass_attachment){ .texture = t->gpu, .load_action = (uint32_t)a.loadAction,
		                                          .clear = { cc.red, cc.green, cc.blue, cc.alpha } };
		if (i == 0 && t->iosurface && t->width >= 640)
			captureNote(t->gpu, (unsigned)t->width, (unsigned)t->height);
		any = YES;
	}
	if (!any)
		return enc;
	pthread_mutex_lock(&gPassLock);
	busyBegin("a render pass");   /* until endEncoding */
	if (!M.pass_begin(gMtl, att)) {
		busyEnd();
		op2_log(@"G pass: %s", M.last_error());
		pthread_mutex_unlock(&gPassLock);
		return enc;
	}
	M.set_pipeline(gMtl, NULL);   /* nothing carries over from another pass */
	if (gFrameTrace == 1 && gFrameTraceN < 160) {
		struct op2_texture *t0 = rpd.colorAttachments[0].texture ? textureOf(rpd.colorAttachments[0].texture) : NULL;
		op2_trace("F pass -> %lux%lu fmt %lu load %lu%s", t0 ? (unsigned long)t0->width : 0UL,
		          t0 ? (unsigned long)t0->height : 0UL, t0 ? (unsigned long)t0->pixelFormat : 0UL,
		          (unsigned long)rpd.colorAttachments[0].loadAction, t0 && t0->iosurface ? " iosurface" : "");
	}
	e->gpu = YES;
	void **cslot = stateOf(self);
	struct op2_cmd_buf *cb = cslot ? (struct op2_cmd_buf *)*cslot : NULL;
	if (cb)
		cb->work = YES;
	return enc;
}

static void
renderEndEncoding(id self, SEL _cmd)
{
	void **slot = stateOf(self);
	encoderClose(slot ? (struct op2_encoder *)*slot : NULL);
	struct objc_super sup = { self, class_getSuperclass(gRenderEncoderClass) };
	if (class_respondsToSelector(class_getSuperclass(gRenderEncoderClass), _cmd))
		((void (*)(struct objc_super *, SEL))objc_msgSendSuper)(&sup, _cmd);
}

static void
encSetPipeline(id self, __unused SEL _cmd, id pipeline)
{
	struct op2_encoder *e = encOf(self);
	if (!e)
		return;
	struct op2_pipeline *p = pipelineOf(pipeline);
	e->pipeline = pipeline;
	e->drawable = p && p->gpu;
	M.set_pipeline(gMtl, p ? p->gpu : NULL);
}

static void
encSetViewport(id self, __unused SEL _cmd, MTLViewport v)
{
	if (encOf(self))
		M.set_viewport(gMtl, v.originX, v.originY, v.width, v.height, v.znear, v.zfar);
}

static void
encSetScissor(id self, __unused SEL _cmd, MTLScissorRect r)
{
	if (encOf(self))
		M.set_scissor(gMtl, (uint32_t)r.x, (uint32_t)r.y, (uint32_t)r.width, (uint32_t)r.height);
}

/* a buffer that is not a GPU buffer (newBufferWithBytesNoCopy) goes in as a copy of its bytes from offset */
static void
encBind(struct op2_encoder *e, unsigned st, id buffer, NSUInteger offset, NSUInteger index)
{
	if (index >= OP2MTL_MAX_BUFFERS)
		return;
	e->buf[st][index] = buffer;
	e->off[st][index] = offset;
	struct op2_buffer *b = buffer ? bufferOf(buffer) : NULL;
	if (!b) {
		M.set_buffer(gMtl, st, (unsigned)index, NULL, 0);
	} else if (b->gpu) {
		M.set_buffer(gMtl, st, (unsigned)index, b->gpu, offset);
	} else if (offset < b->length) {
		op2_log(@"G %s buffer %lu is CPU memory: copied at bind", st ? "fragment" : "vertex", (unsigned long)index);
		M.set_bytes(gMtl, st, (unsigned)index, (const uint8_t *)b->bytes + offset, b->length - offset);
	}
}

static void
encSetVertexBuffer(id self, __unused SEL _cmd, id buffer, NSUInteger offset, NSUInteger index)
{
	struct op2_encoder *e = encOf(self);
	if (e)
		encBind(e, 0, buffer, offset, index);
}

static void
encSetFragmentBuffer(id self, __unused SEL _cmd, id buffer, NSUInteger offset, NSUInteger index)
{
	struct op2_encoder *e = encOf(self);
	if (e)
		encBind(e, 1, buffer, offset, index);
}

static void
encSetOffset(struct op2_encoder *e, unsigned st, NSUInteger offset, NSUInteger index)
{
	if (index >= OP2MTL_MAX_BUFFERS)
		return;
	struct op2_buffer *b = e->buf[st][index] ? bufferOf(e->buf[st][index]) : NULL;
	if (b && b->gpu) {
		e->off[st][index] = offset;
		M.set_buffer_offset(gMtl, st, (unsigned)index, offset);
	} else if (b) {
		encBind(e, st, e->buf[st][index], offset, index);
	}
}

static void
encSetVertexBufferOffset(id self, __unused SEL _cmd, NSUInteger offset, NSUInteger index)
{
	struct op2_encoder *e = encOf(self);
	if (e)
		encSetOffset(e, 0, offset, index);
}

static void
encSetFragmentBufferOffset(id self, __unused SEL _cmd, NSUInteger offset, NSUInteger index)
{
	struct op2_encoder *e = encOf(self);
	if (e)
		encSetOffset(e, 1, offset, index);
}

static void
encSetBytes(id self, unsigned st, const void *bytes, NSUInteger length, NSUInteger index)
{
	struct op2_encoder *e = encOf(self);
	if (!e || index >= OP2MTL_MAX_BUFFERS)
		return;
	e->buf[st][index] = nil;
	M.set_bytes(gMtl, st, (unsigned)index, bytes, length);
	if (gStat.traced < 4) {
		char hx[72];
		op2_trace("B %c%lu bytes %lu: %s", st ? 'f' : 'v', (unsigned long)index, (unsigned long)length,
		          hexOf(bytes, MIN(length, (NSUInteger)32), hx, sizeof hx));
	}
}

static void
encSetVertexBytes(id self, __unused SEL _cmd, const void *bytes, NSUInteger length, NSUInteger index)
{
	encSetBytes(self, 0, bytes, length, index);
}

static void
encSetFragmentBytes(id self, __unused SEL _cmd, const void *bytes, NSUInteger length, NSUInteger index)
{
	encSetBytes(self, 1, bytes, length, index);
}

static void
encSetTexture(id self, unsigned st, id texture, NSUInteger index)
{
	if (!encOf(self))
		return;
	struct op2_texture *t = texture ? textureOf(texture) : NULL;
	if (st < 2 && index < 4)
		gTraceTex[st][index] = texture;
	if (t && t->src && t->srcUploaded != gFrameGen)
		textureUploadSource(t);
	/* which textures draws sample (each distinct binding once): round g5b-11 has the ASTC wallpaper on the GPU
	 * but not on the screen */
	if (t) {
		struct op2_encoder *ee = encOf(self);
		struct op2_pipeline *pp = ee ? pipelineOf(ee->pipeline) : NULL;
		op2_trace("X %c%lu %lux%lu fmt %lu %s%s P%u", st ? 'f' : 'v', (unsigned long)index, (unsigned long)t->width,
		          (unsigned long)t->height, (unsigned long)t->pixelFormat, t->gpu ? "gpu" : "cpu",
		          t->src ? " bytes" : t->iosurface ? " iosurface" : "", pp ? pp->serial : 0);
	}
	if (t && !t->gpu)
		op2_log(@"G %s texture %lu (fmt %lu) is not on the GPU: unbound", st ? "fragment" : "vertex",
		        (unsigned long)index, (unsigned long)t->pixelFormat);
	M.set_texture(gMtl, st, (unsigned)index, t ? t->gpu : NULL);
}

static void
encSetVertexTexture(id self, __unused SEL _cmd, id texture, NSUInteger index)
{
	encSetTexture(self, 0, texture, index);
}

static void
encSetFragmentTexture(id self, __unused SEL _cmd, id texture, NSUInteger index)
{
	encSetTexture(self, 1, texture, index);
}

static void
encSetVertexSampler(id self, __unused SEL _cmd, id sampler, NSUInteger index)
{
	if (encOf(self))
		M.set_sampler(gMtl, 0, (unsigned)index, samplerOf(sampler));
}

static void
encSetFragmentSampler(id self, __unused SEL _cmd, id sampler, NSUInteger index)
{
	if (encOf(self))
		M.set_sampler(gMtl, 1, (unsigned)index, samplerOf(sampler));
}

/* the first draws of a process: which pipeline, which buffers, and the first bytes of each (CA's constants) */
static void
traceDraw(struct op2_encoder *e, const char *what)
{
	if (gStat.traced >= 4)
		return;
	unsigned n = ++gStat.traced;
	struct op2_pipeline *p = pipelineOf(e->pipeline);
	op2_trace("D%u %s P%u %s + %s", n, what, p ? p->serial : 0, p ? p->v : "?", p ? p->f : "?");
	for (unsigned st = 0; st < 2; st++)
		for (unsigned i = 0; i < 8; i++) {
			struct op2_buffer *b = e->buf[st][i] ? bufferOf(e->buf[st][i]) : NULL;
			if (!b)
				continue;
			NSUInteger off = e->off[st][i];
			char hx[72];
			op2_trace("D%u %c%u len %lu off %lu %s: %s", n, st ? 'f' : 'v', i, (unsigned long)b->length,
			          (unsigned long)off, b->gpu ? "gpu" : "cpu",
			          off < b->length ? hexOf((const uint8_t *)b->bytes + off, MIN(b->length - off, (NSUInteger)32),
			                                 hx, sizeof hx) : "-");
			/* the stage-in vertex: CA's colours (half4 at 40 and 48) are past the first 32 bytes */
			if (st == 0 && i == 0 && off + 56 <= b->length)
				op2_trace("D%u v0 +32: %s", n, hexOf((const uint8_t *)b->bytes + off + 32, 24, hx, sizeof hx));
		}
}

static void
frameTraceDraw(struct op2_encoder *e, const char *kind, NSUInteger count)
{
	if (gFrameTrace != 1 || gFrameTraceN >= 160)
		return;
	gFrameTraceN++;
	struct op2_pipeline *p = pipelineOf(e->pipeline);
	char tx[80] = "";
	size_t k = 0;
	for (unsigned i = 0; i < 4; i++) {
		struct op2_texture *t = gTraceTex[1][i] ? textureOf(gTraceTex[1][i]) : NULL;
		if (t && k + 1 < sizeof tx)
			k += (size_t)snprintf(tx + k, sizeof tx - k, " t%u=%lux%lu/%lu%s", i, (unsigned long)t->width,
			                      (unsigned long)t->height, (unsigned long)t->pixelFormat,
			                      t->gpu ? "" : "!cpu");
	}
	op2_trace("F%u %s P%u%s n%lu%s", gFrameTraceN, kind, p ? p->serial : 0, e->drawable ? "" : " SKIP",
	          (unsigned long)count, tx);
}

static void
drawDone(BOOL ok)
{
	gStat.draws++;
	if (!ok) {
		gStat.failed++;
		op2_log(@"G draw failed: %s", M.last_error());
	}
}

static void
encDrawIndexed(id self, __unused SEL _cmd, NSUInteger prim, NSUInteger count, NSUInteger itype, id ibuf,
               NSUInteger ioff, NSUInteger instances)
{
	struct op2_encoder *e = encOf(self);
	if (!e)
		return;
	frameTraceDraw(e, "idx", count);
	if (!e->drawable) {
		gStat.skipped++;
		return;
	}
	struct op2_buffer *ib = ibuf ? bufferOf(ibuf) : NULL;
	if (!ib || !ib->gpu) {
		gStat.skipped++;
		op2_log(@"G indexed draw skipped: the index buffer is not a GPU buffer");
		return;
	}
	traceDraw(e, "indexed");
	drawDone(M.draw_indexed(gMtl, (uint32_t)prim, (uint32_t)count, (uint32_t)itype, ib->gpu, ioff,
	                        (uint32_t)(instances ? instances : 1)));
}

static void
encDrawIndexed1(id self, SEL _cmd, NSUInteger prim, NSUInteger count, NSUInteger itype, id ibuf, NSUInteger ioff)
{
	encDrawIndexed(self, _cmd, prim, count, itype, ibuf, ioff, 1);
}

static void
encDraw(id self, __unused SEL _cmd, NSUInteger prim, NSUInteger start, NSUInteger count, NSUInteger instances)
{
	struct op2_encoder *e = encOf(self);
	if (!e)
		return;
	frameTraceDraw(e, "arr", count);
	if (!e->drawable) {
		gStat.skipped++;
		return;
	}
	traceDraw(e, "arrays");
	drawDone(M.draw(gMtl, (uint32_t)prim, (uint32_t)start, (uint32_t)count, (uint32_t)(instances ? instances : 1)));
}

static void
encDraw1(id self, SEL _cmd, NSUInteger prim, NSUInteger start, NSUInteger count)
{
	encDraw(self, _cmd, prim, start, count, 1);
}

/* after a commit, under gPassLock: see the frame capture comment */
static void
captureMaybe(unsigned commit)
{
	static double sinceBoot;
	(void)sinceBoot;
	if (gCapLast && msSince(gCapLast) < 40000)
		return;
	pthread_mutex_lock(&gCapLock);
	op2mtl_texture *t = gCapTex;
	unsigned w = gCapW, h = gCapH;
	if (t && commit != gCapCommit) {
		uint32_t *full = malloc((size_t)w * h * 4);
		unsigned hw = w / 2, hh = h / 2;
		uint32_t *half = malloc((size_t)hw * hh * 4);
		if (full && half && M.texture_read(t, 0, 0, 0, w, h, full, (size_t)w * 4)) {
			for (unsigned y = 0; y < hh; y++)
				for (unsigned x = 0; x < hw; x++)
					half[y * hw + x] = full[(y * 2) * w + x * 2];
			char path[64];
			unsigned n = gCapN++ % 8;
			snprintf(path, sizeof path, "/private/var/tmp/op2cap-%u.bgra", n);
			FILE *f = fopen(path, "wb");
			if (f) {
				uint32_t hdr[4] = { 0x4332504f /* "OP2C" */, hw, hh, commit };
				fwrite(hdr, 1, sizeof hdr, f);
				fwrite(half, 4, (size_t)hw * hh, f);
				fflush(f);
				fsync(fileno(f));
				fclose(f);
				op2_trace("C capture %u: commit %u, %ux%u -> %s", n, commit, hw, hh, path);
			} else {
				op2_trace("C capture: cannot write %s (errno %d)", path, errno);
			}
		} else {
			op2_trace("C capture: read-back failed (%s)", full && half ? M.last_error() : "no memory");
		}
		free(full);
		free(half);
		gCapCommit = commit;
		gCapLast = mach_absolute_time();
	}
	pthread_mutex_unlock(&gCapLock);
}

/*
 * A command buffer with GPU work: flush, then wait for the GPU before the buffer reports completed (its
 * handlers run after this, and CA reuses the surfaces and buffers they release). Synchronous on purpose for
 * the first milestone; the wait time is what the stats line reports.
 */
static void
gpuCommit(void)
{
	pthread_mutex_lock(&gPassLock);
	uint64_t t0 = mach_absolute_time();
	busyBegin("commit + wait");
	uint64_t fence = M.commit(gMtl);
	BOOL done = M.wait(gMtl, fence, 2000000000ull);
	busyEnd();
	double ms = msSince(t0);
	gStat.commits++;
	gCommitsSeen = gStat.commits;
	gFrameGen++;
	gStat.waitMs += ms;
	if (ms > gStat.maxMs)
		gStat.maxMs = ms;
	if (!done)
		op2_log(@"G command buffer: the GPU did not finish in 2 s (fence %llu)", fence);
	if (gStat.commits == 1 || (gStat.commits % 300 == 0 && gStat.reports < 40)) {
		gStat.reports++;
		char line[128];
		snprintf(line, sizeof line, "M %s: S %u commits, %u draws (%u failed, %u skipped), GPU %.1f ms avg %.1f max",
		         getprogname(), gStat.commits, gStat.draws, gStat.failed, gStat.skipped,
		         gStat.waitMs / gStat.commits, gStat.maxMs);
		kernelLog(line);
		gStat.maxMs = 0;
	}
	/* from 45 s after the first GPU commit: earlier frames are the white boot screen (rounds fable-16..18), and an
	 * idle lock screen commits about once a minute, so a commit count never got there (round fable-19) */
	static uint64_t sFirstCommitAt;
	if (!sFirstCommitAt)
		sFirstCommitAt = mach_absolute_time();
	if (gFrameTrace == 1) {
		gFrameTrace = 2;
		op2_trace("F end: %u draws in commit %u, %u skipped so far", gFrameTraceN, gStat.commits, gStat.skipped);
	} else if (gFrameTrace == 0 && msSince(sFirstCommitAt) > 60000) {
		gFrameTrace = 1;   /* the NEXT command buffer's draws are logged */
	}
	if (done && msSince(sFirstCommitAt) > 45000)
		captureMaybe(gStat.commits);
	pthread_mutex_unlock(&gPassLock);
}

/* ---- Device Property Providers ---- */
static id
deviceName(__unused id self, __unused SEL _cmd)
{
	return @"OnePlus 2 Adreno 430 (OP2Metal)";
}

static unsigned long long
deviceRegistryID(__unused id self, __unused SEL _cmd)
{
	return 0x00018994ULL;
}

static unsigned long long
deviceMaxBufferLength(__unused id self, __unused SEL _cmd)
{
	return 256ULL * 1024 * 1024; /* 256 MB */
}

static MTLSize
deviceMaxThreadsPerThreadgroup(__unused id self, __unused SEL _cmd)
{
	return MTLSizeMake(1024, 1024, 1024);
}

static NSUInteger
deviceFeatureProfile(__unused id self, __unused SEL _cmd)
{
	/*
	 * Return 1: Claims Apple1, Apple2, and Common1.
	 * Required for MPSDevice::GetMPSDevice and MPSKernel during SpringBoard init!
	 */
	return 1;
}

static BOOL
deviceSupportsFamily(__unused id self, __unused SEL _cmd, NSUInteger family)
{
	/* Claim Apple1 (1001), Apple2 (1002), Common1 (3001), Common2 (3002) */
	if (family == 1001 || family == 1002 || family == 3001 || family == 3002) {
		return YES;
	}
	return NO;
}

static unsigned long long
deviceIOSurfaceAlignment(__unused id self, __unused SEL _cmd)
{
	return 64ULL;
}

static NSUInteger
deviceMaxTextureDimension2D(__unused id self, __unused SEL _cmd)
{
	return 8192;
}

static NSUInteger
deviceMaxTextureDimension3D(__unused id self, __unused SEL _cmd)
{
	return 2048;
}

static NSUInteger
deviceMaxColorAttachments(__unused id self, __unused SEL _cmd)
{
	return 8;
}

static BOOL
deviceYes(__unused id self, __unused SEL _cmd)
{
	return YES;
}

static BOOL
deviceNo(__unused id self, __unused SEL _cmd)
{
	return NO;
}

static unsigned long long
deviceNoDedicatedMemory(__unused id self, __unused SEL _cmd)
{
	return 0;
}

static unsigned long long
deviceSharedMemory(__unused id self, __unused SEL _cmd)
{
	return 4096ULL * 1024 * 1024; /* 4GB LPDDR4 unified memory */
}

static BOOL
deviceSupportsFeatureSet(__unused id self, __unused SEL _cmd, unsigned long featureSet)
{
	/* Claim support for iOS GPUFamily1_v1 (0) through GPUFamily3_v2 (5), and Common1_v1 (8) */
	return (featureSet <= 5 || featureSet == 8);
}

/* Descriptor Stubs */
static const struct stubSpec {
	const char *selector;
	const char *cls;
	const char *base;
	const char *protocol;
} gStubSpecs[] = {
	{ "newDepthStencilStateWithDescriptor:", "OP2MetalDepthStencilState", "_MTLDepthStencilState", "MTLDepthStencilState" },
	{ "newComputePipelineStateWithFunction:error:", "OP2MetalComputePipelineState", "_MTLComputePipelineState", "MTLComputePipelineState" },
	{ "newEventWithOptions:", "OP2MetalEvent", "_MTLEvent", "MTLEvent" },
	{ "newFence", "OP2MetalFence", "_MTLFence", "MTLFence" },
};
#define STUB_COUNT (sizeof(gStubSpecs) / sizeof(gStubSpecs[0]))
static Class gStubClass[STUB_COUNT];

OP2_RETURNS_RETAINED static id
stubFactory(__unused id self, SEL _cmd)
{
	const char *called = sel_getName(_cmd);
	for (unsigned i = 0; i < STUB_COUNT; i++) {
		if (strcmp(gStubSpecs[i].selector, called) == 0) {
			if (gStubClass[i] == Nil) {
				return nil;
			}
			return [[gStubClass[i] alloc] init];
		}
	}
	return nil;
}

/* Helper to declare protocol families */
static void
declareProtocolFamily(Class cls, const char *prefix)
{
	unsigned int count = 0;
	Protocol * __unsafe_unretained * all = objc_copyProtocolList(&count);
	size_t len = strlen(prefix);

	for (unsigned int i = 0; i < count; i++) {
		const char *pName = protocol_getName(all[i]);
		if (strncmp(pName, prefix, len) == 0) {
			class_addProtocol(cls, all[i]);
			if (gProtocolCount < (sizeof(gProtocols) / sizeof(gProtocols[0]))) {
				gProtocols[gProtocolCount++] = all[i];
			}
		}
	}
	free(all);
}

static Class
makeCustomClass(const char *name, const char *baseName, const char *protocolFamily, IMP deallocImp)
{
	Class super = baseName ? objc_getClass(baseName) : Nil;
	if (super == Nil) {
		super = objc_getClass("NSObject");
	}
	Class cls = objc_allocateClassPair(super, name, 0);
	if (cls == Nil) {
		return Nil;
	}
	class_addIvar(cls, "_op2State", sizeof(void *), sizeof(void *) == 8 ? 3 : 2, "^v");
	if (protocolFamily) {
		declareProtocolFamily(cls, protocolFamily);
	}
	if (deallocImp) {
		class_addMethod(cls, sel_registerName("dealloc"), deallocImp, "v@:");
	}
	class_addMethod(cls, sel_registerName("device"), (IMP)objectDevice, "@@:");
	class_addMethod(cls, sel_registerName("methodSignatureForSelector:"), (IMP)deviceMethodSignature, "@@::");
	class_addMethod(cls, sel_registerName("forwardInvocation:"), (IMP)deviceForwardInvocation, "v@:@");
	return cls;
}

/* ---- OP2MetalDevice Initialization ---- */
OP2_INIT_METHOD static id
deviceInit(__attribute__((ns_consumed)) id self, SEL _cmd, unsigned int port)
{
	gAcceleratorPort = port;
	gDeviceInstance = self;
	(void)_cmd;
	op2_log(@"Initialized OP2MetalDevice (%p) for IOAcceleratorES port %u", self, port);
	return self;
}

static void
deviceInitLimits(__unused id self, __unused SEL _cmd)
{
	op2_log(@"[OP2MetalDevice initLimits] called");
}

static void
deviceRegisterDevices(id self, __unused SEL _cmd)
{
	op2_log(@"+[OP2MetalDevice registerDevices] invoked by Metal.framework");
	if (gDeviceInstance == nil) {
		io_service_t service = IOServiceGetMatchingService(kIOMasterPortDefault, IOServiceMatching("IOAcceleratorES"));
		if (!service) {
			service = IOServiceGetMatchingService(kIOMasterPortDefault, IOServiceMatching("IOAccelerator"));
		}
		id raw = [((Class)self) alloc];
		gDeviceInstance = ((id (*)(id, SEL, unsigned int))objc_msgSend)(raw, sel_registerName("initWithAcceleratorPort:"), service);
		if (service) {
			IOObjectRelease(service);
		}
	}
	if (!gDeviceAdded && gDeviceInstance != nil) {
		void (*pMTLAddDevice)(id) = (void (*)(id))dlsym(RTLD_DEFAULT, "MTLAddDevice");
		if (pMTLAddDevice) {
			op2_log(@"Adding OP2MetalDevice %p to Metal device array via MTLAddDevice", gDeviceInstance);
			pMTLAddDevice(gDeviceInstance);
			gDeviceAdded = YES;
		}
	}
}

/* Process Gate: metal=1/2/3 */
static BOOL
shownToThisProcess(void)
{
	char args[1024] = "";
	size_t size = sizeof(args) - 1;
	const char *prog = getprogname();
	int level = 1;

	if (sysctlbyname("kern.bootargs", args, &size, NULL, 0) == 0) {
		const char *at = strstr(args, "metal=");
		if (at != NULL) {
			/*
			 * The cmdline has no room for more arguments, so metal= carries the A/B switches too: the level is the
			 * last digit, the digits before it are flags for op2mtl (the kernel only tests metal >= 2):
			 *   metal=2   compositor on the GPU          metal=12  the same without framebuffer fetch
			 *   metal=22  16-bit varyings left 16-bit    metal=32  both
			 *   metal=42  framebuffer fetch through a shadow copy (the fable-16/17 way) instead of in place
			 */
			int v = atoi(at + 6);
			level = v % 10;
			if ((v / 10) & 1)
				setenv("OP2MTL_NOFBFETCH", "1", 1);
			if ((v / 10) & 2)
				setenv("OP2MTL_F16VARY", "1", 1);
			if ((v / 10) & 4)   /* metal=42: framebuffer fetch through a shadow copy instead of in place */
				setenv("OP2MTL_FBCOPY", "1", 1);
		}
	}

	if (prog == NULL) {
		return NO;
	}

	/*
	 * Level policy:
	 *   metal=0: disabled
	 *   metal=1: metal_test only
	 *   metal=2: the compositor (backboardd) on the GPU, plus the test tools; SpringBoard and apps see no Metal device
	 *            and draw with the CPU as on a CPU boot (G5b round 12: with Metal in SpringBoard too, pages did not
	 *            snap and no app launched, while CPU boots launch Settings; this is the A/B, and the shape the other
	 *            ports run in)
	 *   metal=3: every process (SpringBoard and apps too)
	 */
	if (level <= 0) {
		return NO;
	}
	if (level == 1 && strcmp(prog, "metal_test") != 0 && strcmp(prog, "tzinit") != 0) {
		return NO;
	}
	if (level == 2 && strcmp(prog, "backboardd") != 0 && strcmp(prog, "metal_test") != 0 &&
	    strcmp(prog, "tzinit") != 0) {
		return NO;
	}
	if (level >= 2) {
		return YES;
	}

	/* Check OP2Metal.clients file */
	Dl_info info;
	if (dladdr((const void *)&shownToThisProcess, &info) != 0 && info.dli_fname != NULL) {
		char path[1024];
		strlcpy(path, info.dli_fname, sizeof(path));
		char *slash = strrchr(path, '/');
		if (slash != NULL) {
			*slash = '\0';
			slash = strrchr(path, '/');
			if (slash != NULL) {
				strlcpy(slash + 1, "OP2Metal.clients", sizeof(path) - (size_t)(slash + 1 - path));
				FILE *f = fopen(path, "r");
				if (f != NULL) {
					char line[256];
					while (fgets(line, sizeof(line), f) != NULL) {
						line[strcspn(line, " \t\r\n#")] = '\0';
						if (strcmp(line, "*") == 0 || (line[0] != '\0' && strcmp(line, prog) == 0)) {
							fclose(f);
							op2_log(@"Process %s permitted by OP2Metal.clients", prog);
							return YES;
						}
					}
					fclose(f);
				}
			}
		}
	}

	if (level == 2) {
		return YES;
	}

	op2_log(@"Process %s not shown OP2MetalDevice (metal=%d)", prog, level);
	return NO;
}

__attribute__((constructor)) static void
registerOP2Metal(void)
{
	if (!shownToThisProcess()) {
		return;
	}

	gBaseDevice = objc_getClass("MTLIOAccelDevice");
	if (gBaseDevice == Nil) {
		gBaseDevice = objc_getClass("_MTLDevice");
	}
	if (gBaseDevice == Nil) {
		op2_log(@"System lacks MTLIOAccelDevice / _MTLDevice base class; aborting registration");
		return;
	}

	if (objc_getClass("OP2MetalDevice") != Nil) {
		return;
	}

	Class device = objc_allocateClassPair(gBaseDevice, "OP2MetalDevice", 0);
	if (device == Nil) {
		op2_log(@"Failed to allocate class OP2MetalDevice");
		return;
	}

	/* Declare public and private protocols */
	class_addProtocol(device, @protocol(MTLDevice));
	gProtocols[gProtocolCount++] = @protocol(MTLDevice);

	declareProtocolFamily(device, "MTLDevice");

	Protocol *spiProto = objc_getProtocol("MTLDeviceSPI");
	if (spiProto) {
		class_addProtocol(device, spiProto);
		if (gProtocolCount < (sizeof(gProtocols) / sizeof(gProtocols[0]))) {
			gProtocols[gProtocolCount++] = spiProto;
		}
	}

	/* Bind essential MTLDevice methods */
	class_addMethod(device, @selector(initWithAcceleratorPort:), (IMP)deviceInit, "@@:I");
	class_addMethod(device, sel_registerName("initLimits"), (IMP)deviceInitLimits, "v@:");
	class_addMethod(device, @selector(name), (IMP)deviceName, "@@:");
	class_addMethod(device, @selector(registryID), (IMP)deviceRegistryID, "Q@:");
	class_addMethod(device, sel_registerName("maxBufferLength"), (IMP)deviceMaxBufferLength, "Q@:");
	class_addMethod(device, sel_registerName("maxThreadsPerThreadgroup"), (IMP)deviceMaxThreadsPerThreadgroup, "{?=QQQ}@:");
	class_addMethod(device, sel_registerName("featureProfile"), (IMP)deviceFeatureProfile, "Q@:");
	class_addMethod(device, sel_registerName("supportsFamily:"), (IMP)deviceSupportsFamily, "B@:Q");
	class_addMethod(device, sel_registerName("iosurfaceTextureAlignmentBytes"), (IMP)deviceIOSurfaceAlignment, "Q@:");
	class_addMethod(device, sel_registerName("iosurfaceReadOnlyTextureAlignmentBytes"), (IMP)deviceIOSurfaceAlignment, "Q@:");
	class_addMethod(device, sel_registerName("supportsVertexAmplificationCount:"), (IMP)deviceNo, "B@:Q");
	class_addMethod(device, sel_registerName("hasUnifiedMemory"), (IMP)deviceYes, "B@:");
	class_addMethod(device, sel_registerName("dedicatedMemorySize"), (IMP)deviceNoDedicatedMemory, "Q@:");
	class_addMethod(device, sel_registerName("sharedMemorySize"), (IMP)deviceSharedMemory, "Q@:");
	class_addMethod(device, sel_registerName("isLowPower"), (IMP)deviceNo, "B@:");
	class_addMethod(device, sel_registerName("isRemovable"), (IMP)deviceNo, "B@:");
	class_addMethod(device, sel_registerName("isHeadless"), (IMP)deviceNo, "B@:");
	class_addMethod(device, sel_registerName("supportsRenderTargetTextureRotation"), (IMP)deviceNo, "B@:");
	class_addMethod(device, sel_registerName("supportsFeatureSet:"), (IMP)deviceSupportsFeatureSet, "B@:Q");
	class_addMethod(device, sel_registerName("targetDeviceInfo"), (IMP)deviceTargetDeviceInfo, "^v@:");

	class_addMethod(device, sel_registerName("maxTextureWidth2D"), (IMP)deviceMaxTextureDimension2D, "Q@:");
	class_addMethod(device, sel_registerName("maxTextureHeight2D"), (IMP)deviceMaxTextureDimension2D, "Q@:");
	class_addMethod(device, sel_registerName("maxTextureWidth3D"), (IMP)deviceMaxTextureDimension3D, "Q@:");
	class_addMethod(device, sel_registerName("maxTextureHeight3D"), (IMP)deviceMaxTextureDimension3D, "Q@:");
	class_addMethod(device, sel_registerName("maxTextureDepth3D"), (IMP)deviceMaxTextureDimension3D, "Q@:");
	class_addMethod(device, sel_registerName("maxTextureDimensionCube"), (IMP)deviceMaxTextureDimension2D, "Q@:");
	class_addMethod(device, sel_registerName("maxTextureArrayLayers"), (IMP)deviceMaxTextureDimension3D, "Q@:");
	class_addMethod(device, sel_registerName("maxColorAttachments"), (IMP)deviceMaxColorAttachments, "Q@:");
	class_addMethod(device, sel_registerName("maxFramebufferWidth"), (IMP)deviceMaxTextureDimension2D, "Q@:");
	class_addMethod(device, sel_registerName("maxFramebufferHeight"), (IMP)deviceMaxTextureDimension2D, "Q@:");

	class_addMethod(device, sel_registerName("newCommandQueue"), (IMP)deviceNewQueue, "@@:");
	class_addMethod(device, sel_registerName("newCommandQueueWithMaxCommandBufferCount:"), (IMP)deviceNewQueueMax, "@@:Q");

	class_addMethod(device, sel_registerName("newBufferWithLength:options:"), (IMP)deviceNewBuffer, "@@:QQ");
	class_addMethod(device, sel_registerName("newBufferWithBytes:length:options:"), (IMP)deviceNewBufferBytes, "@@:r^vQQ");
	class_addMethod(device, sel_registerName("newBufferWithBytesNoCopy:length:options:deallocator:"), (IMP)deviceNewBufferNoCopy, "@@:^vQQ@?");

	class_addMethod(device, sel_registerName("newTextureWithDescriptor:"), (IMP)deviceNewTexture, "@@:@");
	class_addMethod(device, sel_registerName("newTextureWithDescriptor:iosurface:plane:"), (IMP)deviceNewTextureIOSurface, "@@:@^vQ");
	class_addMethod(device, sel_registerName("newTextureWithBytesNoCopy:length:descriptor:deallocator:"),
	                (IMP)deviceNewTextureBytesNoCopy, "@@:^vQ@@?");
	class_addMethod(device, sel_registerName("newRenderPipelineStateWithDescriptor:error:"), (IMP)deviceNewRenderPipelineState, "@@:@^@");

	class_addMethod(device, sel_registerName("newLibraryWithURL:error:"), (IMP)deviceNewLibraryURL, "@@:@^@");
	class_addMethod(device, sel_registerName("newLibraryWithFile:error:"), (IMP)deviceNewLibraryFile, "@@:@^@");
	class_addMethod(device, sel_registerName("newLibraryWithData:error:"), (IMP)deviceNewLibraryData, "@@:@^@");
	class_addMethod(device, sel_registerName("newDefaultLibrary"), (IMP)deviceNewDefaultLibrary, "@@:");
	class_addMethod(device, sel_registerName("newDefaultLibraryWithBundle:error:"), (IMP)deviceNewDefaultLibraryBundle, "@@:@^@");

	/* Forwarding fallbacks */
	class_addMethod(device, sel_registerName("methodSignatureForSelector:"), (IMP)deviceMethodSignature, "@@::");
	class_addMethod(device, sel_registerName("forwardInvocation:"), (IMP)deviceForwardInvocation, "v@:@");

	objc_registerClassPair(device);
	op2_log(@"Class OP2MetalDevice registered successfully");

	/* Bind class method +registerDevices on metaclass */
	Class metaClass = object_getClass((id)device);
	if (metaClass) {
		class_addMethod(metaClass, sel_registerName("registerDevices"), (IMP)deviceRegisterDevices, "v@:");
	}

	/* Command Queue Class */
	gQueueClass = makeCustomClass("OP2MetalQueue", "_MTLCommandQueue", "MTLCommandQueue", (IMP)queueDealloc);
	if (gQueueClass) {
		class_addMethod(gQueueClass, sel_registerName("device"), (IMP)queueDevice, "@@:");
		class_addMethod(gQueueClass, sel_registerName("label"), (IMP)queueLabel, "@@:");
		class_addMethod(gQueueClass, sel_registerName("setLabel:"), (IMP)queueSetLabel, "v@:@");
		class_addMethod(gQueueClass, sel_registerName("setGPUPriority:"), (IMP)queueSetGPUPriority, "v@:Q");
		class_addMethod(gQueueClass, sel_registerName("setGPUPriority:offset:"), (IMP)queueSetGPUPriorityOffset, "v@:QQ");
		class_addMethod(gQueueClass, sel_registerName("getGPUPriority"), (IMP)queueGetGPUPriority, "Q@:");
		class_addMethod(gQueueClass, sel_registerName("setBackgroundGPUPriority:"), (IMP)queueSetBackgroundGPUPriority, "v@:Q");
		class_addMethod(gQueueClass, sel_registerName("setBackgroundGPUPriority:offset:"), (IMP)queueSetBackgroundGPUPriorityOffset, "v@:QQ");
		class_addMethod(gQueueClass, sel_registerName("getBackgroundGPUPriority"), (IMP)queueGetBackgroundGPUPriority, "Q@:");
		class_addMethod(gQueueClass, sel_registerName("setSubmissionQueue:"), (IMP)queueSetDispatchQueue, "v@:@");
		class_addMethod(gQueueClass, sel_registerName("setCompletionQueue:"), (IMP)queueSetDispatchQueue, "v@:@");
		class_addMethod(gQueueClass, sel_registerName("finish"), (IMP)queueFinish, "v@:");
		class_addMethod(gQueueClass, sel_registerName("insertDebugCaptureBoundary"), (IMP)queueFinish, "v@:");
		class_addMethod(gQueueClass, sel_registerName("commandBuffer"), (IMP)queueCommandBuffer, "@@:");
		class_addMethod(gQueueClass, sel_registerName("commandBufferWithUnretainedReferences"), (IMP)queueCommandBuffer, "@@:");
		objc_registerClassPair(gQueueClass);
	}

	/* Command Buffer Class */
	gCommandBufferClass = makeCustomClass("OP2MetalCommandBuffer", "_MTLCommandBuffer", "MTLCommandBuffer", (IMP)commandBufferDealloc);
	if (gCommandBufferClass) {
		class_addMethod(gCommandBufferClass, sel_registerName("status"), (IMP)commandBufferStatus, "Q@:");
		class_addMethod(gCommandBufferClass, sel_registerName("error"), (IMP)commandBufferError, "@@:");
		class_addMethod(gCommandBufferClass, sel_registerName("commit"), (IMP)commandBufferCommit, "v@:");
		class_addMethod(gCommandBufferClass, sel_registerName("commitWithDeadline:"), (IMP)commandBufferCommit, "v@:d");
		class_addMethod(gCommandBufferClass, sel_registerName("enqueue"), (IMP)commandBufferEnqueue, "v@:");
		class_addMethod(gCommandBufferClass, sel_registerName("waitUntilCompleted"), (IMP)commandBufferWait, "v@:");
		class_addMethod(gCommandBufferClass, sel_registerName("waitUntilScheduled"), (IMP)commandBufferWait, "v@:");
		class_addMethod(gCommandBufferClass, sel_registerName("addCompletedHandler:"), (IMP)commandBufferAddHandler, "v@:@?");
		class_addMethod(gCommandBufferClass, sel_registerName("addScheduledHandler:"), (IMP)commandBufferAddHandler, "v@:@?");
		class_addMethod(gCommandBufferClass, sel_registerName("blitCommandEncoder"), (IMP)commandBufferBlitEncoder, "@@:");
		class_addMethod(gCommandBufferClass, sel_registerName("renderCommandEncoderWithDescriptor:"), (IMP)commandBufferRenderEncoder, "@@:@");
		class_addMethod(gCommandBufferClass, sel_registerName("presentDrawable:"), (IMP)commandBufferPresentDrawable, "v@:@");
		class_addMethod(gCommandBufferClass, sel_registerName("presentDrawable:atTime:"), (IMP)commandBufferPresentDrawableAtTime, "v@:@d");
		class_addMethod(gCommandBufferClass, sel_registerName("presentDrawable:afterMinimumDuration:"), (IMP)commandBufferPresentDrawableAfterDuration, "v@:@d");
		objc_registerClassPair(gCommandBufferClass);
	}

	/* Blit Command Encoder */
	gBlitEncoderClass = makeCustomClass("OP2MetalBlitEncoder", "_MTLCommandEncoder", "MTLBlitCommandEncoder", (IMP)queueDealloc);
	if (gBlitEncoderClass) {
		class_addMethod(gBlitEncoderClass, sel_registerName("copyFromBuffer:sourceOffset:toBuffer:destinationOffset:size:"), (IMP)blitCopyBuffer, "v@:@Q@QQ");
		class_addMethod(gBlitEncoderClass, sel_registerName("fillBuffer:range:value:"), (IMP)blitFillBuffer, "v@:@{_NSRange=QQ}C");
		class_addMethod(gBlitEncoderClass, sel_registerName("endEncoding"), (IMP)blitEndEncoding, "v@:");
		objc_registerClassPair(gBlitEncoderClass);
	}

	/* Render Command Encoder */
	gRenderEncoderClass = makeCustomClass("OP2MetalRenderEncoder", "_MTLCommandEncoder", "MTLRenderCommandEncoder", (IMP)encoderDealloc);
	if (gRenderEncoderClass) {
		Class E = gRenderEncoderClass;
		class_addMethod(E, sel_registerName("endEncoding"), (IMP)renderEndEncoding, "v@:");
		class_addMethod(E, sel_registerName("setRenderPipelineState:"), (IMP)encSetPipeline, "v@:@");
		class_addMethod(E, sel_registerName("setViewport:"), (IMP)encSetViewport, "v@:{?=dddddd}");
		class_addMethod(E, sel_registerName("setScissorRect:"), (IMP)encSetScissor, "v@:{?=QQQQ}");
		class_addMethod(E, sel_registerName("setVertexBuffer:offset:atIndex:"), (IMP)encSetVertexBuffer, "v@:@QQ");
		class_addMethod(E, sel_registerName("setFragmentBuffer:offset:atIndex:"), (IMP)encSetFragmentBuffer, "v@:@QQ");
		class_addMethod(E, sel_registerName("setVertexBufferOffset:atIndex:"), (IMP)encSetVertexBufferOffset, "v@:QQ");
		class_addMethod(E, sel_registerName("setFragmentBufferOffset:atIndex:"), (IMP)encSetFragmentBufferOffset, "v@:QQ");
		class_addMethod(E, sel_registerName("setVertexBytes:length:atIndex:"), (IMP)encSetVertexBytes, "v@:r^vQQ");
		class_addMethod(E, sel_registerName("setFragmentBytes:length:atIndex:"), (IMP)encSetFragmentBytes, "v@:r^vQQ");
		class_addMethod(E, sel_registerName("setVertexTexture:atIndex:"), (IMP)encSetVertexTexture, "v@:@Q");
		class_addMethod(E, sel_registerName("setFragmentTexture:atIndex:"), (IMP)encSetFragmentTexture, "v@:@Q");
		class_addMethod(E, sel_registerName("setVertexSamplerState:atIndex:"), (IMP)encSetVertexSampler, "v@:@Q");
		class_addMethod(E, sel_registerName("setFragmentSamplerState:atIndex:"), (IMP)encSetFragmentSampler, "v@:@Q");
		class_addMethod(E, sel_registerName("drawPrimitives:vertexStart:vertexCount:"), (IMP)encDraw1, "v@:QQQ");
		class_addMethod(E, sel_registerName("drawPrimitives:vertexStart:vertexCount:instanceCount:"), (IMP)encDraw, "v@:QQQQ");
		class_addMethod(E, sel_registerName("drawIndexedPrimitives:indexCount:indexType:indexBuffer:indexBufferOffset:"),
		                (IMP)encDrawIndexed1, "v@:QQQ@Q");
		class_addMethod(E, sel_registerName("drawIndexedPrimitives:indexCount:indexType:indexBuffer:indexBufferOffset:instanceCount:"),
		                (IMP)encDrawIndexed, "v@:QQQ@QQ");
		objc_registerClassPair(gRenderEncoderClass);
	}

	/* Texture Class */
	gTextureClass = makeCustomClass("OP2MetalTexture", "_MTLResource", "MTLTexture", (IMP)textureDealloc);
	if (gTextureClass) {
		declareProtocolFamily(gTextureClass, "MTLTexture");
		declareProtocolFamily(gTextureClass, "MTLResource");
		Protocol *tiProto = objc_getProtocol("MTLTextureImplementation");
		if (tiProto) {
			class_addProtocol(gTextureClass, tiProto);
		}
		class_addMethod(gTextureClass, sel_registerName("device"), (IMP)textureDevice, "@@:");
		class_addMethod(gTextureClass, sel_registerName("textureType"), (IMP)textureType, "Q@:");
		class_addMethod(gTextureClass, sel_registerName("pixelFormat"), (IMP)texturePixelFormat, "Q@:");
		class_addMethod(gTextureClass, sel_registerName("width"), (IMP)textureWidth, "Q@:");
		class_addMethod(gTextureClass, sel_registerName("height"), (IMP)textureHeight, "Q@:");
		class_addMethod(gTextureClass, sel_registerName("depth"), (IMP)textureDepth, "Q@:");
		class_addMethod(gTextureClass, sel_registerName("mipmapLevelCount"), (IMP)textureMipmapLevelCount, "Q@:");
		class_addMethod(gTextureClass, sel_registerName("sampleCount"), (IMP)textureSampleCount, "Q@:");
		class_addMethod(gTextureClass, sel_registerName("arrayLength"), (IMP)textureArrayLength, "Q@:");
		class_addMethod(gTextureClass, sel_registerName("usage"), (IMP)textureUsage, "Q@:");
		class_addMethod(gTextureClass, sel_registerName("iosurface"), (IMP)textureIOSurface, "^v@:");
		class_addMethod(gTextureClass, sel_registerName("iosurfacePlane"), (IMP)textureIOSurfacePlane, "Q@:");
		class_addMethod(gTextureClass, sel_registerName("isDrawable"), (IMP)textureIsDrawable, "B@:");
		class_addMethod(gTextureClass, sel_registerName("isCompressed"), (IMP)textureIsCompressed, "B@:");
		class_addMethod(gTextureClass, sel_registerName("compressionFeedback"), (IMP)textureCompressionFeedback, "q@:");
		class_addMethod(gTextureClass, sel_registerName("uniqueIdentifier"), (IMP)textureUniqueID, "Q@:");
		class_addMethod(gTextureClass, sel_registerName("allocatedSize"), (IMP)textureAllocatedSize, "Q@:");
		class_addMethod(gTextureClass, sel_registerName("gpuAddress"), (IMP)textureGPUAddress, "Q@:");
		class_addMethod(gTextureClass, sel_registerName("swizzleKey"), (IMP)textureSwizzleKey, "I@:");
		class_addMethod(gTextureClass, sel_registerName("numFaces"), (IMP)textureNumFaces, "Q@:");
		class_addMethod(gTextureClass, sel_registerName("rotation"), (IMP)textureRotation, "Q@:");
		class_addMethod(gTextureClass, sel_registerName("storageMode"), (IMP)textureStorageMode, "Q@:");
		class_addMethod(gTextureClass, sel_registerName("replaceRegion:mipmapLevel:withBytes:bytesPerRow:"),
		                (IMP)textureReplace, "v@:{?={?=QQQ}{?=QQQ}}Qr^vQ");
		class_addMethod(gTextureClass, sel_registerName("replaceRegion:mipmapLevel:slice:withBytes:bytesPerRow:bytesPerImage:"),
		                (IMP)textureReplaceSlice, "v@:{?={?=QQQ}{?=QQQ}}QQr^vQQ");
		class_addMethod(gTextureClass, sel_registerName("getBytes:bytesPerRow:fromRegion:mipmapLevel:"),
		                (IMP)textureGetBytes, "v@:^vQ{?={?=QQQ}{?=QQQ}}Q");
		objc_registerClassPair(gTextureClass);
	}

	/* Render Pipeline State Class */
	gPipelineClass = makeCustomClass("OP2MetalRenderPipelineState", "_MTLRenderPipelineState", "MTLRenderPipelineState", (IMP)pipelineDealloc);
	if (gPipelineClass) {
		declareProtocolFamily(gPipelineClass, "MTLRenderPipelineState");
		class_addMethod(gPipelineClass, sel_registerName("device"), (IMP)objectDevice, "@@:");
		class_addMethod(gPipelineClass, sel_registerName("maxTotalThreadsPerThreadgroup"), (IMP)pipelineMaxTotalThreads, "Q@:");
		class_addMethod(gPipelineClass, sel_registerName("threadgroupSizeMatchesTileSize"), (IMP)pipelineThreadgroupMatchesTile, "B@:");
		class_addMethod(gPipelineClass, sel_registerName("imageblockSampleLength"), (IMP)pipelineImageblockSampleLength, "Q@:");
		objc_registerClassPair(gPipelineClass);
	}

	/* Buffer Class */
	gBufferClass = makeCustomClass("OP2MetalBuffer", "_MTLResource", "MTLBuffer", (IMP)bufferDealloc);
	if (gBufferClass) {
		class_addMethod(gBufferClass, sel_registerName("contents"), (IMP)bufferContents, "^v@:");
		class_addMethod(gBufferClass, sel_registerName("length"), (IMP)bufferLength, "Q@:");
		class_addMethod(gBufferClass, sel_registerName("resourceOptions"), (IMP)bufferOptions, "Q@:");
		class_addMethod(gBufferClass, sel_registerName("storageMode"), (IMP)bufferStorageMode, "Q@:");
		class_addMethod(gBufferClass, sel_registerName("cpuCacheMode"), (IMP)bufferCacheMode, "Q@:");
		class_addMethod(gBufferClass, sel_registerName("hazardTrackingMode"), (IMP)bufferHazardMode, "Q@:");
		class_addMethod(gBufferClass, sel_registerName("allocatedSize"), (IMP)bufferAllocatedSize, "Q@:");
		class_addMethod(gBufferClass, sel_registerName("didModifyRange:"), (IMP)bufferDidModify, "v@:{_NSRange=QQ}");
		class_addMethod(gBufferClass, sel_registerName("gpuAddress"), (IMP)bufferGPUAddress, "Q@:");
		class_addMethod(gBufferClass, sel_registerName("setPurgeableState:"), (IMP)bufferPurgeable, "Q@:Q");
		objc_registerClassPair(gBufferClass);
	}

	/* Library Class */
	gLibraryClass = makeCustomClass("OP2MetalLibrary", "_MTLLibrary", "MTLLibrary", (IMP)libraryDealloc);
	if (gLibraryClass) {
		class_addMethod(gLibraryClass, sel_registerName("functionNames"), (IMP)libraryFunctionNames, "@@:");
		class_addMethod(gLibraryClass, sel_registerName("newFunctionWithName:"), (IMP)libraryNewFunction, "@@:@");
		class_addMethod(gLibraryClass, sel_registerName("newFunctionWithName:constantValues:error:"), (IMP)libraryNewFunctionConstants, "@@:@@^@");
		class_addMethod(gLibraryClass, sel_registerName("type"), (IMP)libraryType, "Q@:");
		objc_registerClassPair(gLibraryClass);
	}

	/* Function Class */
	gFunctionClass = makeCustomClass("OP2MetalFunction", "_MTLFunction", "MTLFunction", (IMP)functionDealloc);
	if (gFunctionClass) {
		class_addMethod(gFunctionClass, sel_registerName("name"), (IMP)functionName, "@@:");
		class_addMethod(gFunctionClass, sel_registerName("functionType"), (IMP)functionType, "Q@:");
		objc_registerClassPair(gFunctionClass);
	}

	/* G5b: sampler states on the GPU (a stub with no sampler when the GPU is not there) */
	gSamplerClass = makeCustomClass("OP2MetalSamplerState", "_MTLSamplerState", "MTLSamplerState", (IMP)samplerDealloc);
	if (gSamplerClass) {
		objc_registerClassPair(gSamplerClass);
		class_addMethod(device, sel_registerName("newSamplerStateWithDescriptor:"), (IMP)deviceNewSampler, "@@:@");
	}
	installFCHooks();

	/* Descriptor Stubs */
	for (unsigned i = 0; i < STUB_COUNT; i++) {
		gStubClass[i] = makeCustomClass(gStubSpecs[i].cls, gStubSpecs[i].base, gStubSpecs[i].protocol, NULL);
		if (gStubClass[i]) {
			objc_registerClassPair(gStubClass[i]);
			class_addMethod(device, sel_registerName(gStubSpecs[i].selector), (IMP)stubFactory, "@@:@");
		}
	}
}
