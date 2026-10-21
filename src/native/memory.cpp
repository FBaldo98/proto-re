// Guest address space: the low 4 GiB of the host process.
//
//   0x0f000000  import thunks (imports.cpp)
//   0x10000000  engine image (preferred base, no relocation needed)
//   0x12000000 .. 0xf0000000  arena: VirtualAlloc pages, guest heap, stacks, TEBs
//
// The arena is mapped read/write with MAP_NORESERVE once; the kernel commits pages on first touch, which matches
// VirtualAlloc's reserve/commit closely enough for the engine (it never relies on access faults in reserved pages).
#include <sys/mman.h>

#include <cstring>
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
    for (auto f = g_free.begin(); f != g_free.end(); ++f) {
        if (f->second >= size) {
            uint32_t a = f->first, rest = f->second - size;
            g_free.erase(f);
            if (rest) {
                g_free[a + size] = rest;
            }
            g_used[a] = size;
            return a;
        }
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

void InitGuestMemory() {
    void* p = mmap(reinterpret_cast<void*>(uintptr_t(kArenaLo)), kArenaHi - kArenaLo, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
    if (p != reinterpret_cast<void*>(uintptr_t(kArenaLo))) {
        Fatal("cannot reserve the guest arena at %08x (%p)", kArenaLo, p);
    }
    g_free[kArenaLo] = kArenaHi - kArenaLo;
    InitClasses();
}

uint32_t PageAlloc(uint32_t size, uint32_t at) {
    std::lock_guard<std::mutex> lock(g_mutex);
    return PageAllocLocked(size, at);
}

void PageFree(uint32_t addr) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto u = g_used.find(addr);
    if (u == g_used.end()) {
        return;
    }
    uint32_t a = u->first, size = u->second;
    g_used.erase(u);
    madvise(Ptr<void>(a), size, MADV_DONTNEED);  // zero pages on reuse, as VirtualAlloc guarantees
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
