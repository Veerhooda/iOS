/*
 * Built-in IOSurface for OnePlus 2 (MSM8994).
 *
 * Apple's IOSurface kext is closed. IOSurface.framework finds
 * IOServiceNameMatching("IOCoreSurfaceRoot"), opens it with type 0 and
 * talks to the user client below:
 *
 *   sel 13  limits: struct out 0x18, u16 at +0x16 must be 1 (ABI version),
 *           +0 offset alignment mask, +4 bytes-per-row alignment mask
 *   sel 0   create: scalar [parent id], struct in = serialized property
 *           dictionary (binary or XML), struct out = lock result (0xa68)
 *   sel 4   lookup by id: scalar [id], struct out = lock result
 *   sel 34  lookup by mach port: scalar [port name], struct out = lock result
 *   sel 35  create mach port: scalars [id, options], scalar out [port name]
 *   traps   0/1 use count +/- (id, category), 2 lock / 3 unlock
 *           (id, options, &seed), 4/5 release (id)
 *
 * The lock result tells the library where the surface lives in the
 * caller's address space: +0 pixels, +8 shared state page (registry id,
 * seed at +0xc), +0x10 timestamps, then id, sizes and geometry.
 * Every surface is wired kernel memory mapped into each task that creates
 * or looks it up; mappings are counted per client and dropped on release.
 */
#include "OP2Surface.h"

#if defined(ARM64_BOARD_CONFIG_MSM8994)
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/system.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSUnserialize.h>
#include <sys/proc.h>

#ifndef IOMallocData
#define IOMallocData(sz) IOMalloc(sz)
#endif
#ifndef IOFreeData
#define IOFreeData(p, sz) IOFree((p), (sz))
#endif
static inline void *
IOMallocZeroData(vm_size_t sz)
{
	void *p = IOMalloc(sz);
	if (p != NULL) {
		bzero(p, sz);
	}
	return p;
}

enum {
	kOP2SurfaceCreate             = 0,
	kOP2SurfaceRelease            = 1,
	kOP2SurfaceLock               = 2,
	kOP2SurfaceUnlock             = 3,
	kOP2SurfaceLookup             = 4,
	kOP2SurfaceLimits             = 13,
	kOP2SurfaceLookupFromPort     = 34,
	kOP2SurfaceCreatePort         = 35,
};

enum {
	kOP2SurfaceTrapIncrementUse   = 0,
	kOP2SurfaceTrapDecrementUse   = 1,
	kOP2SurfaceTrapLock           = 2,
	kOP2SurfaceTrapUnlock         = 3,
	kOP2SurfaceTrapRelease        = 4,
	kOP2SurfaceTrapReleaseFailed  = 5,
};

#define OP2_SURFACE_RESULT_SIZE       0xa68
#ifndef OP2_SURFACE_MAX_ID
#define OP2_SURFACE_MAX_ID            4096
#endif
#define OP2_SURFACE_MAX_DIM           16384
#define OP2_SURFACE_MAX_ALLOC         (256u << 20)
#define OP2_SURFACE_MAX_PROPS         (64u << 10)
#define OP2_SURFACE_ROW_ALIGN         64
#define OP2_SURFACE_TIMESTAMPS        0x800   /* offset in the state page */
#define OP2_SURFACE_STATE_SEED        0xc     /* u32 in the state page */
#define kIOSurfaceLockReadOnly        0x1

static IOLock      *op2_surface_lock;
static OP2Surface  *op2_surface_table[OP2_SURFACE_MAX_ID];
static uint32_t     op2_surface_users[OP2_SURFACE_MAX_ID];
static uint32_t     op2_surface_next_id = 1;
static uint32_t     op2_surface_creates_logged;
static uint64_t     op2_surface_wired;       /* bytes of buffer surfaces */
#define OP2_SURFACE_WIRED_MAX         (1536ull << 20)

OSDefineMetaClassAndStructors(OP2Surface, OSObject);

/*
 * Phase 8 MDP5 Direct Scanout Carveout Allocator:
 * Slot 0 is reserved for AppleCLCD default surface (fPhys = 0x03400000).
 * Slots 1..2 (0x03c00000, 0x04800000) are dynamically assigned
 * to full-screen client display surfaces (1080x1920 or 1242x2208) so the MDP5 hardware
 * can scan out directly via DMA without CPU memory copy.
 */
static bool op2_carveout_used[OP2_CARVEOUT_MAX_SLOTS] = { true, false, false };
static IOLock *op2_carveout_lock = NULL;

uint64_t
op2_carveout_alloc_slot(uint32_t size)
{
	if (size > OP2_CARVEOUT_SLOT_SIZE) {
		return 0;
	}
	if (op2_carveout_lock == NULL) {
		IOLock *lock = IOLockAlloc();
		if (!OSCompareAndSwapPtr(NULL, lock, &op2_carveout_lock)) {
			IOLockFree(lock);
		}
	}
	IOLockLock(op2_carveout_lock);
	uint64_t phys = 0;
	for (uint32_t i = 1; i < OP2_CARVEOUT_MAX_SLOTS; i++) {
		if (!op2_carveout_used[i]) {
			op2_carveout_used[i] = true;
			phys = OP2_CARVEOUT_BASE + OP2_CARVEOUT_SLOT0_SIZE + (uint64_t)(i - 1) * OP2_CARVEOUT_SLOT_SIZE;
			break;
		}
	}
	IOLockUnlock(op2_carveout_lock);
	return phys;
}

