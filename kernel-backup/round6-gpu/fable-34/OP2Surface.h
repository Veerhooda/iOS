/*
 * Built-in IOSurface for OnePlus 2 (MSM8994).
 *
 * Shared with OP2Framebuffer.cpp: the framebuffer looks surfaces up by
 * id when a swap names them and publishes the panel memory itself as the
 * default framebuffer surface.
 */
#ifndef _OP2_SURFACE_H
#define _OP2_SURFACE_H

#include <pexpert/arm64/board_config.h>

#if defined(ARM64_BOARD_CONFIG_MSM8994)
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <libkern/c++/OSObject.h>
#include <mach/mach_types.h>

#define OP2_SURFACE_MAX_PLANES        20
#define OP2_SURFACE_MAX_ID            4096

struct op2_surface_plane {
	uint32_t width, height, bytesPerRow, offset, size;
	uint16_t bytesPerElement;
	uint8_t  elementWidth, elementHeight;
};

class OP2Surface : public OSObject {
	OSDeclareDefaultStructors(OP2Surface);

public:
	uint32_t id;
	uint32_t width, height, bytesPerRow, offset, pixelFormat, allocSize;
	uint16_t bytesPerElement;
	uint8_t  elementWidth, elementHeight;
	uint32_t cacheMode, planeCount, parentID;
	struct op2_surface_plane planes[OP2_SURFACE_MAX_PLANES];

	IOMemoryDescriptor       *memory;       /* pixels (buffer or physical) */
	void                     *kernelBuffer; /* direct kernel pointer, or NULL */
	IOOptionBits              mapCache;     /* cache mode for user mappings */
	IOBufferMemoryDescriptor *state;        /* shared state page, read-only for users */
	IOMemoryMap              *kernelMap;    /* lazily created for kernel copies */
	uint64_t                  poolOff, poolLen;   /* G5c: pixels from OP2GPU's boot-time pool (poolLen 0: not) */
	OSDictionary             *values;       /* IOSurfaceSetValue(s): sel 9 stores, sel 10 copies (NULL: none yet) */

	bool init() APPLE_KEXT_OVERRIDE { return OSObject::init(); }
	void free() APPLE_KEXT_OVERRIDE;

	/* Kernel address of the pixels (mapped on first use), or 0. */
	uintptr_t kernelAddress();
};

/* Retained surface for an id, or NULL. */
OP2Surface *op2_surface_copy(uint32_t id);

/* Get surface seed counter from shared state page */
uint32_t op2_surface_get_seed(OP2Surface *s);

/*
 * Publish physical memory (the panel framebuffer) as a surface. Returns a
 * retained surface that stays in the id table until released by the caller
 * and every user.
 */
OP2Surface *op2_surface_create_physical(uint64_t phys, uint32_t width,
    uint32_t height, uint32_t bytesPerRow, uint32_t pixelFormat);

/* Phase 8 MDP5 Direct Scanout Carveout Allocator */
#define OP2_CARVEOUT_BASE             0x03400000ULL
#define OP2_CARVEOUT_SIZE             (34ULL << 20) /* 34 MiB reserved by LK (up to 0x05600000) */
#define OP2_CARVEOUT_SLOT0_SIZE       (8ULL << 20)  /* 8 MiB for AppleCLCD::fSurface (1080x1920) */
#define OP2_CARVEOUT_SLOT_SIZE        (12ULL << 20) /* 12 MiB per display buffer slot (1242x2208 = 11.3 MiB) */
#define OP2_CARVEOUT_MAX_SLOTS        3             /* Slot 0 (fSurface), Slot 1, Slot 2 */

uint64_t op2_carveout_alloc_slot(uint32_t size);
void op2_carveout_free_slot(uint64_t phys);
extern "C" bool op2_gpu_pool_take(uint64_t bytes, IOMemoryDescriptor **desc, void **kaddr, uint64_t *off, uint64_t *len);
extern "C" void op2_gpu_pool_give(uint64_t off, uint64_t len);

/* Send right for the surface in task (IOSurfaceLookupFromMachPort). */
mach_port_name_t op2_surface_port_in_task(OP2Surface *surface, task_t task);

#endif /* ARM64_BOARD_CONFIG_MSM8994 */
#endif /* _OP2_SURFACE_H */
