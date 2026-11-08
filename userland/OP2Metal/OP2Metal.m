/*
 * OP2Metal: Metal Driver Plugin for OnePlus 2 (Adreno 430 / MSM8994) on iOS 12.
 *
 * Implements the Apple Metal device discovery contract:
 * 1. Discovered by Metal.framework via IOKit service IOAcceleratorES.
 * 2. Reads MetalPluginName ("OP2Metal") and MetalPluginClassName ("OP2MetalDevice").
 * 3. Dynamically subclasses _MTLDevice and conforms to <MTLDevice> and <MTLDeviceSPI>.
 * 4. Satisfies MTLCreateSystemDefaultDevice() and prevents null-device crashes
 *    across iOS 12 userland applications (Safari, Camera, Photos, Maps, CoreAnimation).
 */

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/message.h>
#import <objc/runtime.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/sysctl.h>
#include <unistd.h>
#include "op2_metallib.h"

/* IOKit Declarations */
typedef mach_port_t io_object_t;
typedef io_object_t io_service_t;
extern const mach_port_t kIOMasterPortDefault;
CFMutableDictionaryRef IOServiceMatching(const char *name);
io_service_t IOServiceGetMatchingService(mach_port_t masterPort, CFDictionaryRef matching);
kern_return_t IOObjectRelease(io_object_t object);

static Class gBaseDevice;
static unsigned int gAcceleratorPort;
static id gDeviceInstance;
static BOOL gDeviceAdded = NO;

static void
op2_log(NSString *format, ...)
{
	va_list args;
	va_start(args, format);
	NSString *msg = [[NSString alloc] initWithFormat:format arguments:args];
	va_end(args);
	NSLog(@"[OP2Metal] %@", msg);
}

/*
 * Struct returned by pointer for -targetDeviceInfo.
 * Prevents NULL-dereference panics inside QuartzCore and MTLPipelineLibraryBuilder.
 */
struct MTLTargetDeviceArch {
	uint64_t target;
	uint32_t version;
	char *name;
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
	for (unsigned i = 0; i < gProtocolCount; i++) {
		for (int required = 1; required >= 0; required--) {
			struct objc_method_description d = protocol_getMethodDescription(gProtocols[i], selector, required, YES);
			if (d.name != NULL && d.types != NULL) {
				return [NSMethodSignature signatureWithObjCTypes:d.types];
			}
		}
	}
	struct objc_super sup = { self, class_getSuperclass(object_getClass(self)) };
	id (*send)(struct objc_super *, SEL, SEL) = (id (*)(struct objc_super *, SEL, SEL))objc_msgSendSuper;
	(void)_cmd;
	return send(&sup, sel_registerName("methodSignatureForSelector:"), selector);
}

static void
deviceForwardInvocation(id self, SEL _cmd, NSInvocation *invocation)
{
	static NSMutableSet *seenSelectors = nil;
	SEL selector = [invocation selector];
	NSMethodSignature *sig = [invocation methodSignature];
	NSUInteger retLen = [sig methodReturnLength];
	NSString *selName = NSStringFromSelector(selector);

	(void)self;
	(void)_cmd;

	@synchronized(gBaseDevice) {
		if (seenSelectors == nil) {
			seenSelectors = [[NSMutableSet alloc] init];
		}
		if (![seenSelectors containsObject:selName]) {
			[seenSelectors addObject:selName];
			const char *retType = [sig methodReturnType];
			op2_log(@"Forwarding unhandled selector: -%@ (ret len %lu, type %s; replying with 0)",
			        selName, (unsigned long)retLen, retType ? retType : "?");
		}
	}

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

/* Internal ivar access */
static void **
stateOf(id self)
{
	Ivar ivar = class_getInstanceVariable(object_getClass(self), "_op2State");
	return ivar != NULL ? (void **)((char *)(__bridge void *)self + ivar_getOffset(ivar)) : NULL;
}

static void
deallocWith(id self, SEL _cmd, Class cls, void (*free_state)(void *))
{
	void **slot = stateOf(self);
	struct objc_super sup = { self, class_getSuperclass(cls) };

	if (slot != NULL && *slot != NULL) {
		if (free_state != NULL) {
			free_state(*slot);
		}
		*slot = NULL;
	}
	((void (*)(struct objc_super *, SEL))objc_msgSendSuper)(&sup, _cmd);
}

/* Base object properties */
static id
objectDevice(__unused id self, __unused SEL _cmd)
{
	return gDeviceInstance;
}

/* ---- Buffers ---- */
struct op2_buffer {
	void *memory;
	uint64_t length;
	unsigned long options;
	void (^deallocator)(void *, NSUInteger);
	BOOL mapped;
	NSString *label;
};

static void
bufferFree(void *state)
{
	struct op2_buffer *b = (struct op2_buffer *)state;
	if (b->mapped && b->memory) {
		munmap(b->memory, b->length ? b->length : 1);
	} else if (b->deallocator != nil) {
		b->deallocator(b->memory, (NSUInteger)b->length);
	}
	b->deallocator = nil;
	b->label = nil;
	free(b);
}

static void
bufferDealloc(id self, SEL _cmd)
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
	return b != NULL ? b->memory : NULL;
}