void
op2_carveout_free_slot(uint64_t phys)
{
	if (phys < OP2_CARVEOUT_BASE + OP2_CARVEOUT_SLOT0_SIZE ||
	    phys >= OP2_CARVEOUT_BASE + OP2_CARVEOUT_SIZE ||
	    op2_carveout_lock == NULL) {
		return;
	}
	uint32_t slot = 1 + (uint32_t)((phys - (OP2_CARVEOUT_BASE + OP2_CARVEOUT_SLOT0_SIZE)) / OP2_CARVEOUT_SLOT_SIZE);
	if (slot >= 1 && slot < OP2_CARVEOUT_MAX_SLOTS) {
		IOLockLock(op2_carveout_lock);
		op2_carveout_used[slot] = false;
		IOLockUnlock(op2_carveout_lock);
	}
}

void
OP2Surface::free()
{
	if (memory != NULL) {
		IOPhysicalLength segLen = 0;
		uint64_t phys = (uint64_t)memory->getPhysicalSegment(0, &segLen);
		if (phys >= OP2_CARVEOUT_BASE + OP2_CARVEOUT_SLOT0_SIZE &&
		    phys < OP2_CARVEOUT_BASE + OP2_CARVEOUT_SIZE) {
			op2_carveout_free_slot(phys);
		} else if (OSDynamicCast(IOBufferMemoryDescriptor, memory) != NULL) {
			OSAddAtomic64(-(SInt64)allocSize, (volatile SInt64 *)&op2_surface_wired);
		}
	}
	OSSafeReleaseNULL(kernelMap);
	OSSafeReleaseNULL(memory);
	OSSafeReleaseNULL(state);
	OSObject::free();
}

uintptr_t
OP2Surface::kernelAddress()
{
	if (kernelBuffer != NULL) {
		return (uintptr_t)kernelBuffer;
	}
	IOBufferMemoryDescriptor *buffer = OSDynamicCast(IOBufferMemoryDescriptor, (OSObject *)memory);
	if (buffer != NULL) {
		kernelBuffer = buffer->getBytesNoCopy();
		return (uintptr_t)kernelBuffer;
	}
	IOLockLock(op2_surface_lock);
	if (kernelMap == NULL && memory != NULL) {
		kernelMap = memory->map(kIOMapWriteCombineCache);
	}
	if (kernelMap != NULL) {
		kernelBuffer = (void *)kernelMap->getVirtualAddress();
	}
	IOLockUnlock(op2_surface_lock);
	return (uintptr_t)kernelBuffer;
}

static uint32_t *
op2_surface_seed(OP2Surface *s)
{
	return (uint32_t *)(void *)((uint8_t *)s->state->getBytesNoCopy() + OP2_SURFACE_STATE_SEED);
}

uint32_t
op2_surface_get_seed(OP2Surface *s)
{
	if (s == NULL || s->state == NULL) return 0;
	void *b = s->state->getBytesNoCopy();
	if (b == NULL) return 0;
	volatile uint32_t *sp = (volatile uint32_t *)(void *)((uint8_t *)b + OP2_SURFACE_STATE_SEED);
	return *sp;
}

/*
 * Give the surface an id and a table slot; the table holds one retain and a
 * user count (creators, client mappings, kernel holders). Lock held.
 */
static bool
op2_surface_insert_locked(OP2Surface *s)
{
	for (uint32_t n = 0; n < OP2_SURFACE_MAX_ID - 1; n++) {
		uint32_t id = op2_surface_next_id;
		op2_surface_next_id = id + 1 < OP2_SURFACE_MAX_ID ? id + 1 : 1;
		if (op2_surface_table[id] == NULL) {
			s->id = id;
			s->retain();
			op2_surface_table[id] = s;
			op2_surface_users[id] = 1;
			*(uint64_t *)s->state->getBytesNoCopy() = id;   /* registry id */
			return true;
		}
	}
	return false;
}

/* Add a user to a surface that may have left the table (port lookups). */
static bool
op2_surface_get(OP2Surface *s)
{
	bool ok = true;
	IOLockLock(op2_surface_lock);
	if (op2_surface_table[s->id] == s) {
		op2_surface_users[s->id]++;
	} else if (op2_surface_table[s->id] == NULL) {
		s->retain();
		op2_surface_table[s->id] = s;
		op2_surface_users[s->id] = 1;
	} else {
		ok = false;
	}
	IOLockUnlock(op2_surface_lock);
	return ok;
}

/* Drop a user; the last one removes the surface from the table. */
static void
op2_surface_put(OP2Surface *s)
{
	bool drop = false;
	IOLockLock(op2_surface_lock);
	if (op2_surface_table[s->id] == s && --op2_surface_users[s->id] == 0) {
		op2_surface_table[s->id] = NULL;
		drop = true;
	}
	IOLockUnlock(op2_surface_lock);
	if (drop) {
		s->release();
	}
}

OP2Surface *
op2_surface_copy(uint32_t id)
{
	OP2Surface *s = NULL;
	if (id == 0 || id >= OP2_SURFACE_MAX_ID || op2_surface_lock == NULL) {
		return NULL;
	}
	IOLockLock(op2_surface_lock);
	s = op2_surface_table[id];
	if (s != NULL) {
		s->retain();
	}
	IOLockUnlock(op2_surface_lock);
	return s;
}

/* The table lock; the framebuffer may publish a surface before the root starts. */
static void
op2_surface_init_lock(void)
{
	if (op2_surface_lock == NULL) {
		IOLock *lock = IOLockAlloc();
		if (!OSCompareAndSwapPtr(NULL, lock, &op2_surface_lock)) {
			IOLockFree(lock);
		}
	}
}

