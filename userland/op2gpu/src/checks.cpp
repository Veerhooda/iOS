#include "op2gpu/checks.hpp"

#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <vector>

#include "op2gpu/blit.hpp"

namespace op2gpu {

namespace {

constexpr int64_t kTimeout = 2000000000;   // 2 s
constexpr uint8_t kCanary = 0xcd;

// FNV-1a over the bytes: one number per buffer to compare runs by
uint32_t hash(const uint8_t* p, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}

std::string fmt(const char* f, ...) __attribute__((format(printf, 1, 2)));
std::string fmt(const char* f, ...) {
    char b[256];
    va_list a;
    va_start(a, f);
    vsnprintf(b, sizeof b, f, a);
    va_end(a);
    return b;
}

// the first byte where P and Q differ, or -1
long firstDiff(const uint8_t* p, const uint8_t* q, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (p[i] != q[i]) return long(i);
    return -1;
}

}  // namespace

int runBlitChecks(Gpu& gpu, const std::function<void(const std::string&)>& say) {
    int failures = 0;
    auto check = [&](bool ok, const std::string& what) {
        say((ok ? "ok   " : "FAIL ") + what);
        if (!ok) failures++;
    };

    uint64_t faults0 = gpu.faults();
    say(fmt("chip 0x%08x faults %llu", gpu.chipId(), (unsigned long long)faults0));
    int e = 0;
    const uint64_t kSize = 1 << 20;
    std::unique_ptr<Bo> a = gpu.newBo(kSize, 0, &e);
    std::unique_ptr<Bo> b = gpu.newBo(kSize, 0, &e);
    if (!a || !b) {
        check(false, fmt("two 1 MB buffers: errno %d", e));
        return failures;
    }
    auto* pa = static_cast<uint8_t*>(a->cpu());
    auto* pb = static_cast<uint8_t*>(b->cpu());
    std::vector<uint8_t> want(kSize);
    std::string why;
    Batch batch(gpu);

    // 1. fills of awkward lengths at awkward (word) offsets; every byte of the buffer compared, canaries included
    {
        const uint64_t lengths[] = {4, 8, 12, 60, 64, 68, 4092, 4096, 4100, 64000, 64004, 262144};
        const uint64_t offsets[] = {0, 4, 12, 4092, 65536 + 8};
        int done = 0, bad = 0;
        std::string first;
        for (uint64_t len : lengths) {
            for (uint64_t off : offsets) {
                if (off + len + 64 > kSize) continue;
                memset(pa, kCanary, kSize);
                memset(want.data(), kCanary, kSize);
                uint8_t v = uint8_t(0x11 + done * 7);
                memset(want.data() + off, v, len);
                if (!batch.fill(*a, off, len, v, &why) || batch.run(0, kTimeout, &why) != 0) {
                    bad++;
                    if (first.empty()) first = fmt("len %llu off %llu: %s", (unsigned long long)len, (unsigned long long)off, why.c_str());
                } else {
                    long d = firstDiff(pa, want.data(), kSize);
                    if (d >= 0) {
                        bad++;
                        if (first.empty())
                            first = fmt("len %llu off %llu: byte %ld is 0x%02x want 0x%02x", (unsigned long long)len,
                                        (unsigned long long)off, d, pa[d], want[d]);
                    }
                }
                done++;
            }
        }
        check(bad == 0, fmt("fill: %d of %d (length, offset) pairs byte exact%s%s", done - bad, done,
                            first.empty() ? "" : "; first: ", first.c_str()));
    }

    // 2. a copy of 500 000 bytes, the whole of both buffers compared
    {
        const uint64_t n = 500000, from = 8, to = 4096;
        for (uint64_t i = 0; i < kSize; i++) pa[i] = uint8_t(i * 31 + (i >> 9));
        memset(pb, kCanary, kSize);
        memset(want.data(), kCanary, kSize);
        memcpy(want.data() + to, pa + from, n);
        bool ran = batch.copy(*a, from, *b, to, n, &why) && batch.run(0, kTimeout, &why) == 0;
        long d = ran ? firstDiff(pb, want.data(), kSize) : 0;
        check(ran && d < 0, fmt("copy of %llu bytes: %s, %lld us, hash %08x (want %08x)", (unsigned long long)n,
                                !ran ? why.c_str() : d < 0 ? "byte exact" : fmt("byte %ld differs", d).c_str(),
                                (long long)(batch.lastRunNs() / 1000), hash(pb, kSize), hash(want.data(), kSize)));
        // and one not 8-aligned against 8-aligned (all 32-bit packets), one word long, one odd word count
        const uint64_t cases[][3] = {{4, 8, 4096}, {12, 20, 4}, {16, 32, 12}};
        int bad = 0;
        for (auto& c : cases) {
            memset(pb, kCanary, kSize);
            memset(want.data(), kCanary, kSize);
            memcpy(want.data() + c[1], pa + c[0], c[2]);
            if (!batch.copy(*a, c[0], *b, c[1], c[2], &why) || batch.run(0, kTimeout, &why) != 0 ||
                firstDiff(pb, want.data(), kSize) >= 0)
                bad++;
        }
        check(bad == 0, fmt("copy: unaligned pairs, one word, odd word count: %d of 3 byte exact", 3 - bad));
    }

    // 3. 40 batches, each marking its index then copying the previous mark forward: their order is visible
    {
        auto* wa = static_cast<uint32_t*>(a->cpu());
        memset(pa, 0, 4096);
        bool ok = true;
        for (uint32_t i = 0; i < 40 && ok; i++) {
            ok = batch.mark(*a, 4 * (i + 1), 0x40000000u | i, &why) &&
                 batch.copy(*a, 4 * (i + 1), *a, 1024 + 4 * i, 4, &why) && batch.run(0, kTimeout, &why) == 0;
        }
        int wrong = 0;
        for (uint32_t i = 0; i < 40; i++)
            if (wa[1 + i] != (0x40000000u | i) || wa[256 + i] != (0x40000000u | i)) wrong++;
        check(ok && wrong == 0, fmt("40 batches in order: %s, %d words wrong", ok ? "all ran" : why.c_str(), wrong));
    }

    // 4. refusals by name, nothing submitted
    {
        bool r1 = !batch.fill(*a, 1, 4, 0, &why);
        bool r2 = !batch.copy(*a, 0, *a, 4, 64, &why);           // overlapping
        bool r3 = !batch.copy(*a, 0, *b, kSize - 4, 8, &why);    // outside
        check(r1 && r2 && r3 && batch.empty(), "refused: byte fill, overlapping copy, copy past the end");
    }

    // 5. throughput: a 1 MB fill
    {
        bool ran = batch.fill(*b, 0, kSize, 0x5a, &why) && batch.run(0, kTimeout, &why) == 0;
        int64_t us = batch.lastRunNs() / 1000;
        bool exact = ran && pb[0] == 0x5a && pb[kSize - 1] == 0x5a && hash(pb, kSize) == [&] {
            memset(want.data(), 0x5a, kSize);
            return hash(want.data(), kSize);
        }();
        check(exact, fmt("fill of 1 MB: %s in %lld us (%.0f MB/s)", exact ? "exact" : why.c_str(), (long long)us,
                         us > 0 ? double(kSize) / double(us) : 0.0));
    }

    check(gpu.faults() == faults0, fmt("no GPU reset during the checks (faults %llu -> %llu)",
                                       (unsigned long long)faults0, (unsigned long long)gpu.faults()));
    return failures;
}

}  // namespace op2gpu
