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
#include "op2_metallib.h"

#ifndef MTLHazardTrackingModeDefault
#define MTLHazardTrackingModeDefault 0
#endif

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

static void
op2_log(NSString *format, ...)
{
	va_list args;
	va_start(args, format);
	NSString *msg = [[NSString alloc] initWithFormat:format arguments:args];
	va_end(args);
	NSLog(@"[OP2Metal] %@", msg);
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

	static unsigned gLogged = 0;
	if (gLogged < 30) {
		op2_log(@"Unimplemented selector: -[%s %s] (retLen: %lu)",
		        object_getClassName(self), sel_getName(sel), (unsigned long)retLen);
		gLogged++;
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
};

static void
bufferFree(void *state)
{
	struct op2_buffer *b = (struct op2_buffer *)state;
	if (b->deallocator) {
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

static id
deviceNewBufferBytes(id self, SEL _cmd, const void *bytes, NSUInteger length, NSUInteger options)
{
	id buf = ((id (*)(id, SEL, NSUInteger, NSUInteger))objc_msgSend)(self, sel_registerName("newBufferWithLength:options:"), length, options);
	void *dest = bufferContents(buf, NULL);
	if (dest && bytes && length) {
		memcpy(dest, bytes, length);
	}
	return buf;
}

static id
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

static id
deviceNewBuffer(id self, SEL _cmd, NSUInteger length, NSUInteger options)
{
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
queueSetGPUPriority(__unused id self, __unused SEL _cmd, __unused unsigned long priority)
{
}

/* ---- Command Buffer ---- */
struct op2_cmd_buf {
	id queue;
	id presentedDrawable;
	unsigned long status;
};

static void
cmdBufFree(void *state)
{
	struct op2_cmd_buf *cb = (struct op2_cmd_buf *)state;
	cb->queue = nil;
	cb->presentedDrawable = nil;
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
			op2_log(@"Presenting drawable %p at commit", cb->presentedDrawable);
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
		cb->status = MTLCommandBufferStatusCompleted;
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

static void
commandBufferAddHandler(id self, __unused SEL _cmd, void (^handler)(id))
{
	if (handler) {
		handler(self);
	}
}

static void
commandBufferPresentDrawable(id self, __unused SEL _cmd, id drawable)
{
	op2_log(@"presentDrawable: %p", drawable);
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

static id
commandBufferRenderEncoder(__unused id self, __unused SEL _cmd, __unused id descriptor)
{
	if (gRenderEncoderClass == Nil) {
		return nil;
	}
	id enc = [[gRenderEncoderClass alloc] init];
	op2_log(@"Created render command encoder %p", enc);
	return enc;
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

/* ---- Render Command Encoder ---- */
static void
renderEndEncoding(id self, SEL _cmd)
{
	op2_log(@"renderEndEncoding on %p", self);
	struct objc_super sup = { self, class_getSuperclass(gRenderEncoderClass) };
	if (class_respondsToSelector(class_getSuperclass(gRenderEncoderClass), _cmd)) {
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
};

static void
textureFree(void *state)
{
	struct op2_texture *t = (struct op2_texture *)state;
	if (t) {
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
textureMipmapLevelCount(__unused id self, __unused SEL _cmd)
{
	return 1;
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

static id
deviceNewTextureIOSurface(id self, __unused SEL _cmd, id descriptor, void *surface, NSUInteger plane)
{
	op2_log(@"newTextureWithDescriptor:iosurface:%p plane:%lu called", surface, (unsigned long)plane);
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
	t->iosurface = surface;
	t->plane = plane;
	t->uniqueID = ++gNextTexID;
	t->isDrawable = YES;
	t->allocatedSize = t->width * t->height * 4;
	*slot = t;
	op2_log(@"Created IOSurface texture %p (%lux%lu, fmt %lu, id %llu)",
	        tex, (unsigned long)t->width, (unsigned long)t->height, (unsigned long)t->pixelFormat, (unsigned long long)t->uniqueID);
	return tex;
}

static id
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
	t->bytes = calloc(1, t->allocatedSize);
	*slot = t;
	return tex;
}

/* ---- Render Pipeline State ---- */
static id
deviceNewRenderPipelineState(id self, __unused SEL _cmd, __unused id descriptor, __unused NSError **error)
{
	if (gPipelineClass == Nil) {
		return nil;
	}
	id pipeline = [[gPipelineClass alloc] init];
	op2_log(@"newRenderPipelineStateWithDescriptor: created %p", pipeline);
	return pipeline;
}

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
	{ "newSamplerStateWithDescriptor:", "OP2MetalSamplerState", "_MTLSamplerState", "MTLSamplerState" },
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

/* ---- OP2MetalDevice Initialization ---- */
static id
deviceInit(id self, SEL _cmd, unsigned int port)
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

	class_addMethod(device, sel_registerName("newCommandQueue"), (IMP)deviceNewQueue, "@@:");
	class_addMethod(device, sel_registerName("newCommandQueueWithMaxCommandBufferCount:"), (IMP)deviceNewQueueMax, "@@:Q");

	class_addMethod(device, sel_registerName("newBufferWithLength:options:"), (IMP)deviceNewBuffer, "@@:QQ");
	class_addMethod(device, sel_registerName("newBufferWithBytes:length:options:"), (IMP)deviceNewBufferBytes, "@@:r^vQQ");
	class_addMethod(device, sel_registerName("newBufferWithBytesNoCopy:length:options:deallocator:"), (IMP)deviceNewBufferNoCopy, "@@:^vQQ@?");

	class_addMethod(device, sel_registerName("newTextureWithDescriptor:"), (IMP)deviceNewTexture, "@@:@");
	class_addMethod(device, sel_registerName("newTextureWithDescriptor:iosurface:plane:"), (IMP)deviceNewTextureIOSurface, "@@:@^vQ");
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
		class_addMethod(gQueueClass, sel_registerName("setGPUPriority:"), (IMP)queueSetGPUPriority, "v@:Q");
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
	gRenderEncoderClass = makeCustomClass("OP2MetalRenderEncoder", "_MTLCommandEncoder", "MTLRenderCommandEncoder", (IMP)queueDealloc);
	if (gRenderEncoderClass) {
		class_addMethod(gRenderEncoderClass, sel_registerName("endEncoding"), (IMP)renderEndEncoding, "v@:");
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
		objc_registerClassPair(gTextureClass);
	}

	/* Render Pipeline State Class */
	gPipelineClass = makeCustomClass("OP2MetalRenderPipelineState", "_MTLRenderPipelineState", "MTLRenderPipelineState", (IMP)queueDealloc);
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

	/* Descriptor Stubs */
	for (unsigned i = 0; i < STUB_COUNT; i++) {
		gStubClass[i] = makeCustomClass(gStubSpecs[i].cls, gStubSpecs[i].base, gStubSpecs[i].protocol, NULL);
		if (gStubClass[i]) {
			objc_registerClassPair(gStubClass[i]);
			class_addMethod(device, sel_registerName(gStubSpecs[i].selector), (IMP)stubFactory, "@@:@");
		}
	}
}