static OP2Surface *
op2_surface_alloc(void)
{
	op2_surface_init_lock();
	OP2Surface *s = OSTypeAlloc(OP2Surface);
	if (s == NULL || !s->init()) {
		OSSafeReleaseNULL(s);
		return NULL;
	}
	s->state = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,
	    kIODirectionInOut | kIOMemoryKernelUserShared, page_size, page_size);
	if (s->state == NULL) {
		s->release();
		return NULL;
	}
	bzero(s->state->getBytesNoCopy(), page_size);
	*op2_surface_seed(s) = 1;
	return s;
}

static bool
op2_surface_publish(OP2Surface *s)
{
	bool ok;
	IOLockLock(op2_surface_lock);
	ok = op2_surface_insert_locked(s);
	IOLockUnlock(op2_surface_lock);
	return ok;
}

OP2Surface *
op2_surface_create_physical(uint64_t phys, uint32_t width, uint32_t height,
    uint32_t bytesPerRow, uint32_t pixelFormat)
{
	OP2Surface *s = op2_surface_alloc();
	if (s == NULL) {
		return NULL;
	}
	s->width = width;
	s->height = height;
	s->bytesPerRow = bytesPerRow;
	s->bytesPerElement = 4;
	s->elementWidth = s->elementHeight = 1;
	s->pixelFormat = pixelFormat;
	s->allocSize = (uint32_t)round_page(bytesPerRow * height);
	s->memory = IOMemoryDescriptor::withPhysicalAddress(phys, s->allocSize, kIODirectionInOut);
	s->mapCache = kIOMapWriteCombineCache;
	if (s->memory != NULL) {
		s->kernelMap = s->memory->map(kIOMapWriteCombineCache);
		if (s->kernelMap != NULL) {
			s->kernelBuffer = (void *)s->kernelMap->getVirtualAddress();
		}
	}
	if (s->memory == NULL || !op2_surface_publish(s)) {
		s->release();
		return NULL;
	}
	return s;
}

mach_port_name_t
op2_surface_port_in_task(OP2Surface *s, task_t task)
{
	mach_port_name_t name = 0;
	IOUserClient::copyPortNameForObjectInTask(task, s, &name);
	return name;
}

static uint32_t
op2_dict_u32(OSDictionary *dict, const char *key, uint32_t def)
{
	OSNumber *n = OSDynamicCast(OSNumber, dict->getObject(key));
	return n != NULL ? n->unsigned32BitValue() : def;
}

static uint32_t
op2_div_up(uint32_t a, uint32_t b)
{
	return b > 1 ? (a + b - 1) / b : a;
}

static uint32_t
op2_row_bytes(uint32_t width, uint32_t ew, uint32_t bpe)
{
	uint64_t row = (uint64_t)op2_div_up(width, ew) * bpe;
	return (uint32_t)((row + OP2_SURFACE_ROW_ALIGN - 1) & ~(uint64_t)(OP2_SURFACE_ROW_ALIGN - 1));
}

/* Geometry from the creation properties (IOSurface.h keys). */
static IOReturn
op2_surface_layout(OP2Surface *s, OSDictionary *props)
{
	uint64_t end;

	s->width = op2_dict_u32(props, "IOSurfaceWidth", 0);
	s->height = op2_dict_u32(props, "IOSurfaceHeight", 0);
	s->bytesPerElement = (uint16_t)op2_dict_u32(props, "IOSurfaceBytesPerElement", 4);
	s->elementWidth = (uint8_t)op2_dict_u32(props, "IOSurfaceElementWidth", 1);
	s->elementHeight = (uint8_t)op2_dict_u32(props, "IOSurfaceElementHeight", 1);
	s->pixelFormat = op2_dict_u32(props, "IOSurfacePixelFormat", 0);
	s->cacheMode = op2_dict_u32(props, "IOSurfaceCacheMode", 0);
	s->offset = op2_dict_u32(props, "IOSurfaceOffset", 0);
	if (s->width > OP2_SURFACE_MAX_DIM || s->height > OP2_SURFACE_MAX_DIM ||
	    s->bytesPerElement > 64 || s->elementWidth == 0 || s->elementHeight == 0) {
		return kIOReturnBadArgument;
	}
	s->bytesPerRow = op2_dict_u32(props, "IOSurfaceBytesPerRow",
	    op2_row_bytes(s->width, s->elementWidth, s->bytesPerElement));
	if (s->bytesPerRow < (uint64_t)op2_div_up(s->width, s->elementWidth) * s->bytesPerElement) {
		return kIOReturnBadArgument;
	}
	end = (uint64_t)s->offset + (uint64_t)s->bytesPerRow * op2_div_up(s->height, s->elementHeight);

	OSArray *planes = OSDynamicCast(OSArray, props->getObject("IOSurfacePlaneInfo"));
	s->planeCount = 0;
	if (planes != NULL) {
		uint64_t next = s->offset;
		if (planes->getCount() > OP2_SURFACE_MAX_PLANES) {
			return kIOReturnBadArgument;
		}
		for (unsigned int i = 0; i < planes->getCount(); i++) {
			OSDictionary *pd = OSDynamicCast(OSDictionary, planes->getObject(i));
			struct op2_surface_plane *p = &s->planes[i];
			if (pd == NULL) {
				return kIOReturnBadArgument;
			}
			p->width = op2_dict_u32(pd, "IOSurfacePlaneWidth", s->width);
			p->height = op2_dict_u32(pd, "IOSurfacePlaneHeight", s->height);
			p->bytesPerElement = (uint16_t)op2_dict_u32(pd, "IOSurfacePlaneBytesPerElement", 1);
			p->elementWidth = (uint8_t)op2_dict_u32(pd, "IOSurfacePlaneElementWidth", 1);
			p->elementHeight = (uint8_t)op2_dict_u32(pd, "IOSurfacePlaneElementHeight", 1);
			if (p->width > OP2_SURFACE_MAX_DIM || p->height > OP2_SURFACE_MAX_DIM ||
			    p->bytesPerElement > 64 || p->elementWidth == 0 || p->elementHeight == 0) {
				return kIOReturnBadArgument;
			}
			p->bytesPerRow = op2_dict_u32(pd, "IOSurfacePlaneBytesPerRow",
			    op2_row_bytes(p->width, p->elementWidth, p->bytesPerElement));
			p->offset = op2_dict_u32(pd, "IOSurfacePlaneOffset", (uint32_t)next);
			p->size = op2_dict_u32(pd, "IOSurfacePlaneSize",
			    p->bytesPerRow * op2_div_up(p->height, p->elementHeight));
			next = round_page((uint64_t)p->offset + p->size);
			if ((uint64_t)p->offset + p->size > end) {
				end = (uint64_t)p->offset + p->size;
			}
		}
		s->planeCount = planes->getCount();
	}

	uint64_t alloc = op2_dict_u32(props, "IOSurfaceAllocSize", 0);
	if (alloc < end) {
		alloc = end;
	}
	alloc = round_page(alloc);
	if (alloc == 0 || alloc > OP2_SURFACE_MAX_ALLOC) {
		return kIOReturnNoMemory;
	}
	s->allocSize = (uint32_t)alloc;
	return kIOReturnSuccess;
}

