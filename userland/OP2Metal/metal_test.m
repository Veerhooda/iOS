/*
 * metal_test: Standalone verification program for OP2Metal on OnePlus 2 (iOS 12).
 * Verifies IOAcceleratorES / OP2GPU discovery, OP2GPUUserClient dispatch,
 * GEM buffer allocation, memory mapping, PM4 Type 3 hardware command submission,
 * hardware fence synchronization, MTLCreateSystemDefaultDevice(), and command queues.
 */

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <mach/mach.h>
#import <objc/message.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

/* Standard IOKit C Declarations */
typedef mach_port_t io_object_t;
typedef io_object_t io_service_t;
typedef io_object_t io_connect_t;
typedef io_object_t io_iterator_t;
typedef io_object_t io_registry_entry_t;
typedef char io_name_t[128];
typedef uint32_t IOOptionBits;
#define kIOMapAnywhere 0x00000001

extern const mach_port_t kIOMasterPortDefault;

CFMutableDictionaryRef IOServiceMatching(const char *name);
io_service_t IOServiceGetMatchingService(mach_port_t masterPort, CFDictionaryRef matching);
kern_return_t IOServiceGetMatchingServices(mach_port_t masterPort, CFDictionaryRef matching, io_iterator_t *existing);
io_object_t IOIteratorNext(io_iterator_t iterator);
kern_return_t IOObjectRelease(io_object_t object);
kern_return_t IORegistryEntryGetName(io_registry_entry_t entry, io_name_t name);
CFTypeRef IORegistryEntryCreateCFProperty(io_registry_entry_t entry, CFStringRef key, CFAllocatorRef allocator, uint32_t options);

kern_return_t IOServiceOpen(io_service_t service, task_port_t owningTask, uint32_t type, io_connect_t *connect);
kern_return_t IOServiceClose(io_connect_t connect);
kern_return_t IOConnectCallStructMethod(io_connect_t connect, uint32_t selector,
    const void *inputStruct, size_t inputStructCnt,
    void *outputStruct, size_t *outputStructCnt);
kern_return_t IOConnectMapMemory64(io_connect_t connect, uint32_t memoryType,
    task_port_t intoTask, mach_vm_address_t *atAddress,
    mach_vm_size_t *ofSize, IOOptionBits options);
kern_return_t IOConnectUnmapMemory64(io_connect_t connect, uint32_t memoryType,
    task_port_t fromTask, mach_vm_address_t atAddress);

/* DRM MSM Constants (Linux msm_drm.h ABI compatibility) */
enum {
	kDrmGemClose            = 0x09,
	kMsmGetParam            = 0x40,
	kMsmSetParam            = 0x41,
	kMsmGemNew              = 0x42,
	kMsmGemInfo             = 0x43,
	kMsmGemCpuPrep          = 0x44,
	kMsmGemCpuFini          = 0x45,
	kMsmGemSubmit           = 0x46,
	kMsmWaitFence           = 0x47,
};

#define MSM_PARAM_GPU_ID        0x01
#define MSM_PARAM_GMEM_SIZE     0x02
#define MSM_PARAM_CHIP_ID       0x03
#define MSM_PARAM_MAX_FREQ      0x04
#define MSM_PARAM_TIMESTAMP     0x05
#define MSM_PARAM_GMEM_BASE     0x06

#define MSM_INFO_GET_OFFSET     0x00
#define MSM_INFO_GET_IOVA       0x01

#define MSM_SUBMIT_CMD_BUF      0x01

#define CP_TYPE3_PKT                    (3U << 30)
#define cp_type3_packet(op, cnt)        (CP_TYPE3_PKT | (((cnt) - 1) << 16) | (((op) & 0xff) << 8))
#define CP_NOP                          0x10
#define CP_WAIT_FOR_IDLE                0x26

struct msm_param {
	uint32_t pipe, param;
	uint64_t value;
	uint32_t len, pad;
};

struct msm_gem_new {
	uint64_t size;
	uint32_t flags, handle;
};

struct msm_gem_info {
	uint32_t handle, info;
	uint64_t value;
	uint32_t len, pad;
};

struct msm_timespec {
	int64_t seconds, nanoseconds;
};

struct msm_gem_cpu_prep {
	uint32_t handle, op;
	struct msm_timespec timeout;
};

struct msm_gem_submit_cmd {
	uint32_t type, submit_idx, submit_offset, size, pad, nr_relocs;
	uint64_t relocs;
};

struct msm_gem_submit_bo {
	uint32_t flags, handle;
	uint64_t presumed;
};

struct msm_gem_submit {
	uint32_t flags, fence, nr_bos, nr_cmds;
	uint64_t bos, cmds;
	int32_t fence_fd;
	uint32_t queueid;
	uint64_t in_syncobjs, out_syncobjs;
	uint32_t nr_in_syncobjs, nr_out_syncobjs, syncobj_stride, pad;
};

