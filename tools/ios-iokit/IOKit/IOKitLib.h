/* The part of IOKitLib.h a user client needs, for the iPhoneOS SDK (which ships IOKit.tbd and no headers).
 * Declarations as in Apple's IOKitLib.h / IOReturn.h / IOTypes.h. */
#ifndef OP2_IOKITLIB_H
#define OP2_IOKITLIB_H
#include <mach/mach.h>
#include <CoreFoundation/CoreFoundation.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef mach_port_t io_object_t;
typedef io_object_t io_service_t;
typedef io_object_t io_connect_t;
typedef kern_return_t IOReturn;
extern const mach_port_t kIOMasterPortDefault;
CFMutableDictionaryRef IOServiceMatching(const char *name);
io_service_t IOServiceGetMatchingService(mach_port_t masterPort, CFDictionaryRef matching);
kern_return_t IOServiceOpen(io_service_t service, task_port_t owningTask, uint32_t type, io_connect_t *connect);
kern_return_t IOServiceClose(io_connect_t connect);
kern_return_t IOObjectRelease(io_object_t object);
kern_return_t IOConnectCallStructMethod(io_connect_t connection, uint32_t selector, const void *inputStruct,
                                        size_t inputStructCnt, void *outputStruct, size_t *outputStructCnt);
kern_return_t IOConnectCallScalarMethod(io_connect_t connection, uint32_t selector, const uint64_t *input,
                                        uint32_t inputCnt, uint64_t *output, uint32_t *outputCnt);
kern_return_t IOConnectMapMemory64(io_connect_t connect, uint32_t memoryType, task_port_t intoTask,
                                   mach_vm_address_t *atAddress, mach_vm_size_t *ofSize, uint32_t options);
kern_return_t IOConnectUnmapMemory64(io_connect_t connect, uint32_t memoryType, task_port_t fromTask,
                                     mach_vm_address_t atAddress);
#ifdef __cplusplus
}
#endif
#define kIOMapAnywhere           0x00000001
#define kIOReturnSuccess         KERN_SUCCESS
#define kIOReturnNoMemory        ((IOReturn)0xe00002bd)
#define kIOReturnBadArgument     ((IOReturn)0xe00002c2)
#define kIOReturnUnsupported     ((IOReturn)0xe00002c7)
#define kIOReturnBusy            ((IOReturn)0xe00002d5)
#define kIOReturnTimeout         ((IOReturn)0xe00002d6)
#define kIOReturnNotReady        ((IOReturn)0xe00002d8)
#define kIOReturnNoSpace         ((IOReturn)0xe00002db)
#define kIOReturnNotFound        ((IOReturn)0xe00002f0)
#endif
