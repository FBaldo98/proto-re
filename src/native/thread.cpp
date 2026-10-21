// Guest threads and fibers. Each guest thread runs on a host thread with its own TEB and guest stack in guest
// memory. Fibers get their own guest stack and host stack; SwitchToFiber swaps host contexts (ucontext for now;
// Android's bionic has no swapcontext, an arm64 context switch replaces it there).
#include <ucontext.h>

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

#include "guest.h"

namespace native {

extern thread_local Ctx* tl_ctx;

namespace {

thread_local GuestThread* tl_thread = nullptr;
std::atomic<uint32_t> g_next_thread_id{0x100};

// TEB fields the engine and the MSVC CRT read through fs.
constexpr uint32_t kTebExceptionList = 0x00;
constexpr uint32_t kTebStackBase = 0x04;
constexpr uint32_t kTebStackLimit = 0x08;
constexpr uint32_t kTebFiberData = 0x10;
constexpr uint32_t kTebSelf = 0x18;
constexpr uint32_t kTebClientId = 0x20;
constexpr uint32_t kTebTlsSlots = 0xe10;
constexpr uint32_t kTebSize = 0x1000;

}  // namespace

GuestThread* AttachThread(uint32_t stack_size) {
    auto* t = new GuestThread();
    stack_size = stack_size ? (stack_size + 0xffff) & ~0xffffu : (1u << 20) * 4;
    t->stack_lo = PageAlloc(stack_size);
    t->stack_hi = t->stack_lo + stack_size;
    t->teb = PageAlloc(kTebSize);
    t->id = g_next_thread_id.fetch_add(4);
    W32(t->teb + kTebExceptionList, 0xffffffffu);
    W32(t->teb + kTebStackBase, t->stack_hi);
    W32(t->teb + kTebStackLimit, t->stack_lo);
    W32(t->teb + kTebSelf, t->teb);
    W32(t->teb + kTebClientId, 0x10);
    W32(t->teb + kTebClientId + 4, t->id);
    tl_thread = t;
    return t;
}

GuestThread* CurrentThread() {
    if (!tl_thread) Fatal("guest call on a host thread that was never attached");
    return tl_thread;
}

uint32_t TebTlsSlot(int index) { return CurrentThread()->teb + kTebTlsSlots + 4 * index; }

// ---- fibers ----
namespace {

struct Fiber {
    uint32_t handle;  // guest block: [0] fiber data (GetFiberData), [4] index
    uint32_t start, param;
    uint32_t stack_lo, stack_hi;
    ucontext_t uc;
    void* host_stack;
    bool is_thread;  // the converted thread itself (runs on the thread's own stacks)
};

std::mutex g_fibers_mutex;
std::vector<Fiber*> g_fibers;

Fiber* FiberOf(uint32_t handle) {
    std::lock_guard<std::mutex> lock(g_fibers_mutex);
    uint32_t index = R32(handle + 4);
    return index < g_fibers.size() ? g_fibers[index] : nullptr;
}

Fiber* NewFiber(uint32_t param) {
    auto* f = new Fiber();
    f->handle = HeapAlloc(32);
    W32(f->handle, param);
    std::lock_guard<std::mutex> lock(g_fibers_mutex);
    W32(f->handle + 4, uint32_t(g_fibers.size()));
    g_fibers.push_back(f);
    f->param = param;
    return f;
}

void SetCurrentFiber(GuestThread* t, Fiber* f) {
    t->current_fiber = f ? f->handle : 0;
    W32(t->teb + kTebFiberData, f ? f->handle : 0);
    if (f && !f->is_thread) {
        W32(t->teb + kTebStackBase, f->stack_hi);
        W32(t->teb + kTebStackLimit, f->stack_lo);
    }
}

thread_local uint32_t tl_thread_stack_hi = 0;

void FiberMain(unsigned lo, unsigned hi) {
    Fiber* f = reinterpret_cast<Fiber*>((uintptr_t(hi) << 32) | lo);
    GuestThread* t = CurrentThread();
    t->stack_hi = f->stack_hi;  // GuestCall starts from the fiber's own guest stack
    tl_ctx = nullptr;
    GuestCall(f->start, {f->param});
    Fatal("fiber %08x returned from its start routine", f->handle);
}

}  // namespace

uint32_t FiberConvertThread(uint32_t param) {
    GuestThread* t = CurrentThread();
    Fiber* f = NewFiber(param);
    f->is_thread = true;
    f->stack_lo = t->stack_lo;
    f->stack_hi = t->stack_hi;
    tl_thread_stack_hi = t->stack_hi;
    SetCurrentFiber(t, f);
    return f->handle;
}

uint32_t FiberCreate(uint32_t stack_size, uint32_t start, uint32_t param) {
    Fiber* f = NewFiber(param);
    f->start = start;
    stack_size = stack_size ? (stack_size + 0xffff) & ~0xffffu : (1u << 20);
    f->stack_lo = PageAlloc(stack_size);
    f->stack_hi = f->stack_lo + stack_size;
    constexpr size_t kHostStack = 4u << 20;  // recompiled C frames are larger than the guest frames
    f->host_stack = malloc(kHostStack);
    getcontext(&f->uc);
    f->uc.uc_stack.ss_sp = f->host_stack;
    f->uc.uc_stack.ss_size = kHostStack;
    f->uc.uc_link = nullptr;
    uintptr_t p = reinterpret_cast<uintptr_t>(f);
    makecontext(&f->uc, reinterpret_cast<void (*)()>(FiberMain), 2, unsigned(p), unsigned(p >> 32));
    return f->handle;
}

void FiberSwitch(uint32_t handle) {
    GuestThread* t = CurrentThread();
    Fiber* from = t->current_fiber ? FiberOf(t->current_fiber) : nullptr;
    Fiber* to = FiberOf(handle);
    if (!from || !to) Fatal("SwitchToFiber %08x without a current fiber", handle);
    if (from == to) return;
    // Per-fiber host state that lives in thread-locals: the active Ctx and the guest stack top.
    Ctx* saved_ctx = tl_ctx;
    uint32_t saved_stack_hi = t->stack_hi;
    SetCurrentFiber(t, to);
    swapcontext(&from->uc, &to->uc);
    // Resumed.
    SetCurrentFiber(t, from);
    tl_ctx = saved_ctx;
    t->stack_hi = saved_stack_hi;
}

void FiberDelete(uint32_t handle) {
    Fiber* f = FiberOf(handle);
    if (!f || f->is_thread) return;
    // The stacks stay allocated: deleting the running fiber's stacks would pull them from under it.
}

void FiberConvertToThread() {
    GuestThread* t = CurrentThread();
    SetCurrentFiber(t, nullptr);
}

}  // namespace native