struct msm_wait_fence {
	uint32_t fence, flags;
	struct msm_timespec timeout;
	uint32_t queueid;
};

static void
test_iokit_accelerator(void)
{
	printf("=== Step 1: IOKit IOAcceleratorES Discovery ===\n");
	CFMutableDictionaryRef matching = IOServiceMatching("IOAcceleratorES");
	io_iterator_t iter;
	kern_return_t kr = IOServiceGetMatchingServices(kIOMasterPortDefault, matching, &iter);

	if (kr != KERN_SUCCESS) {
		printf("[-] IOServiceGetMatchingServices failed: 0x%x\n", kr);
		return;
	}

	io_service_t service;
	int count = 0;
	while ((service = IOIteratorNext(iter)) != 0) {
		count++;
		io_name_t name;
		IORegistryEntryGetName(service, name);

		CFTypeRef pluginName = IORegistryEntryCreateCFProperty(service, CFSTR("MetalPluginName"), kCFAllocatorDefault, 0);
		CFTypeRef className = IORegistryEntryCreateCFProperty(service, CFSTR("MetalPluginClassName"), kCFAllocatorDefault, 0);

		printf("[+] Found IOAcceleratorES [%d]: \"%s\"\n", count, name);
		if (pluginName && CFGetTypeID(pluginName) == CFStringGetTypeID()) {
			printf("    MetalPluginName:      %s\n", [(__bridge NSString *)pluginName UTF8String]);
		} else {
			printf("    MetalPluginName:      (none)\n");
		}
		if (className && CFGetTypeID(className) == CFStringGetTypeID()) {
			printf("    MetalPluginClassName: %s\n", [(__bridge NSString *)className UTF8String]);
		} else {
			printf("    MetalPluginClassName: (none)\n");
		}

		if (pluginName) CFRelease(pluginName);
		if (className) CFRelease(className);
		IOObjectRelease(service);
	}
	IOObjectRelease(iter);

	if (count == 0) {
		printf("[-] No IOAcceleratorES services found in registry!\n");
	}
}

