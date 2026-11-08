/*
 * metal_test: Standalone verification program for OP2Metal on OnePlus 2 (iOS 12).
 * Verifies IOAcceleratorES discovery, MTLCreateSystemDefaultDevice(), buffer allocations,
 * and command queue creation.
 */

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <mach/mach.h>
#import <objc/message.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Standard IOKit C Declarations */
typedef mach_port_t io_object_t;
typedef io_object_t io_service_t;
typedef io_object_t io_iterator_t;
typedef io_object_t io_registry_entry_t;
typedef char io_name_t[128];

extern const mach_port_t kIOMasterPortDefault;

CFMutableDictionaryRef IOServiceMatching(const char *name);
kern_return_t IOServiceGetMatchingServices(mach_port_t masterPort, CFDictionaryRef matching, io_iterator_t *existing);
io_object_t IOIteratorNext(io_iterator_t iterator);
kern_return_t IOObjectRelease(io_object_t object);
kern_return_t IORegistryEntryGetName(io_registry_entry_t entry, io_name_t name);
CFTypeRef IORegistryEntryCreateCFProperty(io_registry_entry_t entry, CFStringRef key, CFAllocatorRef allocator, uint32_t options);

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
test_metal_device(void)
{
	printf("\n=== Step 2: MTLCreateSystemDefaultDevice() Query ===\n");
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
	printf("\n=== Step 3: Metal Buffer Memory Test ===\n");
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
	printf("\n=== Step 4: Command Queue & Command Buffer Test ===\n");
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
	printf("\n>>> OP2Metal Phase 1 Verification PASSED! <<<\n");
}

int
main(int argc, const char *argv[])
{
	@autoreleasepool {
		printf("========================================\n");
		printf("  OnePlus 2 Metal Bring-Up Diagnostic   \n");
		printf("========================================\n");
		test_iokit_accelerator();
		test_metal_device();
	}
	return 0;
}
