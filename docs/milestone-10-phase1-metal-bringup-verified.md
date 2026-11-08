# Milestone 10: Phase 1 Userland Metal Shim & IOAcceleratorES Bring-Up

**Date:** November 08, 2026  
**Target:** OnePlus 2 (`ONE A2003`, Qualcomm MSM8994 / Snapdragon 810 v2.1)  
**Kernel:** XNU 4903.270.47 (`DEVELOPMENT_ARM64_MSM8994`)  
**OS:** Apple iOS 12.5.7 (`16H81`, `n56ap` iPhone 6 Plus profile)  
**Status:** **VERIFIED & DEPLOYED**

---

## 1. Executive Summary & Problem Solved

With SMP Dual-Core (`cpus=2`) operational and Milestone 9 physical display continuity verified, the primary remaining blocker to running user-facing iOS 12 applications (Camera, Safari, Photos, Maps) was the **graphics acceleration contract**:

1. **Null `MTLDevice` Crash Loop:** On standard iOS 12 userland, applications invoke `MTLCreateSystemDefaultDevice()`. Because no graphics accelerator driver was published in IOKit, this API returned `NULL`, triggering immediate `SIGSEGV` fatal null-pointer dereferences across application threads.
2. **Context Assertion Failures:** `backboardd` and `SpringBoard` repeatedly issued matching queries:
   ```text
   IOK backboardd: matching1 <dict ID="0"><key>IOProviderClass</key><string ID="1">AGXAccelerator</string></dict> -> none
   CoreAnimation: Software renderer! Simulating crash!
   CoreAnimation: Failed to initialize Metal or OpenGLES contexts!
   ```
3. **Unimplemented Selector Panics:** Naive Metal stubs crash when private frameworks probe undocumented internal SPI selectors without valid signatures.

Phase 1 establishes the **Userland Crash Hardening & Metal Shim Layer**, completely satisfying Apple's IOKit and Objective-C runtime contracts to eliminate null-device crashes while allowing `backboardd` to retain stable CPU/OpenGL ES rendering.

---

## 2. iOS 12 Metal Device Discovery Chain

On Darwin/iOS, Metal device discovery proceeds through an explicit 5-step handshake:

```mermaid
flowchart TD
    A["Process invokes 'MTLCreateSystemDefaultDevice()'"] --> B["'MTLRegisterDevices()' in Metal.framework"]
    B --> C["IOKit Query: 'IOServiceMatching(\"IOAcceleratorES\")'"]
    C --> D["Kernel matches 'AGXAccelerator' (subclass of 'IOAcceleratorES')"]
    D --> E["Reads 'MetalPluginName' ('OP2Metal') and 'MetalPluginClassName' ('OP2MetalDevice')"]
    E --> F["Loads '/System/Library/Extensions/OP2Metal.bundle'"]
    F --> G["Validates 'OP2MetalDevice' subclasses private '_MTLDevice'"]
    G --> H["Sends '-initWithAcceleratorPort:' and asserts '<MTLDeviceSPI>'"]
    H --> I["Returns singleton 'OP2MetalDevice' to Application"]
```

---

## 3. Kernel Subsystem: Dual-Class Service Architecture

In `xnu/xnu-4903.270.47/iokit/Kernel/arm/OP2Framebuffer.cpp`, we declare:

```cpp
class IOAcceleratorES : public IOService {
    OSDeclareDefaultStructors(IOAcceleratorES);
};
OSDefineMetaClassAndStructors(IOAcceleratorES, IOService);

class AGXAccelerator : public IOAcceleratorES {
    OSDeclareDefaultStructors(AGXAccelerator);
};
OSDefineMetaClassAndStructors(AGXAccelerator, IOAcceleratorES);
```

### Discovery Properties
When boot-arg `metal=N` is present (`metal != 0`), `AppleCLCD::start()` attaches `AGXAccelerator` to the framebuffer and sets:
* `MetalPluginName` = `"OP2Metal"`
* `MetalPluginClassName` = `"OP2MetalDevice"`
* `IOGLBundleName` = `"OP2Metal"`
* `IOGLPluginName` = `"OP2Metal"`

