// Difftest for native replacements (src/native/hot_*.cpp): the recompiled original and the native version run on
// the same random inputs in guest (low 4 GiB) memory and must produce the same outputs.
//
// Build/run: src/native/tests/run_hot_test.sh (generates the reference with recomp.py --no-native).
#include <sys/mman.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

#include "../guest.h"

extern "C" {
volatile uint32_t RecompRestrictLo = 0, RecompRestrictHi = 0xffffffffu, RecompTestDetours = 0;
uint8_t* RecompMemoryBase = nullptr;
void f_585e8860(Ctx* c);  // recompiled reference (ref.c)
void Native_105e8860(Ctx* c);
void RecompDispatch(Ctx* c, uint32_t target) {
    fprintf(stderr, "unexpected dispatch to %08x\n", target);
    exit(2);
}
RecompFn RecompLookup(uint32_t) { return nullptr; }
void RecompUnimplemented(Ctx*, uint32_t va, const char* what) {
    fprintf(stderr, "trap %08x %s\n", va, what);
    exit(2);
}
void RecompCpuid(Ctx*) {}
}

namespace {

uint32_t g_next = 0x20000000u;
uint32_t Alloc(uint32_t size) {
    uint32_t a = g_next;
    g_next += (size + 63) & ~63u;
    return a;
}

void MapAt(uint32_t addr, uint32_t size) {
    void* p = mmap(reinterpret_cast<void*>(uintptr_t(addr)), size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (p != reinterpret_cast<void*>(uintptr_t(addr))) {
        perror("mmap");
        exit(1);
    }
}

}  // namespace

int main(int argc, char** argv) {
    // Engine constants used by the function (DLL addresses, values read by run_hot_test.sh).
    if (argc < 4) return 1;
    const uint32_t const_addrs[3] = {0x10e2e9dc, 0x10e2e864, 0x10e313a4};
    for (int i = 0; i < 3; ++i) {
        uint32_t a = const_addrs[i] - 0x10000000u + native::kEngineBase;
        uint32_t page = a & ~0xfffu;
        static uint32_t mapped[3];
        bool done = false;
        for (int k = 0; k < i; ++k) done |= mapped[k] == page;
        if (!done) MapAt(page, 0x1000);
        mapped[i] = page;
        float v = strtof(argv[1 + i], nullptr);
        memcpy(reinterpret_cast<void*>(uintptr_t(a)), &v, 4);
    }
    MapAt(0x20000000u, 64u << 20);

    std::mt19937 rng(1234);
    std::uniform_real_distribution<float> uf(-1.0f, 1.0f);
    int failures = 0;
    for (int trial = 0; trial < 200; ++trial) {
        g_next = 0x20000000u;
        const uint32_t ring_size = 1u << (8 + trial % 4);
        const uint32_t channels = 1 + trial % 3, outputs = 1 + (trial / 3) % 3;
        uint32_t p1 = Alloc(0x40), p2 = Alloc(0x600), p3 = Alloc(24 * channels), p4 = Alloc(4 * 8 * outputs);
        memset(native::Ptr<void>(p2), 0, 0x600);
        for (uint32_t off = 0x44; off < 0x500; off += 4) WF32(p2 + off, 0.25f + 0.75f * (uf(rng) + 1.0f));
        W32(p1 + 0xc, outputs);
        W32(p1 + 0x10, trial % 2);
        W8(p2 + 0x42, trial % 3 == 0 ? 0 : 1);
        WF32(p2 + 0x48, 0.3f + (uf(rng) + 1.0f));
        W32(p2 + 0x94, channels);
        W32(p2 + 0x98, ring_size);
        W64(p2 + 0xa8, uint64_t(rng()) * 4096u + rng() % 4096);
        W32(p2 + 0x4f8, trial % 7 == 0 ? 0x100 : rng() % 256);
        for (uint32_t ch = 0; ch < channels; ++ch) {
            W32(p2 + 0x4b0 + 4 * ch - 0x110, rng());
            W32(p2 + 0x4b0 + 4 * ch - 0x158, rng());
            uint32_t ring = Alloc(4 * ring_size);
            for (uint32_t i = 0; i < ring_size; ++i) WF32(ring + 4 * i, uf(rng));
            W32(p3 + 4 + 24 * ch, ring);
        }
        std::vector<uint32_t> bins;
        for (uint32_t b = 0; b < 8 * outputs; ++b) {
            uint32_t buf = Alloc(4 * 272);
            for (uint32_t i = 0; i < 272; ++i) WF32(buf + 4 * i, uf(rng));
            W32(p4 + 4 * b, buf);
            bins.push_back(buf);
        }
        // Snapshot everything the function may write, run both, compare.
        const uint32_t lo = 0x20000000u, size = g_next - lo;
        std::vector<uint8_t> start(size), ref(size);
        memcpy(start.data(), native::Ptr<void>(lo), size);
        uint32_t stack = Alloc(0x4000) + 0x3000;
        auto run = [&](void (*fn)(Ctx*)) {
            memset(native::Ptr<void>(stack - 0x3000), 0, 0x4000);  // same "garbage" past the block for both
            Ctx c;
            memset(&c, 0, sizeof(c));
            c.fpu_cw = 0x007f;  // single precision: D3D9 sets it without D3DCREATE_FPU_PRESERVE
            c.esp = stack;
            Push32(&c, p4);
            Push32(&c, p3);
            Push32(&c, p2);
            Push32(&c, p1);
            Push32(&c, 0x12345678u);
            fn(&c);
            return c;
        };
        memcpy(native::Ptr<void>(lo), start.data(), size);
        Ctx a = run(f_585e8860);
        memcpy(ref.data(), native::Ptr<void>(lo), size);
        memcpy(native::Ptr<void>(lo), start.data(), size);
        Ctx b = run(Native_105e8860);
        if (a.esp != b.esp || a.ret_target != b.ret_target || a.st_top != b.st_top || a.ebx != b.ebx ||
            a.esi != b.esi || a.edi != b.edi || a.ebp != b.ebp) {
            printf("trial %d: register state differs (esp %08x/%08x)\n", trial, a.esp, b.esp);
            ++failures;
            continue;
        }
        // Compare as floats inside the bins and meters (the stack area is scratch).
        int diffs = 0;
        for (uint32_t off = 0; off + 4 <= stack - 0x3000 - lo; off += 4) {
            float fa, fb;
            memcpy(&fa, ref.data() + off, 4);
            memcpy(&fb, native::Ptr<uint8_t>(lo) + off, 4);
            if (memcmp(&fa, &fb, 4) != 0 && !(std::isnan(fa) && std::isnan(fb))) {
                if (diffs++ < 3) printf("trial %d: +%#x ref %.9g native %.9g\n", trial, off, fa, fb);
            }
        }
        if (diffs) {
            printf("trial %d: %d differing words\n", trial, diffs);
            ++failures;
        }
    }
    printf("%s: %d/200 trials differ\n", failures ? "FAIL" : "OK", failures);
    return failures ? 1 : 0;
}