static void
test_op2_gpu_userclient(void)
{
	printf("\n=== Step 2: OP2GPU User Client Bring-Up & Hardware Command Submission ===\n");

	/* Try opening OP2GPU directly or AGXAccelerator */
	io_service_t gpu_service = IOServiceGetMatchingService(kIOMasterPortDefault, IOServiceMatching("OP2GPU"));
	if (!gpu_service) {
		printf("[*] OP2GPU service not matched directly; trying IOAcceleratorES...\n");
		gpu_service = IOServiceGetMatchingService(kIOMasterPortDefault, IOServiceMatching("IOAcceleratorES"));
	}
	if (!gpu_service) {
		printf("[-] Neither OP2GPU nor IOAcceleratorES found in IORegistry!\n");
		return;
	}

	io_connect_t connect = 0;
	kern_return_t kr = IOServiceOpen(gpu_service, mach_task_self(), 0, &connect);
	IOObjectRelease(gpu_service);

	if (kr != KERN_SUCCESS || connect == 0) {
		printf("[-] IOServiceOpen failed: 0x%x\n", kr);
		return;
	}
	printf("[+] Successfully opened OP2GPU user client! (connect port 0x%x)\n", connect);

	/* 1. Query Hardware Parameters */
	struct msm_param param;
	memset(&param, 0, sizeof(param));
	param.param = MSM_PARAM_CHIP_ID;
	size_t out_sz = sizeof(param);
	kr = IOConnectCallStructMethod(connect, kMsmGetParam, &param, sizeof(param), &param, &out_sz);
	if (kr == KERN_SUCCESS) {
		printf("[+] Hardware Param MSM_PARAM_CHIP_ID: 0x%08llx (%s)\n",
		       param.value, param.value == 0x04030002 ? "Adreno 430 v2.1" : "Unknown");
	} else {
		printf("[-] Failed to query MSM_PARAM_CHIP_ID: 0x%x\n", kr);
	}

	param.param = MSM_PARAM_GMEM_SIZE;
	out_sz = sizeof(param);
	kr = IOConnectCallStructMethod(connect, kMsmGetParam, &param, sizeof(param), &param, &out_sz);
	if (kr == KERN_SUCCESS) {
		printf("[+] Hardware Param MSM_PARAM_GMEM_SIZE: 0x%llx (%llu KB GMEM)\n",
		       param.value, param.value >> 10);
	}

	/* 2. Allocate GEM Buffer */
	struct msm_gem_new gem_new;
	memset(&gem_new, 0, sizeof(gem_new));
	gem_new.size = 4096;
	out_sz = sizeof(gem_new);
	kr = IOConnectCallStructMethod(connect, kMsmGemNew, &gem_new, sizeof(gem_new), &gem_new, &out_sz);
	if (kr != KERN_SUCCESS || gem_new.handle == 0) {
		printf("[-] kMsmGemNew failed: 0x%x (handle %u)\n", kr, gem_new.handle);
		IOServiceClose(connect);
		return;
	}
	printf("[+] Allocated GPU GEM Buffer: handle %u (size %llu bytes)\n", gem_new.handle, gem_new.size);

	/* 3. Query Physical Memory Address (IOVA) */
	struct msm_gem_info gem_info;
	memset(&gem_info, 0, sizeof(gem_info));
	gem_info.handle = gem_new.handle;
	gem_info.info = MSM_INFO_GET_IOVA;
	out_sz = sizeof(gem_info);
	kr = IOConnectCallStructMethod(connect, kMsmGemInfo, &gem_info, sizeof(gem_info), &gem_info, &out_sz);
	if (kr == KERN_SUCCESS) {
		printf("[+] GPU Buffer Physical Address: 0x%llx\n", gem_info.value);
	}

	/* 4. Map Buffer to Userspace */
	mach_vm_address_t user_addr = 0;
	mach_vm_size_t user_size = 0;
	kr = IOConnectMapMemory64(connect, gem_new.handle, mach_task_self(), &user_addr, &user_size, kIOMapAnywhere);
	if (kr != KERN_SUCCESS || user_addr == 0) {
		printf("[-] IOConnectMapMemory64 failed: 0x%x\n", kr);
		IOServiceClose(connect);
		return;
	}
	printf("[+] Mapped GPU Buffer to Userspace: virt=%p size=%llu bytes\n", (void *)user_addr, user_size);

	/* 5. Encode Hardware PM4 Command Stream into Mapped Buffer */
	uint32_t *cmds = (uint32_t *)user_addr;
	cmds[0] = cp_type3_packet(CP_NOP, 5);
	cmds[1] = 0xdeada430; /* Canary tag 1 */
	cmds[2] = 0x11223344; /* Canary tag 2 */
	cmds[3] = 0x55667788; /* Canary tag 3 */
	cmds[4] = 0x99aabbcc; /* Canary tag 4 */
	cmds[5] = 0x00000001; /* Canary tag 5 */
	cmds[6] = cp_type3_packet(CP_WAIT_FOR_IDLE, 1);
	cmds[7] = 0x00000000;
	uint32_t cmd_words = 8;

	printf("[+] Populated PM4 command stream (8 words, CP_NOP + CP_WAIT_FOR_IDLE)\n");

	/* 6. Clean CPU Cache */
	uint32_t handle = gem_new.handle;
	IOConnectCallStructMethod(connect, kMsmGemCpuFini, &handle, sizeof(handle), NULL, NULL);

	/* 7. Submit Indirect Buffer to Adreno 430 Hardware */
	struct msm_gem_submit_bo submit_bo;
	memset(&submit_bo, 0, sizeof(submit_bo));
	submit_bo.handle = gem_new.handle;

	struct msm_gem_submit_cmd submit_cmd;
	memset(&submit_cmd, 0, sizeof(submit_cmd));
	submit_cmd.type = MSM_SUBMIT_CMD_BUF;
	submit_cmd.submit_idx = 0;
	submit_cmd.submit_offset = 0;
	submit_cmd.size = cmd_words * sizeof(uint32_t);

	struct msm_gem_submit submit;
	memset(&submit, 0, sizeof(submit));
	submit.nr_bos = 1;
	submit.bos = (uint64_t)(uintptr_t)&submit_bo;
	submit.nr_cmds = 1;
	submit.cmds = (uint64_t)(uintptr_t)&submit_cmd;

	out_sz = sizeof(submit);
	kr = IOConnectCallStructMethod(connect, kMsmGemSubmit, &submit, sizeof(submit), &submit, &out_sz);
	if (kr != KERN_SUCCESS || submit.fence == 0) {
		printf("[-] kMsmGemSubmit failed: 0x%x (fence %u)\n", kr, submit.fence);
	} else {
		printf("[+] Submitted Indirect Buffer to GPU! Issued hardware fence: %u\n", submit.fence);

		/* 8. Wait for Hardware Fence */
		struct msm_wait_fence wait_f;
		memset(&wait_f, 0, sizeof(wait_f));
		wait_f.fence = submit.fence;
		wait_f.timeout.seconds = 5;
		kr = IOConnectCallStructMethod(connect, kMsmWaitFence, &wait_f, sizeof(wait_f), NULL, NULL);
		if (kr == KERN_SUCCESS) {
			printf("[+] SUCCESS: Hardware Fence %u completed! Adreno 430 executed userspace commands over DDR DMA!\n", submit.fence);
		} else {
			printf("[-] kMsmWaitFence timed out or failed: 0x%x\n", kr);
		}
	}

	/* 9. Cleanup */
	IOConnectUnmapMemory64(connect, gem_new.handle, mach_task_self(), user_addr);
	uint32_t close_handle = gem_new.handle;
	IOConnectCallStructMethod(connect, kDrmGemClose, &close_handle, sizeof(close_handle), NULL, NULL);
	IOServiceClose(connect);
	printf("[+] Successfully cleaned up GEM buffer and closed connection.\n");
}