/* Per-client mapping of one surface. */
class OP2SurfaceRef : public OSObject {
	OSDeclareDefaultStructors(OP2SurfaceRef);

public:
	OP2Surface  *surface;
	IOMemoryMap *data;
	IOMemoryMap *state;
	uint32_t     count;

	bool init() APPLE_KEXT_OVERRIDE { return OSObject::init(); }
	void free() APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(OP2SurfaceRef, OSObject);

void
OP2SurfaceRef::free()
{
	OSSafeReleaseNULL(data);
	OSSafeReleaseNULL(state);
	if (surface != NULL) {
		op2_surface_put(surface);
		surface->release();
		surface = NULL;
	}
	OSObject::free();
}

class IOCoreSurfaceRoot : public IOService {
	OSDeclareDefaultStructors(IOCoreSurfaceRoot);

public:
	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(IOCoreSurfaceRoot, IOService);

bool
IOCoreSurfaceRoot::start(IOService *provider)
{
	if (!IOService::start(provider)) {
		return false;
	}
	op2_surface_init_lock();
	setProperty(kIOUserClientClassKey, "IOSurfaceRootUserClient");
	registerService();
	IOLog("OP2 SURF: IOCoreSurfaceRoot registered\n");
	return true;
}

class IOSurfaceRootUserClient : public IOUserClient {
	OSDeclareDefaultStructors(IOSurfaceRootUserClient);

	task_t   fTask;
	IOLock  *fLock;
	OSArray *fRefs;

	OP2SurfaceRef *findRef(uint32_t id, unsigned int *index);
	IOReturn addRef(OP2Surface *s, OP2SurfaceRef **refOut, bool *existing);
	IOReturn returnSurface(OP2Surface *s, IOExternalMethodArguments *args);
	IOReturn create(IOExternalMethodArguments *args);

public:
	bool initWithTask(task_t owningTask, void *securityID, UInt32 type) APPLE_KEXT_OVERRIDE;
	void free() APPLE_KEXT_OVERRIDE;
	IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args,
	    IOExternalMethodDispatch *dispatch, OSObject *target,
	    void *reference) APPLE_KEXT_OVERRIDE;
	IOExternalTrap *getTargetAndTrapForIndex(IOService **targetP, UInt32 index) APPLE_KEXT_OVERRIDE;