static unsigned long
bufferLength(id self, __unused SEL _cmd)
{
	struct op2_buffer *b = bufferOf(self);
	return b != NULL ? (unsigned long)b->length : 0;
}

static unsigned long
bufferOptions(id self, __unused SEL _cmd)
{
	struct op2_buffer *b = bufferOf(self);
	return b != NULL ? b->options : 0;
}

static unsigned long
bufferStorageMode(id self, SEL _cmd)
{
	return (bufferOptions(self, _cmd) >> MTLResourceStorageModeShift) & 0xf;
}

static unsigned long
bufferCacheMode(id self, SEL _cmd)
{
	return (bufferOptions(self, _cmd) >> MTLResourceCPUCacheModeShift) & 0xf;
}

static unsigned long
bufferHazardMode(id self, SEL _cmd)
{
	return (bufferOptions(self, _cmd) >> MTLResourceHazardTrackingModeShift) & 0x3;
}

static unsigned long
bufferAllocatedSize(id self, SEL _cmd)
{
	unsigned long page = (unsigned long)getpagesize();
	return (bufferLength(self, _cmd) + page - 1) / page * page;
}

static void
bufferDidModify(__unused id self, __unused SEL _cmd, __unused NSRange range)
{
}

static uint64_t
bufferGPUAddress(__unused id self, __unused SEL _cmd)
{
	return 0;
}

static unsigned long
bufferPurgeable(__unused id self, __unused SEL _cmd, __unused unsigned long state)
{
	return MTLPurgeableStateNonVolatile;
}

static id
bufferMake(void *memory, uint64_t length, unsigned long options, BOOL mapped, void (^deallocator)(void *, NSUInteger))
{
	id buffer = [[gBufferClass alloc] init];
	if (buffer == nil) {
		return nil;
	}
	struct op2_buffer *b = calloc(1, sizeof(*b));
	void **slot = stateOf(buffer);
	if (b == NULL || slot == NULL) {
		free(b);
		return nil;
	}
	b->memory = memory;
	b->length = length;
	b->options = options;
	b->mapped = mapped;
	b->deallocator = [deallocator copy];
	*slot = b;
	return buffer;
}

