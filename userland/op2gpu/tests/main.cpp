// Host tests: the G1 checks against the fake GPU, then what the fake must refuse (each a mutation the real CP
// would hang on), so a check that passes here is known to be able to fail.
#include <cstdio>
#include <cstring>

#include "op2gpu/blit.hpp"
#include "op2gpu/checks.hpp"
#include "op2gpu/fake_gpu.hpp"

using namespace op2gpu;

static int failures = 0;
static void expect(bool ok, const char* what) {
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) failures++;
}

int main() {
    // the checks the phone runs
    {
        auto st = std::make_shared<FakeGpuState>();
        Gpu gpu(newFakeGpu(st));
        int f = runBlitChecks(gpu, [](const std::string& l) { printf("  %s\n", l.c_str()); });
        expect(f == 0, "runBlitChecks on the fake GPU");
        expect(st->faults == 0, "no fault on the fake GPU");
    }

    // the packets, word for word
    {
        pm4::Stream s;
        uint32_t w[2] = {0xaabbccdd, 0x11223344};
        s.memWrite(0x1000, w, 2);
        s.memToMem(0x2000, 0x3000, true);
        s.waitForIdle();
        const uint32_t want[] = {0xc0023d00, 0x1000, 0xaabbccdd, 0x11223344,
                                 0xc0027300, 1u << 29, 0x2000, 0x3000,
                                 0xc0002600, 0};
        expect(s.size() == 10 && memcmp(s.words().data(), want, sizeof want) == 0,
               "CP_MEM_WRITE / CP_MEM_TO_MEM / CP_WAIT_FOR_IDLE words as the kernel self-test sent them");
    }

    // mutations the fake must refuse (and count as a GPU reset)
    auto refused = [](const std::vector<uint32_t>& words, const char* what) {
        auto st = std::make_shared<FakeGpuState>();
        Gpu gpu(newFakeGpu(st));
        int e = 0;
        auto target = gpu.newBo(4096, 0, &e);
        auto cmd = gpu.newBo(4 * words.size(), 0, &e);
        std::vector<uint32_t> w = words;
        for (auto& x : w) if (x == 0xfeed0000u) x = uint32_t(target->gpuAddress());
        memcpy(cmd->cpu(), w.data(), 4 * w.size());
        uint32_t fence = 0;
        gpu.submit(0, *cmd, 0, uint32_t(w.size()), {{target.get(), true}}, &fence);
        bool timedOut = gpu.wait(0, fence, 1000000) != 0;
        expect(timedOut && gpu.faults() == 1, what);
    };
    refused({pm4::type3(0x46, 3), 4, 0xfeed0000u, 1}, "refuses CP_EVENT_WRITE (never retires on the A430 today)");
    refused({pm4::type3(0x75, 3), 1, 0xfeed0000u, 0xfeed0000u}, "refuses CP_MEMCPY (A6xx only)");
    refused({pm4::type3(pm4::kMemWrite, 2), 0x100, 1}, "refuses a write outside every buffer");
    refused({pm4::type3(pm4::kMemWrite, 9), 0xfeed0000u, 1}, "refuses a count past the end of the stream");
    refused({pm4::type3(pm4::kMemToMem, 3), 1u << 29, 0xfeed0000u + 4, 0xfeed0000u}, "refuses an unaligned 64-bit MEM_TO_MEM");

    // a batch that hangs: run() says so, by errno, and the next batch still runs
    {
        auto st = std::make_shared<FakeGpuState>();
        Gpu gpu(newFakeGpu(st));
        int e = 0;
        auto bo = gpu.newBo(4096, 0, &e);
        Batch batch(gpu);
        std::string why;
        // a copy whose source lies outside every buffer: through the API this is refused, so build it by hand
        pm4::Stream bad;
        bad.memToMem(uint32_t(bo->gpuAddress()), 0x200, false);
        auto cmd = gpu.newBo(4 * bad.size(), 0, &e);
        memcpy(cmd->cpu(), bad.words().data(), 4 * bad.size());
        uint32_t fence = 0;
        gpu.submit(0, *cmd, 0, uint32_t(bad.size()), {}, &fence);
        expect(gpu.wait(0, fence, 1000000) == ETIMEDOUT, "a hung submission times out");
        expect(batch.fill(*bo, 0, 64, 7, &why) && batch.run(0, 1000000000, &why) == 0 &&
                   static_cast<uint8_t*>(bo->cpu())[63] == 7,
               "the batch after a reset runs");
    }

    printf("%s: %d failure(s)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