	IOReturn trapUseCount(void *id, void *category, void *, void *, void *, void *);
	IOReturn trapLock(void *id, void *options, void *seedOut, void *, void *, void *);
	IOReturn trapUnlock(void *id, void *options, void *seedOut, void *, void *, void *);
	IOReturn trapRelease(void *id, void *, void *, void *, void *, void *);
};

OSDefineMetaClassAndStructors(IOSurfaceRootUserClient, IOUserClient);

static uint64_t op2_surf_logged;      /* selectors / traps already logged (< 64) */

static void
op2_surf_log_once(const char *what, uint32_t n)
{
	uint32_t bit = (what[0] == 't' ? 48 : 0) + n;
	if (bit >= 64 || (op2_surf_logged & (1ULL << bit))) {
		return;
	}
	op2_surf_logged |= 1ULL << bit;
	char name[32];
	proc_selfname(name, sizeof(name));
	IOLog("OP2 SURF: %s %s %u\n", name, what, n);
}

bool
IOSurfaceRootUserClient::initWithTask(task_t owningTask, void *securityID, UInt32 type)
{
	if (!IOUserClient::initWithTask(owningTask, securityID, type)) {
		return false;
	}
	fTask = owningTask;
	fLock = IOLockAlloc();
	fRefs = OSArray::withCapacity(16);
	return fLock != NULL && fRefs != NULL;
}

void
IOSurfaceRootUserClient::free()
{
	OSSafeReleaseNULL(fRefs);
	if (fLock != NULL) {
		IOLockFree(fLock);
		fLock = NULL;
	}
	IOUserClient::free();
}

IOReturn
IOSurfaceRootUserClient::clientClose(void)
{
	IOLockLock(fLock);
	fRefs->flushCollection();
	IOLockUnlock(fLock);
	terminate();
	return kIOReturnSuccess;
}

/* Lock held. */
OP2SurfaceRef *
IOSurfaceRootUserClient::findRef(uint32_t id, unsigned int *index)
{
	for (unsigned int i = 0; i < fRefs->getCount(); i++) {
		OP2SurfaceRef *ref = (OP2SurfaceRef *)fRefs->getObject(i);
		if (ref->surface->id == id) {
			if (index != NULL) {
				*index = i;
			}
			return ref;
		}
	}
	return NULL;
}

/*
 * Count one more create/lookup of s by this client, mapping it into the
 * task the first time. Takes over the caller's user count on s.
 *
 * A lookup of a surface this client already has maps nothing and counts
 * nothing (*existing): IOSurface.framework then gets base address -1 in the
 * lock result and reuses its cached IOSurfaceClient.
 */
IOReturn
IOSurfaceRootUserClient::addRef(OP2Surface *s, OP2SurfaceRef **refOut, bool *existing)
{
	*existing = false;
	IOLockLock(fLock);
	OP2SurfaceRef *ref = findRef(s->id, NULL);
	if (ref != NULL && ref->surface == s) {
		IOLockUnlock(fLock);
		op2_surface_put(s);
		*refOut = ref;
		*existing = true;
		return kIOReturnSuccess;
	}
	IOLockUnlock(fLock);

	ref = OSTypeAlloc(OP2SurfaceRef);
	if (ref == NULL || !ref->init()) {
		OSSafeReleaseNULL(ref);
		op2_surface_put(s);
		return kIOReturnNoMemory;
	}
	s->retain();
	ref->surface = s;
	ref->count = 1;
	ref->data = s->memory->createMappingInTask(fTask, 0, kIOMapAnywhere | s->mapCache);
	ref->state = s->state->createMappingInTask(fTask, 0, kIOMapAnywhere | kIOMapReadOnly);
	if (ref->data == NULL || ref->state == NULL) {
		ref->release();
		return kIOReturnVMError;
	}
	IOLockLock(fLock);
	OP2SurfaceRef *raced = findRef(s->id, NULL);
	if (raced != NULL && raced->surface == s) {
		/* Another thread mapped it meanwhile: count there, drop ours. */
		raced->count++;
		IOLockUnlock(fLock);
		ref->release();
		*refOut = raced;
		return kIOReturnSuccess;
	}
	fRefs->setObject(ref);
	IOLockUnlock(fLock);
	ref->release();         /* fRefs holds it */
	*refOut = ref;
	return kIOReturnSuccess;
}

static inline uint32_t
get32(const uint8_t *p, uint32_t off)
{
	uint32_t v;
	memcpy(&v, p + off, 4);
	return v;
}

static void
put32(uint8_t *p, uint32_t off, uint32_t v)
{
	memcpy(p + off, &v, 4);
}

static void
put64(uint8_t *p, uint32_t off, uint64_t v)
{
	memcpy(p + off, &v, 8);
}

/* IOSurface.framework's lock result (copied to IOSurfaceClient + 0x58). */
static void
op2_surface_fill_result(OP2Surface *s, OP2SurfaceRef *ref, uint8_t *out)
{
	bzero(out, OP2_SURFACE_RESULT_SIZE);
	put64(out, 0x00, ref->data->getAddress());
	put64(out, 0x08, ref->state->getAddress());
	put64(out, 0x10, ref->state->getAddress() + OP2_SURFACE_TIMESTAMPS);
	put32(out, 0x18, s->id);
	put32(out, 0x1c, s->allocSize);
	put32(out, 0x20, s->width);
	put32(out, 0x24, s->height);
	put32(out, 0x28, s->bytesPerRow);
	put32(out, 0x2c, s->offset);
	put32(out, 0x30, s->pixelFormat);
	put32(out, 0x34, s->allocSize);
	put32(out, 0x38, s->planeCount);
	memcpy(out + 0x40, &s->bytesPerElement, 2);
	out[0x42] = s->elementWidth;
	out[0x43] = s->elementHeight;
	put32(out, 0x44, s->cacheMode);
	put32(out, 0x48, op2_surface_get_seed(s));
	put32(out, 0x50, s->parentID);
	put32(out, 0x54, 1);
	for (uint32_t i = 0; i < s->planeCount; i++) {
		uint8_t *p = out + 0x50 + i * 0x6c;
		const struct op2_surface_plane *pl = &s->planes[i];
		put32(p, 0x00, pl->width);
		put32(p, 0x04, pl->height);
		put32(p, 0x08, pl->bytesPerRow);
		put32(p, 0x0c, pl->offset);
		put32(p, 0x10, pl->size);
		memcpy(p + 0x18, &pl->bytesPerElement, 2);
		p[0x1a] = pl->elementWidth;
		p[0x1b] = pl->elementHeight;
	}
}

static IOReturn
op2_struct_out(IOExternalMethodArguments *args, const void *buf, uint32_t len)
{
	if (args->structureOutputDescriptor != NULL) {
		uint32_t copy_len = len;
		if (copy_len > args->structureOutputDescriptor->getLength()) {
			copy_len = (uint32_t)args->structureOutputDescriptor->getLength();
		}
		args->structureOutputDescriptor->writeBytes(0, buf, copy_len);
		args->structureOutputDescriptorSize = copy_len;
		return kIOReturnSuccess;
	}
	if (args->structureOutput == NULL) {
		return kIOReturnBadArgument;
	}
	uint32_t copy_len = len;
	if (copy_len > args->structureOutputSize) {
		copy_len = args->structureOutputSize;
	}
	memcpy(args->structureOutput, buf, copy_len);
	args->structureOutputSize = copy_len;
	return kIOReturnSuccess;
}

/* Map s for this client and answer with its lock result. Takes s's user count. */
IOReturn
IOSurfaceRootUserClient::returnSurface(OP2Surface *s, IOExternalMethodArguments *args)
{
	OP2SurfaceRef *ref = NULL;
	bool existing = false;
	IOReturn ret = addRef(s, &ref, &existing);
	if (ret != kIOReturnSuccess) {
		return ret;
	}
	uint8_t *out = (uint8_t *)IOMallocZeroData(OP2_SURFACE_RESULT_SIZE);
	if (out == NULL) {
		ret = kIOReturnNoMemory;
	} else {
		op2_surface_fill_result(s, ref, out);
		if (existing) {
			put64(out, 0x00, ~0ULL);        /* "already yours": use the cached client */
		}
		ret = op2_struct_out(args, out, OP2_SURFACE_RESULT_SIZE);
		IOFreeData(out, OP2_SURFACE_RESULT_SIZE);
	}
	if (ret != kIOReturnSuccess && !existing) {
		/* The caller never learns the id: undo this create/lookup. */
		trapRelease((void *)(uintptr_t)s->id, NULL, NULL, NULL, NULL, NULL);
	}
	return ret;
}

IOReturn
IOSurfaceRootUserClient::create(IOExternalMethodArguments *args)
{
	const char *buf = (const char *)args->structureInput;
	uint32_t len = args->structureInputSize;
	char *copy = NULL;
	IOReturn ret;

	if (args->structureInputDescriptor != NULL) {
		len = (uint32_t)args->structureInputDescriptor->getLength();
		if (len == 0 || len > OP2_SURFACE_MAX_PROPS) {
			return kIOReturnBadArgument;
		}
		copy = (char *)IOMallocData(len);
		if (copy == NULL) {
			return kIOReturnNoMemory;
		}
		args->structureInputDescriptor->readBytes(0, copy, len);
		buf = copy;
	}
	OSObject *obj = (buf != NULL && len > 0) ? OSUnserializeXML(buf, len) : NULL;
	if (copy != NULL) {
		IOFreeData(copy, len);
	}
	OSDictionary *props = OSDynamicCast(OSDictionary, obj);
	if (props == NULL) {
		OSSafeReleaseNULL(obj);
		IOLog("OP2 SURF: create: bad property dictionary (%u bytes)\n", len);
		return kIOReturnBadArgument;
	}

	OP2Surface *s = op2_surface_alloc();
	if (s == NULL) {
		props->release();
		return kIOReturnNoMemory;
	}
	s->parentID = args->scalarInputCount > 0 ? (uint32_t)args->scalarInput[0] : 0;
	ret = op2_surface_layout(s, props);
	if (ret == kIOReturnSuccess) {
		IOMemoryDescriptor *mem = NULL;
		uint64_t carveout_phys = 0;

		/*
		 * MDP5 Direct Scanout Optimization:
		 * Only allocate from carveout if the surface matches the panel scanout
		 * dimensions (1080x1920) so MDP5 can scan it out directly without downsampling.
		 * Surfaces that require downsampling (like SpringBoard's 1242x2208) use
		 * normal cached system RAM (IOBufferMemoryDescriptor) for high-speed CPU
		 * compositing and downsampling.
		 */
		if (s->width == 1080 && s->height == 1920 && s->allocSize <= OP2_CARVEOUT_SLOT_SIZE) {
			carveout_phys = op2_carveout_alloc_slot(s->allocSize);
		}

		if (carveout_phys != 0) {
			mem = IOMemoryDescriptor::withPhysicalAddress(carveout_phys, s->allocSize, kIODirectionInOut);
			s->mapCache = kIOMapWriteCombineCache;
			if (mem != NULL) {
				s->kernelMap = mem->map(kIOMapWriteCombineCache);
				if (s->kernelMap != NULL) {
					s->kernelBuffer = (void *)s->kernelMap->getVirtualAddress();
				}
			}
		} else {
			if ((uint64_t)OSAddAtomic64(s->allocSize, (volatile SInt64 *)&op2_surface_wired) +
			    s->allocSize <= OP2_SURFACE_WIRED_MAX) {
				mem = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,
				    kIODirectionInOut | kIOMemoryKernelUserShared, s->allocSize, page_size);
			}
			if (mem == NULL) {
				OSAddAtomic64(-(SInt64)s->allocSize, (volatile SInt64 *)&op2_surface_wired);
				ret = kIOReturnNoMemory;
			} else {
				s->kernelBuffer = ((IOBufferMemoryDescriptor *)mem)->getBytesNoCopy();
				s->mapCache = kIOMapDefaultCache;
			}
		}

		if (mem != NULL) {
			if (s->kernelBuffer != NULL && carveout_phys == 0) {
				bzero(s->kernelBuffer, s->allocSize);
			}
			s->memory = mem;
			if (!op2_surface_publish(s)) {
				ret = kIOReturnNoResources;
			} else if (carveout_phys != 0) {
				IOLog("OP2 SURF: allocated carveout slot 0x%llx for surface %u (%ux%u)\n",
				    carveout_phys, s->id, s->width, s->height);
			}
		}
	}
	if (ret != kIOReturnSuccess || op2_surface_creates_logged < 64) {
		char name[32];
		proc_selfname(name, sizeof(name));
		op2_surface_creates_logged++;
		IOLog("OP2 SURF: %s create id %u %ux%u fmt 0x%x bpe %u bpr %u planes %u size %u keys %u -> 0x%x\n",
		    name, s->id, s->width, s->height, s->pixelFormat, s->bytesPerElement,
		    s->bytesPerRow, s->planeCount, s->allocSize, props->getCount(), ret);
	}
	props->release();
	if (ret != kIOReturnSuccess) {
		s->release();
		return ret;
	}
	ret = returnSurface(s, args);   /* takes the creator's user count */
	s->release();
	return ret;
}

