// OP2GPU's user client: the requests as struct-method calls whose selector is the request number, a buffer's memory
// as the client memory of the type that is its handle. Adapted from hakios-metal's src/iokit_transport.cpp
// (MIT, Copyright (c) 2025-2026 the HakiOS authors; see LICENSE-hakios).
#include <mach/mach.h>
#include <mach/mach_time.h>

#if __has_include(<IOKit/IOKitLib.h>)
#include <IOKit/IOKitLib.h>
#else
// The iPhoneOS SDK ships IOKit.tbd without its headers: the few calls used here, as IOKitLib.h declares them.
#include <CoreFoundation/CoreFoundation.h>
extern "C" {
typedef mach_port_t io_object_t;
typedef io_object_t io_service_t;
typedef io_object_t io_connect_t;
extern const mach_port_t kIOMasterPortDefault;
CFMutableDictionaryRef IOServiceMatching(const char* name);
io_service_t IOServiceGetMatchingService(mach_port_t masterPort, CFDictionaryRef matching);
kern_return_t IOServiceOpen(io_service_t service, task_port_t owningTask, uint32_t type, io_connect_t* connect);
kern_return_t IOServiceClose(io_connect_t connect);
kern_return_t IOObjectRelease(io_object_t object);
kern_return_t IOConnectCallStructMethod(io_connect_t connection, uint32_t selector, const void* inputStruct,
                                        size_t inputStructCnt, void* outputStruct, size_t* outputStructCnt);
kern_return_t IOConnectMapMemory64(io_connect_t connect, uint32_t memoryType, task_port_t intoTask,
                                   mach_vm_address_t* atAddress, mach_vm_size_t* ofSize, uint32_t options);
kern_return_t IOConnectUnmapMemory64(io_connect_t connect, uint32_t memoryType, task_port_t fromTask,
                                     mach_vm_address_t atAddress);
}
#define kIOMapAnywhere 0x00000001
#define kIOReturnNoMemory ((kern_return_t)0xe00002bd)
#define kIOReturnBadArgument ((kern_return_t)0xe00002c2)
#define kIOReturnTimeout ((kern_return_t)0xe00002d6)
#define kIOReturnBusy ((kern_return_t)0xe00002d5)
#define kIOReturnNotFound ((kern_return_t)0xe00002f0)
#define kIOReturnUnsupported ((kern_return_t)0xe00002c7)
#define kIOReturnNotReady ((kern_return_t)0xe00002d8)
#define kIOReturnNoSpace ((kern_return_t)0xe00002db)
#endif

#include <cerrno>
#include <cstdio>

#include "op2gpu/gpu.hpp"

namespace op2gpu {

namespace {

class KernelGpu : public Transport {
public:
    explicit KernelGpu(io_connect_t c) : c_(c) { mach_timebase_info(&timebase_); }
    ~KernelGpu() override { IOServiceClose(c_); }

    int call(uint32_t request, void* data, size_t size) override {
        size_t returned = size;
        // kLog has no structure to give back
        kern_return_t r = request == kLog ? IOConnectCallStructMethod(c_, request, data, size, nullptr, nullptr)
                                          : IOConnectCallStructMethod(c_, request, data, size, data, &returned);
        switch (r) {
        case KERN_SUCCESS: return 0;
        case kIOReturnNoMemory: return ENOMEM;
        case kIOReturnBadArgument: return EINVAL;
        case kIOReturnTimeout: return ETIMEDOUT;
        case kIOReturnBusy: return EBUSY;
        case kIOReturnNotFound: return ENOENT;
        case kIOReturnUnsupported: return ENOSYS;
        case kIOReturnNotReady: return EAGAIN;
        case kIOReturnNoSpace: return ENOSPC;
        default:
            fprintf(stderr, "op2gpu: request 0x%x answered 0x%x\n", request, unsigned(r));
            return EIO;
        }
    }

    void* map(uint32_t handle, uint64_t) override {
        mach_vm_address_t address = 0;
        mach_vm_size_t size = 0;
        if (IOConnectMapMemory64(c_, handle, mach_task_self(), &address, &size, kIOMapAnywhere) != KERN_SUCCESS) {
            return nullptr;
        }
        return reinterpret_cast<void*>(uintptr_t(address));
    }

    void unmap(uint32_t handle, void* address) override {
        IOConnectUnmapMemory64(c_, handle, mach_task_self(), mach_vm_address_t(uintptr_t(address)));
    }

    int64_t nowNs() override {
        return int64_t(mach_continuous_time() * timebase_.numer / timebase_.denom);
    }

private:
    io_connect_t c_;
    mach_timebase_info_data_t timebase_{};
};

}  // namespace

std::unique_ptr<Transport> openKernelGpu(std::string* why) {
    io_service_t s = IOServiceGetMatchingService(kIOMasterPortDefault, IOServiceMatching("OP2GPU"));
    if (s == 0) {
        *why = "no OP2GPU service (gpu=0, or the kernel did not start the GPU)";
        return nullptr;
    }
    io_connect_t c = 0;
    kern_return_t r = IOServiceOpen(s, mach_task_self(), 0, &c);
    IOObjectRelease(s);
    if (r != KERN_SUCCESS) {
        *why = "OP2GPU would not open: error " + std::to_string(r);
        return nullptr;
    }
    return std::unique_ptr<Transport>(new KernelGpu(c));
}

}  // namespace op2gpu