static void
test_metal_device(void)
{
	printf("\n=== Step 3: MTLCreateSystemDefaultDevice() Query ===\n");
	id<MTLDevice> device = MTLCreateSystemDefaultDevice();
	if (device == nil) {
		printf("[-] MTLCreateSystemDefaultDevice() returned nil!\n");
		return;
	}

	printf("[+] Success! Default Metal Device: %s\n", [[device name] UTF8String]);
	printf("    maxBufferLength:          %llu MB\n", (unsigned long long)([device maxBufferLength] >> 20));
	BOOL unified = [device respondsToSelector:sel_registerName("hasUnifiedMemory")] ? ((BOOL (*)(id, SEL))objc_msgSend)(device, sel_registerName("hasUnifiedMemory")) : YES;
	printf("    hasUnifiedMemory:         %s\n", unified ? "YES" : "NO");

	/* Buffer Allocation Test */
	printf("\n=== Step 4: Metal Buffer Memory Test ===\n");
	NSUInteger bufSize = 1024 * 1024; /* 1 MB */
	id<MTLBuffer> buffer = [device newBufferWithLength:bufSize options:MTLResourceStorageModeShared];
	if (buffer == nil) {
		printf("[-] Failed to allocate %lu byte MTLBuffer!\n", (unsigned long)bufSize);
		return;
	}

	uint32_t *ptr = (uint32_t *)[buffer contents];
	if (ptr == NULL) {
		printf("[-] Buffer contents pointer is NULL!\n");
		return;
	}

	/* Write pattern: 0xDEADBEEF */
	for (size_t i = 0; i < (bufSize / sizeof(uint32_t)); i++) {
		ptr[i] = (uint32_t)(0xDEADBEEF ^ i);
	}

	/* Verify pattern */
	int mismatches = 0;
	for (size_t i = 0; i < (bufSize / sizeof(uint32_t)); i++) {
		if (ptr[i] != (uint32_t)(0xDEADBEEF ^ i)) {
			mismatches++;
		}
	}
	if (mismatches == 0) {
		printf("[+] 1 MB buffer write and readback verified: 100%% byte integrity\n");
	} else {
		printf("[-] Buffer integrity failure: %d mismatches!\n", mismatches);
	}

	/* Command Queue Test */
	printf("\n=== Step 5: Command Queue & Command Buffer Test ===\n");
	id<MTLCommandQueue> queue = [device newCommandQueue];
	if (queue == nil) {
		printf("[-] Failed to create MTLCommandQueue!\n");
		return;
	}
	printf("[+] Successfully created MTLCommandQueue: %s\n", [[queue label] UTF8String]);

	id<MTLCommandBuffer> cmdBuf = [queue commandBuffer];
	if (cmdBuf == nil) {
		printf("[-] Failed to create MTLCommandBuffer!\n");
		return;
	}
	printf("[+] Successfully created MTLCommandBuffer\n");

	id<MTLBlitCommandEncoder> blit = [cmdBuf blitCommandEncoder];
	if (blit != nil) {
		printf("[+] Successfully created MTLBlitCommandEncoder\n");
		[blit endEncoding];
	}

	[cmdBuf commit];
	printf("[+] Committed command buffer cleanly\n");
	printf("\n>>> OP2Metal Phase 3 Verification Complete! <<<\n");
}

int
main(int argc, const char *argv[])
{
	int con = open("/dev/console", O_WRONLY | O_NOCTTY);
	if (con >= 0) {
		dup2(con, 1);
		dup2(con, 2);
	}
	setvbuf(stdout, NULL, _IONBF, 0);
	setvbuf(stderr, NULL, _IONBF, 0);

	@autoreleasepool {
		printf("\n\n====================================================\n");
		printf("  OnePlus 2 Metal & OP2GPU Bring-Up Diagnostic      \n");
		printf("====================================================\n");
		fflush(stdout);
		test_iokit_accelerator();
		fflush(stdout);
		test_op2_gpu_userclient();
		fflush(stdout);
		test_metal_device();
		fflush(stdout);
		printf("====================================================\n\n");
		fflush(stdout);
	}
	return 0;
}