IOReturn
IOSurfaceRootUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments *args,
    __unused IOExternalMethodDispatch *dispatch, __unused OSObject *target,
    __unused void *reference)
{
	uint64_t arg0 = args->scalarInputCount > 0 ? args->scalarInput[0] : 0;

	switch (selector) {
	case kOP2SurfaceLimits: {
		uint8_t limits[0x18] = { 0 };
		put32(limits, 0x0, OP2_SURFACE_ROW_ALIGN - 1);
		put32(limits, 0x4, OP2_SURFACE_ROW_ALIGN - 1);
		limits[0x16] = 1;
		op2_surf_log_once("sel", selector);
		return op2_struct_out(args, limits, sizeof(limits));
	}
	case kOP2SurfaceCreate:
		return create(args);
	case kOP2SurfaceLock: {
		uint32_t id = 0, options = 0;
		if (args->structureInput != NULL && args->structureInputSize >= 8) {
			id = get32((const uint8_t *)args->structureInput, 0);
			options = get32((const uint8_t *)args->structureInput, 4);
		} else if (args->scalarInputCount >= 1) {
			id = (uint32_t)args->scalarInput[0];
			options = args->scalarInputCount >= 2 ? (uint32_t)args->scalarInput[1] : 0;
		}
		IOLockLock(fLock);
		OP2SurfaceRef *ref = findRef(id, NULL);
		if (ref != NULL) {
			ref->retain();
		}
		IOLockUnlock(fLock);
		if (ref == NULL) {
			OP2Surface *s = op2_surface_copy(id);
			if (s == NULL) {
				return kIOReturnNotFound;
			}
			bool existing = false;
			IOReturn ret = addRef(s, &ref, &existing);
			s->release();
			if (ret != kIOReturnSuccess || ref == NULL) {
				return kIOReturnNotFound;
			}
			ref->retain();
		}
		uint8_t *out = (uint8_t *)IOMallocZeroData(OP2_SURFACE_RESULT_SIZE);
		if (out == NULL) {
			ref->release();
			return kIOReturnNoMemory;
		}
		op2_surface_fill_result(ref->surface, ref, out);
		static uint32_t op2_surf_locks_logged = 0;
		if (op2_surf_locks_logged < 16) {
			op2_surf_locks_logged++;
			IOLog("OP2 SURF: lock id %u options 0x%x -> seed %u base 0x%llx\n",
			    id, options, op2_surface_get_seed(ref->surface), (unsigned long long)ref->data->getAddress());
		}
		IOReturn ret = op2_struct_out(args, out, OP2_SURFACE_RESULT_SIZE);
		IOFreeData(out, OP2_SURFACE_RESULT_SIZE);
		ref->release();
		op2_surf_log_once("sel", selector);
		return ret;
	}
	case kOP2SurfaceUnlock: {
		uint32_t id = 0, options = 0;
		if (args->structureInput != NULL && args->structureInputSize >= 8) {
			id = get32((const uint8_t *)args->structureInput, 0);
			options = get32((const uint8_t *)args->structureInput, 4);
		} else if (args->scalarInputCount >= 1) {
			id = (uint32_t)args->scalarInput[0];
			options = args->scalarInputCount >= 2 ? (uint32_t)args->scalarInput[1] : 0;
		}
		OP2Surface *s = op2_surface_copy(id);
		if (s == NULL) {
			return kIOReturnNotFound;
		}
		uint32_t new_seed;
		if (options & kIOSurfaceLockReadOnly) {
			new_seed = op2_surface_get_seed(s);
		} else {
			new_seed = OSIncrementAtomic((volatile SInt32 *)op2_surface_seed(s)) + 1;
		}
		__asm__ volatile("dmb sy" ::: "memory");
		static uint32_t op2_surf_unlocks_logged = 0;
		if (op2_surf_unlocks_logged < 16) {
			op2_surf_unlocks_logged++;
			IOLog("OP2 SURF: unlock id %u options 0x%x -> new_seed %u\n", id, options, new_seed);
		}
		if (args->structureOutput != NULL && args->structureOutputSize >= 4) {
			memcpy(args->structureOutput, &new_seed, sizeof(new_seed));
			args->structureOutputSize = 4;
		}
		if (args->scalarOutputCount >= 1) {
			args->scalarOutput[0] = new_seed;
			args->scalarOutputCount = 1;
		}
		s->release();
		op2_surf_log_once("sel", selector);
		return kIOReturnSuccess;
	}
	case kOP2SurfaceRelease: {
		if (args->scalarInputCount < 1) {
			return kIOReturnBadArgument;
		}
		uint32_t id = (uint32_t)args->scalarInput[0];
		unsigned int index;
		IOLockLock(fLock);
		OP2SurfaceRef *ref = findRef(id, &index);
		if (ref != NULL && --ref->count == 0) {
			fRefs->removeObject(index);     /* unmaps and drops the user count */
		}
		IOLockUnlock(fLock);
		op2_surf_log_once("sel", selector);
		return ref != NULL ? kIOReturnSuccess : kIOReturnNotFound;
	}
	case kOP2SurfaceLookup: {
		OP2Surface *s = op2_surface_copy((uint32_t)arg0);
		IOReturn ret = kIOReturnNotFound;
		if (s != NULL) {
			ret = op2_surface_get(s) ? returnSurface(s, args) : kIOReturnNotFound;
			s->release();
		}
		op2_surf_log_once("sel", selector);
		return ret;
	}
	case kOP2SurfaceLookupFromPort: {
		OSObject *obj = NULL;
		IOReturn ret = IOUserClient::copyObjectForPortNameInTask(fTask, (mach_port_name_t)arg0, &obj);
		OP2Surface *s = OSDynamicCast(OP2Surface, obj);
		if (ret == kIOReturnSuccess) {
			ret = (s != NULL && op2_surface_get(s)) ? returnSurface(s, args) : kIOReturnBadArgument;
		}
		OSSafeReleaseNULL(obj);
		op2_surf_log_once("sel", selector);
		return ret;
	}
	case kOP2SurfaceCreatePort: {
		OP2Surface *s = op2_surface_copy((uint32_t)arg0);
		if (s == NULL || args->scalarOutputCount < 1) {
			OSSafeReleaseNULL(s);
			return kIOReturnNotFound;
		}
		args->scalarOutput[0] = op2_surface_port_in_task(s, fTask);
		args->scalarOutputCount = 1;
		s->release();
		op2_surf_log_once("sel", selector);
		return kIOReturnSuccess;
	}
	default:
		/* Values, notifications, ownership, timestamps: accept and ignore. */
		for (uint32_t i = 0; i < args->scalarOutputCount; i++) {
			args->scalarOutput[i] = 0;
		}
		if (args->structureOutput != NULL) {
			bzero(args->structureOutput, args->structureOutputSize);
		}
		op2_surf_log_once("sel", selector);
		return kIOReturnSuccess;
	}
}