Because `AGXAccelerator` subclasses `IOAcceleratorES`:
* `IOServiceMatching("AGXAccelerator")` queries from `backboardd` and `SpringBoard` match successfully (eliminating `-> none`).
* `IOServiceMatching("IOAcceleratorES")` queries from `Metal.framework` match identically.

---

## 4. Userland Subsystem: `OP2Metal.bundle`

Located at `/System/Library/Extensions/OP2Metal.bundle`, compiled targeting `arm64-apple-ios12.0` with the `iPhoneOS12.4.sdk`.

### Key Components

| Class | Base Class | Protocols Adopted | Role |
| :--- | :--- | :--- | :--- |
| `OP2MetalDevice` | `_MTLDevice` | `<MTLDevice>`, `<MTLDeviceSPI>` | Primary device singleton; claims 256MB max buffer, unified memory, feature sets `0` and `2`. |
| `OP2MetalQueue` | `_MTLCommandQueue`| `<MTLCommandQueue>`, `MTLCommandQueueSPI` | Dispenses command buffers; no-op priority management. |
| `OP2MetalCommandBuffer`| `_MTLCommandBuffer`| `<MTLCommandBuffer>`, `MTLCommandBufferSPI` | Tracks completion callbacks; returns status completed. |
| `OP2MetalBuffer` | `_MTLResource` | `<MTLBuffer>`, `<MTLResource>` | Wraps `mmap`/process memory with 100% byte integrity. |
| `OP2MetalLibrary` | `_MTLLibrary` | `<MTLLibrary>`, `MTLLibrarySPI` | MTLB container parser (`op2_metallib.c`). |
| `OP2MetalFunction` | `_MTLFunction` | `<MTLFunction>`, `MTLFunctionSPI` | Exposes AIR bitcode functions. |
| `OP2MetalBlitEncoder` | `_MTLCommandEncoder` | `<MTLBlitCommandEncoder>` | Performs CPU-accelerated `memcpy`/`memset` blits. |

### Robust Crash Prevention: Dynamic Method Forwarding
To handle hundreds of private SPI selectors queried by internal Apple frameworks without crashing:
1. `deviceMethodSignature`: Resolves method signatures across all runtime protocols starting with `MTLDevice`, falling back to `[super methodSignatureForSelector:]`.
2. `deviceForwardInvocation`: Allocates a zeroed return buffer matching `methodReturnLength` and logs the selector once via `NSLog`.
3. `targetDeviceInfo`: Answers with a statically allocated `struct MTLTargetDeviceArch` (`"Adreno430"`), preventing null pointer dereferences inside QuartzCore.

### Safe Process Isolation Levels (`metal=N`)
* **`metal=1`:** Safe diagnostic mode (only `metal_test` or clients listed in `OP2Metal.clients`).
* **`metal=2` (Default for bring-up):** Enabled for **all processes except `backboardd`**. Apps obtain a working Metal device; WindowServer safely continues its proven CPU/OpenGL ES rendering loop without disruption.
* **`metal=3`:** Enabled across all processes including `backboardd`.

---

## 5. Standalone Diagnostic Tool: `metal_test`

Installed at `/usr/bin/metal_test`, this self-contained diagnostic validates:
1. **IOKit Registration:** Verifies `IOAcceleratorES` and properties `MetalPluginName` and `MetalPluginClassName`.
2. **Default Device Acquisition:** Calls `MTLCreateSystemDefaultDevice()`, checking non-null status and feature flags.
3. **Memory Integrity:** Allocates 1 MB shared `MTLBuffer`, writes XOR test patterns, and reads back with 100% checksum verification.
4. **Command Pipeline:** Creates command queue, command buffer, blit command encoder, and commits cleanly.

---

## 6. Next Step: Phase 2 Adreno 430 Kernel Driver

With userland stabilized and crash-hardened, we proceed to **Phase 2**:
* Implementing `OP2GPU.cpp` mapping Adreno 430 registers at `0xfdb00000`.
* Ungating core clocks via MSM8994 GCC.
* Allocating the command ring buffer and submitting the first PM4 Type 3 NOP packet.
