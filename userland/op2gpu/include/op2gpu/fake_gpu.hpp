// A GPU for the host tests: the kernel's requests answered in this process, and the command streams run by an
// interpreter as strict as the A4xx CP is known to be. It knows exactly the packets pm4.hpp writes; anything else
// (an unknown opcode, a count past the end of the buffer, an address outside every buffer, CP_EVENT_WRITE) is a fault:
// the submission's fence never passes until the "kernel" resets it, and FAULTS counts that, as OP2GPU does.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "op2gpu/gpu.hpp"

namespace op2gpu {

struct FakeGpuState {
    struct Buffer {
        uint64_t iova = 0;
        std::vector<uint8_t> bytes;
    };
    std::map<uint32_t, Buffer> buffers;   // by handle
    uint32_t nextHandle = 1;
    uint64_t nextIova = 0x10000000;
    uint32_t fenceSeq = 0, fenceDone = 0;
    uint64_t faults = 0;
    uint64_t packets = 0;
    std::vector<std::string> log;         // kLog lines and fault reasons
    std::map<uint32_t, uint32_t> registers;
};

// a transport over STATE (which the test keeps to look at memory and the log)
std::unique_ptr<Transport> newFakeGpu(std::shared_ptr<FakeGpuState> state);

}  // namespace op2gpu