IOReturn
IOSurfaceRootUserClient::trapUseCount(__unused void *id, __unused void *category,
    void *, void *, void *, void *)
{
	return kIOReturnSuccess;
}

static IOReturn
op2_surface_seed_out(OP2Surface *s, void *seedOut, bool bump)
{
	uint32_t seed = bump ? OSIncrementAtomic((volatile SInt32 *)op2_surface_seed(s)) + 1 :
	    *op2_surface_seed(s);
	if (seedOut != NULL && copyout(&seed, (user_addr_t)seedOut, sizeof(seed)) != 0) {
		return kIOReturnBadArgument;
	}
	return kIOReturnSuccess;
}

/* Retained surface this client created or looked up, or NULL. */
static OP2Surface *
op2_surface_held(IOLock *lock, OSArray *refs, uint32_t id)
{
	OP2Surface *s = NULL;
	IOLockLock(lock);
	for (unsigned int i = 0; i < refs->getCount(); i++) {
		OP2SurfaceRef *ref = (OP2SurfaceRef *)refs->getObject(i);
		if (ref->surface->id == id) {
			s = ref->surface;
			s->retain();
			break;
		}
	}
	IOLockUnlock(lock);
	return s;
}

IOReturn
IOSurfaceRootUserClient::trapLock(void *id, __unused void *options, void *seedOut,
    void *, void *, void *)
{
	OP2Surface *s = op2_surface_held(fLock, fRefs, (uint32_t)(uintptr_t)id);
	if (s == NULL) {
		return kIOReturnNotFound;
	}
	IOReturn ret = op2_surface_seed_out(s, seedOut, false);
	s->release();
	return ret;
}

