// Guest address space: the low 4 GiB of the host process.
//
//   import thunks: an arena block (imports.cpp)
//   kEngineBase (guest.h) engine image, rebased; kept out of the arena
//   0x12000000 .. 0xf0000000  arena: VirtualAlloc pages, guest heap, stacks, TEBs
//
// The arena is the set of gaps between the mappings already there at startup (on Android, ART keeps its Java heap
// and boot image below 4 GiB for compressed references). Guest allocations map their pages read/write with
// MAP_NORESERVE when they are made (the kernel commits pages on first touch, close enough to VirtualAlloc's
// reserve/commit) and unmap them when freed, leaving the rest of the low 4 GiB to others (Qualcomm's GPU driver).
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <map>
#include <mutex>

#include "guest.h"

namespace native {

namespace {

constexpr uint32_t kArenaLo = 0x12000000u;
constexpr uint32_t kArenaHi = 0xf0000000u;
constexpr uint32_t kPage = 0x10000u;  // VirtualAlloc allocation granularity

std::mutex g_mutex;
std::map<uint32_t, uint32_t> g_free;  // start -> size, page ranges available
std::map<uint32_t, uint32_t> g_used;  // start -> size, allocated page ranges

uint32_t RoundUp(uint32_t v, uint32_t a) { return (v + a - 1) & ~(a - 1); }

// Maps [a, a + size) for the guest unless something else got there first (MAP_FIXED_NOREPLACE).
bool MapGuest(uint32_t a, uint32_t size) {
    void* p = mmap(reinterpret_cast<void*>(uintptr_t(a)), size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
    if (p == reinterpret_cast<void*>(uintptr_t(a))) return true;
    if (p != MAP_FAILED) munmap(p, size);  // an old kernel took the address as a hint only
    return false;
}

// Removes [lo, hi) from the candidate ranges.
void Withdraw(uint64_t lo, uint64_t hi) {
    for (auto f = g_free.begin(); f != g_free.end();) {
        uint64_t fs = f->first, fe = fs + f->second;
        if (fe <= lo || fs >= hi) {
            ++f;
            continue;
        }
        f = g_free.erase(f);
        if (fs < lo) g_free[uint32_t(fs)] = uint32_t(lo - fs);
        if (fe > hi) g_free[uint32_t(hi)] = uint32_t(fe - hi);
    }
}

// Someone else (the GPU driver, ART) mapped into a candidate range: drop every foreign mapping from the candidates.
void WithdrawForeignMappings() {
    FILE* maps = fopen("/proc/self/maps", "r");
    if (!maps) return;
    char line[512];
    while (fgets(line, sizeof(line), maps)) {
        unsigned long long lo, hi;
        if (sscanf(line, "%llx-%llx", &lo, &hi) != 2 || lo >= 0x100000000ull) continue;
        auto u = g_used.upper_bound(uint32_t(lo));
        if (u != g_used.begin() && std::prev(u)->first + uint64_t(std::prev(u)->second) > lo) continue;  // ours
        Withdraw(lo & ~uint64_t(kPage - 1), (hi + kPage - 1) & ~uint64_t(kPage - 1));
    }
    fclose(maps);
}

// The arena is address space below 4 GiB that the guest may use (g_free), mapped only when the guest allocates
// it. Reserving it all up front left nothing below 3 GiB for Qualcomm's driver, which maps GPU memory at matching
// CPU and GPU addresses there (KGSL SVM): DXVK's first host-visible allocation failed.
uint32_t PageAllocLocked(uint32_t size, uint32_t at) {
    size = RoundUp(size ? size : 1, kPage);
    if (at) {
        at &= ~(kPage - 1);
        // Committing inside an existing reservation returns it unchanged.
        auto u = g_used.upper_bound(at);
        if (u != g_used.begin()) {
            --u;
            if (at >= u->first && at < u->first + u->second) {
                return at;
            }
        }
        auto f = g_free.upper_bound(at);
        if (f == g_free.begin()) {
            return 0;
        }
        --f;
        uint32_t fs = f->first, fsz = f->second;
        if (at < fs || uint64_t(at) + size > uint64_t(fs) + fsz) {
            return 0;
        }
        if (!MapGuest(at, size)) {
            WithdrawForeignMappings();
            return 0;
        }
        g_free.erase(f);
        if (at > fs) {
            g_free[fs] = at - fs;
        }
        if (at + size < fs + fsz) {
            g_free[at + size] = fs + fsz - (at + size);
        }
        g_used[at] = size;
        return at;
    }
    for (int attempt = 0; attempt < 8; ++attempt) {
        auto f = g_free.begin();
        while (f != g_free.end() && f->second < size) ++f;
        if (f == g_free.end()) return 0;
        uint32_t a = f->first, rest = f->second - size;
        if (!MapGuest(a, size)) {
            WithdrawForeignMappings();
            continue;
        }
        g_free.erase(f);
        if (rest) {
            g_free[a + size] = rest;
        }
        g_used[a] = size;
        return a;
    }
    return 0;
}

// ---- heap: size classes carved from page chunks, large blocks straight from pages ----
// Block header (16 bytes, keeps 16-byte alignment): size class index or ~0 for large, user size.
struct Header {
    uint32_t cls;
    uint32_t size;
    uint32_t magic;
    uint32_t pad;
};
constexpr uint32_t kMagic = 0x48454150u;  // "HEAP"
constexpr uint32_t kLarge = 0xffffffffu;
constexpr int kClasses = 40;
uint32_t g_class_size[kClasses];
uint32_t g_free_list[kClasses];
uint32_t g_chunk_ptr = 0, g_chunk_end = 0;

void InitClasses() {
    int n = 0;
    for (uint32_t s = 32; s <= 512; s += 32) g_class_size[n++] = s;       // 16 classes
    for (uint32_t s = 768; s <= 65536 && n < kClasses; s += s / 2) g_class_size[n++] = s;
    while (n < kClasses) g_class_size[n++] = 0xffffffffu;
}

int ClassFor(uint32_t total) {
    for (int i = 0; i < kClasses; ++i) {
        if (g_class_size[i] != 0xffffffffu && total <= g_class_size[i]) return i;
    }
    return -1;
}

}  // namespace

// Top of the arena; setting arena_top (hex) lowers it.
uint64_t ArenaTop() {
    std::string top = Setting("arena_top", "");
    if (!top.empty()) return std::min<uint64_t>(strtoull(top.c_str(), nullptr, 16), kArenaHi);
    return kArenaHi;
}

void InitGuestMemory() {
    // Mappings overlapping the arena, rounded out to the allocation granularity.
    std::map<uint64_t, uint64_t> taken;  // start -> end
    if (FILE* maps = fopen("/proc/self/maps", "r")) {
        char line[512];
        while (fgets(line, sizeof(line), maps)) {
            unsigned long long lo, hi;
            if (sscanf(line, "%llx-%llx", &lo, &hi) != 2 || lo >= kArenaHi || hi <= 0x10000000u) continue;
            taken[lo & ~uint64_t(kPage - 1)] = (hi + kPage - 1) & ~uint64_t(kPage - 1);
#ifdef __ANDROID__
            Log("memory: below 4 GiB: %.120s", line);
#endif
        }
        fclose(maps);
    }
    // The recompiled code only works with the engine at kEngineBase: say so plainly if something already sits there.
    for (const auto& t : taken) {
        if (t.first < uint64_t(kEngineBase) + kEngineReserve && t.second > kEngineBase) {
            Fatal("the engine's address range %08x-%08x is in use (%08llx-%08llx, see the memory lines above)",
                  kEngineBase, kEngineBase + kEngineReserve, (unsigned long long)t.first,
                  (unsigned long long)t.second);
        }
    }
    taken[kEngineBase] = uint64_t(kEngineBase) + kEngineReserve;
    const uint64_t top = ArenaTop();
    uint64_t cursor = kArenaLo, total = 0;
    auto reserve = [&](uint64_t lo, uint64_t hi) {  // candidates only: mapped on allocation
        if (hi <= lo) return;
        g_free[uint32_t(lo)] = uint32_t(hi - lo);
        total += hi - lo;
    };
    for (const auto& t : taken) {
        if (t.first > cursor) reserve(cursor, std::min(t.first, top));
        if (t.second > cursor) cursor = t.second;
    }
    reserve(cursor, top);
    if (total < (512u << 20)) Fatal("guest arena too small: %llu MiB free below 4 GiB", (unsigned long long)(total >> 20));
    if (!taken.empty()) {
        Log("memory: arena %llu MiB in %zu ranges, top %08llx", (unsigned long long)(total >> 20), g_free.size(),
            (unsigned long long)top);
    }
    InitClasses();
}

uint32_t PageAlloc(uint32_t size, uint32_t at) {
    std::lock_guard<std::mutex> lock(g_mutex);
    return PageAllocLocked(size, at);
}

// Address space the guest holds (reserved or committed) and the highest address in use, for the perf line.
std::string ArenaUsage() {
    std::lock_guard<std::mutex> lock(g_mutex);
    uint64_t used = 0;
    uint32_t top = 0;
    for (const auto& u : g_used) {
        used += u.second;
        top = std::max(top, u.first + u.second);
    }
    static uint64_t peak = 0;
    peak = std::max(peak, used);
    char line[96];
    snprintf(line, sizeof(line), "%llu MiB (peak %llu), up to %08x", (unsigned long long)(used >> 20),
             (unsigned long long)(peak >> 20), top);
    return line;
}

void PageFree(uint32_t addr) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto u = g_used.find(addr);
    if (u == g_used.end()) {
        return;
    }
    uint32_t a = u->first, size = u->second;
    g_used.erase(u);
    munmap(Ptr<void>(a), size);  // back to the address space pool (zero pages on reuse, as VirtualAlloc guarantees)
    auto next = g_free.find(a + size);
    if (next != g_free.end()) {
        size += next->second;
        g_free.erase(next);
    }
    auto prev = g_free.lower_bound(a);
    if (prev != g_free.begin()) {
        --prev;
        if (prev->first + prev->second == a) {
            prev->second += size;
            return;
        }
    }
    g_free[a] = size;
}

uint32_t HeapAlloc(uint32_t size) {
    std::lock_guard<std::mutex> lock(g_mutex);
    uint32_t total = size + sizeof(Header);
    int cls = ClassFor(total);
    uint32_t block;
    if (cls < 0) {
        block = PageAllocLocked(total, 0);
        if (!block) return 0;
        *Ptr<Header>(block) = {kLarge, size, kMagic, 0};
        return block + sizeof(Header);
    }
    if (g_free_list[cls]) {
        block = g_free_list[cls];
        g_free_list[cls] = R32(block + sizeof(Header));
    } else {
        uint32_t csize = g_class_size[cls];
        if (g_chunk_ptr + csize > g_chunk_end) {
            g_chunk_ptr = PageAllocLocked(1 << 20, 0);
            if (!g_chunk_ptr) return 0;
            g_chunk_end = g_chunk_ptr + (1 << 20);
        }
        block = g_chunk_ptr;
        g_chunk_ptr += csize;
    }
    *Ptr<Header>(block) = {uint32_t(cls), size, kMagic, 0};
    return block + sizeof(Header);
}

void HeapFree(uint32_t addr) {
    if (!addr) return;
    uint32_t block = addr - sizeof(Header);
    Header* h = Ptr<Header>(block);
    if (h->magic != kMagic) {
        Log("heap: free of a foreign block %08x ignored", addr);
        return;
    }
    if (h->cls == kLarge) {
        h->magic = 0;
        PageFree(block);
        return;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    h->magic = 0;
    W32(addr, g_free_list[h->cls]);
    g_free_list[h->cls] = block;
}

uint32_t HeapSize(uint32_t addr) {
    Header* h = Ptr<Header>(addr - sizeof(Header));
    return h->magic == kMagic ? h->size : 0xffffffffu;
}

uint32_t HeapRealloc(uint32_t addr, uint32_t size) {
    if (!addr) return HeapAlloc(size);
    if (!size) {
        HeapFree(addr);
        return 0;
    }
    Header* h = Ptr<Header>(addr - sizeof(Header));
    if (h->cls != kLarge && size + sizeof(Header) <= g_class_size[h->cls]) {
        h->size = size;
        return addr;
    }
    uint32_t n = HeapAlloc(size);
    if (n) {
        memcpy(Ptr<void>(n), Ptr<void>(addr), h->size < size ? h->size : size);
        HeapFree(addr);
    }
    return n;
}

uint32_t GuestStrdup(const char* s) {
    uint32_t n = static_cast<uint32_t>(strlen(s)) + 1;
    uint32_t a = HeapAlloc(n);
    memcpy(Ptr<void>(a), s, n);
    return a;
}

}  // namespace native