static id
deviceNewBuffer(id self, __unused SEL _cmd, unsigned long length, unsigned long options)
{
	gDeviceInstance = self;
	if (length > (256ull << 20)) {
		return nil;
	}
	void *mem = mmap(NULL, length ? length : 1, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
	if (mem == MAP_FAILED) {
		return nil;
	}
	return bufferMake(mem, length, options, YES, nil);
}

static id
deviceNewBufferBytes(id self, SEL _cmd, const void *bytes, unsigned long length, unsigned long options)
{
	id buf = deviceNewBuffer(self, _cmd, length, options);
	if (buf && bytes && length) {
		memcpy(bufferContents(buf, NULL), bytes, length);
	}
	return buf;
}

static id
deviceNewBufferNoCopy(id self, __unused SEL _cmd, void *bytes, unsigned long length, unsigned long options, void (^deallocator)(void *, NSUInteger))
{
	gDeviceInstance = self;
	return bufferMake(bytes, length, options, NO, deallocator);
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
queueSetGPUPriority(__unused id self, __unused SEL _cmd, __unused unsigned long priority)
{
}

static id
queueCommandBuffer(__unused id self, __unused SEL _cmd)
{
	id cb = [[gCommandBufferClass alloc] init];
	return cb;
}

static void
queueDealloc(id self, SEL _cmd)
{
	struct objc_super sup = { self, objc_getClass("NSObject") };
	void (*send)(struct objc_super *, SEL) = (void (*)(struct objc_super *, SEL))objc_msgSendSuper;
	send(&sup, _cmd);
}

static id
deviceNewQueue(id self, __unused SEL _cmd)
{
	gDeviceInstance = self;
	if (gQueueClass == Nil) {
		return nil;
	}
	id queue = [[gQueueClass alloc] init];
	op2_log(@"newCommandQueue: queue instance created");
	return queue;
}

static id
deviceNewQueueMax(id self, SEL _cmd, __unused unsigned long maxBuffers)
{
	return deviceNewQueue(self, _cmd);
}

/* ---- Command Buffer ---- */
static unsigned long
commandBufferStatus(__unused id self, __unused SEL _cmd)
{
	return MTLCommandBufferStatusCompleted;
}

static id
commandBufferError(__unused id self, __unused SEL _cmd)
{
	return nil;
}

static void
commandBufferCommit(__unused id self, __unused SEL _cmd)
{
}

static void
commandBufferEnqueue(__unused id self, __unused SEL _cmd)
{
}

static void
commandBufferWait(__unused id self, __unused SEL _cmd)
{
}

static void
commandBufferAddHandler(__unused id self, __unused SEL _cmd, void (^handler)(id))
{
	if (handler) {
		handler(self);
	}
}

static id
commandBufferBlitEncoder(__unused id self, __unused SEL _cmd)
{
	return [[gBlitEncoderClass alloc] init];
}

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

/* ---- Libraries & Functions ---- */
struct op2_library {
	NSData *data;
	struct op2_metallib info;
	struct op2_metallib_function *functions;
	NSArray *names;
	NSString *label;
};

struct op2_function {
	id library;
	struct op2_metallib_function f;
	NSString *name;
	NSString *label;
};

static void
libraryFree(void *state)
{
	struct op2_library *lib = (struct op2_library *)state;
	free(lib->functions);
	lib->data = nil;
	lib->names = nil;
	lib->label = nil;
	free(lib);
}

static void
libraryDealloc(id self, SEL _cmd)
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

static id
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
	return nil;
}

static id
libraryNewFunctionConstants(id self, SEL _cmd, NSString *name, __unused id constants, __unused NSError **error)
{
	return libraryNewFunction(self, _cmd, name);
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
functionDealloc(id self, SEL _cmd)
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

static id
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

static id
deviceNewLibraryURL(id self, __unused SEL _cmd, NSURL *url, NSError **error)
{
	gDeviceInstance = self;
	NSData *data = [NSData dataWithContentsOfURL:url options:0 error:error];
	if (data == nil) {
		return nil;
	}
	return libraryMake(data, [url path], error);
}

static id
deviceNewLibraryFile(id self, SEL _cmd, NSString *path, NSError **error)
{
	return deviceNewLibraryURL(self, _cmd, [NSURL fileURLWithPath:path], error);
}

static id
deviceNewLibraryData(id self, __unused SEL _cmd, NSData *data, NSError **error)
{
	gDeviceInstance = self;
	return libraryMake(data, @"data", error);
}

static id
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

static id
deviceNewDefaultLibrary(id self, SEL _cmd)
{
	return deviceNewDefaultLibraryBundle(self, _cmd, [NSBundle mainBundle], NULL);
}

/* Device Capability Queries */
static id
deviceName(__unused id self, __unused SEL _cmd)
{
	return @"OnePlus 2 Adreno 430 (OP2Metal)";
}

static uint64_t
deviceRegistryID(__unused id self, __unused SEL _cmd)
{
	return gAcceleratorPort;
}

static uint64_t
deviceMaxBufferLength(__unused id self, __unused SEL _cmd)
{
	return 256ull << 20;
}

static MTLSize
deviceMaxThreadsPerThreadgroup(__unused id self, __unused SEL _cmd)
{
	return MTLSizeMake(1024, 1024, 1024);
}

static NSUInteger
deviceFeatureProfile(__unused id self, __unused SEL _cmd)
{
	return 0;
}

static BOOL
deviceNo(__unused id self, __unused SEL _cmd)
{
	return NO;
}

static BOOL
deviceYes(__unused id self, __unused SEL _cmd)
{
	return YES;
}

static uint64_t
deviceSharedMemory(__unused id self, __unused SEL _cmd)
{
	return 1024ull * 1024 * 1024;
}

static uint64_t
deviceNoDedicatedMemory(__unused id self, __unused SEL _cmd)
{
	return 0;
}

static BOOL
deviceSupportsFeatureSet(__unused id self, __unused SEL _cmd, unsigned long featureSet)
{
	/* Claim support for iOS GPUFamily1_v1 (0), iOS GPUFamily2_v1 (2), Common1_v1 (8) */
	return (featureSet <= 2 || featureSet == 8);
}

/* Descriptor Stubs */
static const struct stubSpec {
	const char *selector;
	const char *cls;
	const char *base;
	const char *protocol;
} gStubSpecs[] = {
	{ "newDepthStencilStateWithDescriptor:", "OP2MetalDepthStencilState", "_MTLDepthStencilState", "MTLDepthStencilState" },
	{ "newSamplerStateWithDescriptor:", "OP2MetalSamplerState", "_MTLSamplerState", "MTLSamplerState" },
	{ "newTextureWithDescriptor:", "OP2MetalTexture", "_MTLTexture", "MTLTexture" },
	{ "newRenderPipelineStateWithDescriptor:error:", "OP2MetalRenderPipelineState", "_MTLRenderPipelineState", "MTLRenderPipelineState" },
	{ "newComputePipelineStateWithFunction:error:", "OP2MetalComputePipelineState", "_MTLComputePipelineState", "MTLComputePipelineState" },
	{ "newEventWithOptions:", "OP2MetalEvent", "_MTLEvent", "MTLEvent" },
	{ "newFence", "OP2MetalFence", "_MTLFence", "MTLFence" },
};
#define STUB_COUNT (sizeof(gStubSpecs) / sizeof(gStubSpecs[0]))
static Class gStubClass[STUB_COUNT];

static id
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

/* Device Initializer */
static id
deviceInit(id self, SEL _cmd, unsigned int port)
{
	struct objc_super sup = { self, gBaseDevice };
	id (*send)(struct objc_super *, SEL) = (id (*)(struct objc_super *, SEL))objc_msgSendSuper;

	self = send(&sup, @selector(init));
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
			level = atoi(at + 6);
		}
	}

	if (prog == NULL) {
		return NO;
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

	/*
	 * Level policy:
	 *   metal=1: metal_test only
	 *   metal=2: all processes EXCEPT backboardd (recommended: preserves CPU rendering for WindowServer!)
	 *   metal=3: all processes including backboardd
	 */
	if (level >= 3) {
		return YES;
	}
	if (level == 2 && strcmp(prog, "backboardd") != 0) {
		return YES;
	}
	if (level == 1 && strcmp(prog, "metal_test") == 0) {
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

	class_addMethod(device, sel_registerName("newCommandQueue"), (IMP)deviceNewQueue, "@@:");
	class_addMethod(device, sel_registerName("newCommandQueueWithMaxCommandBufferCount:"), (IMP)deviceNewQueueMax, "@@:Q");

	class_addMethod(device, sel_registerName("newBufferWithLength:options:"), (IMP)deviceNewBuffer, "@@:QQ");
	class_addMethod(device, sel_registerName("newBufferWithBytes:length:options:"), (IMP)deviceNewBufferBytes, "@@:r^vQQ");
	class_addMethod(device, sel_registerName("newBufferWithBytesNoCopy:length:options:deallocator:"), (IMP)deviceNewBufferNoCopy, "@@:^vQQ@?");

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
		class_addMethod(gQueueClass, sel_registerName("setGPUPriority:"), (IMP)queueSetGPUPriority, "v@:Q");
		class_addMethod(gQueueClass, sel_registerName("commandBuffer"), (IMP)queueCommandBuffer, "@@:");
		class_addMethod(gQueueClass, sel_registerName("commandBufferWithUnretainedReferences"), (IMP)queueCommandBuffer, "@@:");
		objc_registerClassPair(gQueueClass);
	}

	/* Command Buffer Class */
	gCommandBufferClass = makeCustomClass("OP2MetalCommandBuffer", "_MTLCommandBuffer", "MTLCommandBuffer", (IMP)queueDealloc);
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

	/* Descriptor Stubs */
	for (unsigned i = 0; i < STUB_COUNT; i++) {
		gStubClass[i] = makeCustomClass(gStubSpecs[i].cls, gStubSpecs[i].base, gStubSpecs[i].protocol, NULL);
		if (gStubClass[i]) {
			objc_registerClassPair(gStubClass[i]);
			class_addMethod(device, sel_registerName(gStubSpecs[i].selector), (IMP)stubFactory, "@@:@");
		}
	}
}