IOReturn
IOSurfaceRootUserClient::trapUnlock(void *id, void *options, void *seedOut,
    void *, void *, void *)
{
	OP2Surface *s = op2_surface_held(fLock, fRefs, (uint32_t)(uintptr_t)id);
	if (s == NULL) {
		return kIOReturnNotFound;
	}
	IOReturn ret = op2_surface_seed_out(s, seedOut,
	    !((uintptr_t)options & kIOSurfaceLockReadOnly));
	s->release();
	return ret;
}

IOReturn
IOSurfaceRootUserClient::trapRelease(void *id, void *, void *, void *, void *, void *)
{
	unsigned int index;
	IOLockLock(fLock);
	OP2SurfaceRef *ref = findRef((uint32_t)(uintptr_t)id, &index);
	if (ref != NULL && --ref->count == 0) {
		fRefs->removeObject(index);     /* unmaps and drops the user count */
	}
	IOLockUnlock(fLock);
	return ref != NULL ? kIOReturnSuccess : kIOReturnNotFound;
}

static IOExternalTrap op2_surface_traps[] = {
	[kOP2SurfaceTrapIncrementUse]  = { NULL, (IOTrap) & IOSurfaceRootUserClient::trapUseCount },
	[kOP2SurfaceTrapDecrementUse]  = { NULL, (IOTrap) & IOSurfaceRootUserClient::trapUseCount },
	[kOP2SurfaceTrapLock]          = { NULL, (IOTrap) & IOSurfaceRootUserClient::trapLock },
	[kOP2SurfaceTrapUnlock]        = { NULL, (IOTrap) & IOSurfaceRootUserClient::trapUnlock },
	[kOP2SurfaceTrapRelease]       = { NULL, (IOTrap) & IOSurfaceRootUserClient::trapRelease },
	[kOP2SurfaceTrapReleaseFailed] = { NULL, (IOTrap) & IOSurfaceRootUserClient::trapRelease },
};

IOExternalTrap *
IOSurfaceRootUserClient::getTargetAndTrapForIndex(IOService **targetP, UInt32 index)
{
	if (index >= sizeof(op2_surface_traps) / sizeof(op2_surface_traps[0])) {
		op2_surf_log_once("trap", index);
		return NULL;
	}
	op2_surf_log_once("trap", index);
	*targetP = this;
	return &op2_surface_traps[index];
}
#endif /* ARM64_BOARD_CONFIG_MSM8994 */
