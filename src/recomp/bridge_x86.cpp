// In-process bridge between the original 32-bit engine and statically recompiled code (prototype on x86/Wine).
//
// Native -> recompiled: a per-function stub `push fn; jmp RecompEnter`. RecompEnter saves the registers, switches
// to a per-thread host stack (the recompiled C code must not use the guest stack, which the guest code itself
// pushes onto), runs fn on a Ctx and returns to the guest return address with the resulting registers.
//
// Recompiled -> native: RecompCallNative swaps the already pushed guest return address for NativeReturn, switches
// to the guest stack, loads the registers and jumps to the native code; NativeReturn collects the registers (and an
// x87 return value) back into the Ctx and resumes on the host stack.
//
// Per-thread bridge state is reached through fs:[0x14] (TIB ArbitraryUserPointer), usable from assembly without
// clobbering registers.
#include <windows.h>

#include <cstdint>
#include <cstring>

#include "../host/hooks.h"
#include "bridge.h"
#include "runtime.h"

extern "C" {
extern const struct RecompTableEntry {
    uint32_t va;
    RecompFn fn;
} g_recomp_table[];
extern const uint32_t g_recomp_count;
uint8_t* RecompMemoryBase = nullptr;
}

namespace {

constexpr size_t kHostStackSize = 1 << 20;

// Packed to 4: the assembly below uses fixed offsets, and fldl/fstpl do not need 8-byte alignment.
#pragma pack(push, 4)
struct Bridge {
    uint32_t host_top;      // +0  where the next host-stack frame may start (moves down while nested)
    Ctx* native_ctx;        // +4  Ctx of the native call in progress
    uint32_t host_resume;   // +8  host esp to resume after the native call
    double st0;             // +12 x87 value returned by the native call
    uint32_t has_st0;       // +20
    uint8_t* stack_base;    // +24
};
#pragma pack(pop)
static_assert(offsetof(Bridge, native_ctx) == 4 && offsetof(Bridge, host_resume) == 8 &&
                  offsetof(Bridge, st0) == 12 && offsetof(Bridge, has_st0) == 20,
              "offsets used by the assembly below");
static_assert(offsetof(Ctx, eax) == 0 && offsetof(Ctx, esp) == 16 && offsetof(Ctx, edi) == 28,
              "Ctx register offsets used by the assembly below");

Bridge* ThreadBridge() {
    Bridge* b;
    asm volatile("movl %%fs:0x14, %0" : "=r"(b));
    if (!b) {
        b = static_cast<Bridge*>(VirtualAlloc(nullptr, sizeof(Bridge), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        b->stack_base = static_cast<uint8_t*>(
            VirtualAlloc(nullptr, kHostStackSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        b->host_top = reinterpret_cast<uint32_t>(b->stack_base + kHostStackSize - 64);
        asm volatile("movl %0, %%fs:0x14" : : "r"(b));
    }
    return b;
}

// Exit record consumed by RecompEnter's epilogue.
#pragma pack(push, 4)
struct ExitRecord {
    uint32_t eax, ecx, edx, ebx, ebp, esi, edi, esp, ret_target;  // +0 .. +32
    double st0;                                                     // +36
    uint32_t has_st0;                                               // +44
};
#pragma pack(pop)
static_assert(offsetof(ExitRecord, esp) == 28 && offsetof(ExitRecord, ret_target) == 32 &&
                  offsetof(ExitRecord, st0) == 36 && offsetof(ExitRecord, has_st0) == 44,
              "ExitRecord offsets used by the assembly below");

// pushad frame as laid out on the guest stack, followed by the fn pointer pushed by the stub.
struct EntryFrame {
    uint32_t edi, esi, ebp, esp_unused, ebx, edx, ecx, eax;
    RecompFn fn;
};

}  // namespace

extern "C" uint32_t RecompHostStackTop() {
    return ThreadBridge()->host_top & ~15u;
}

extern "C" ExitRecord* RecompEnterC(EntryFrame* frame) {
    // Evidence that recompiled code actually runs: log at every power of two entries.
    static volatile LONG entries = 0;
    LONG n = InterlockedIncrement(&entries);
    if ((n & (n - 1)) == 0 && n >= 1024) {
        protore::Log("recomp: %ld entries into recompiled code", n);
    }
    Bridge* b = ThreadBridge();
    const uint32_t saved_top = b->host_top;
    uint32_t here;
    asm volatile("movl %%esp, %0" : "=r"(here));
    b->host_top = here - 256;  // nested entries continue below this frame

    Ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.eax = frame->eax;
    ctx.ecx = frame->ecx;
    ctx.edx = frame->edx;
    ctx.ebx = frame->ebx;
    ctx.ebp = frame->ebp;
    ctx.esi = frame->esi;
    ctx.edi = frame->edi;
    ctx.esp = reinterpret_cast<uint32_t>(&frame->fn) + 4;  // the guest return address
    ctx.fpu_cw = 0x027f;
    frame->fn(&ctx);

    static thread_local ExitRecord exit_record;
    ExitRecord& r = exit_record;
    r.eax = ctx.eax;
    r.ecx = ctx.ecx;
    r.edx = ctx.edx;
    r.ebx = ctx.ebx;
    r.ebp = ctx.ebp;
    r.esi = ctx.esi;
    r.edi = ctx.edi;
    r.esp = ctx.esp;
    r.ret_target = ctx.ret_target;
    // A value left on the emulated x87 stack is the function's float return value.
    r.has_st0 = ctx.st_top != 0;
    r.st0 = r.has_st0 ? ctx.st[ctx.st_top & 7] : 0.0;
    b->host_top = saved_top;
    return &r;
}

asm(".text\n"
    ".globl _RecompEnter\n"
    "_RecompEnter:\n\t"
    "pushal\n\t"
    "movl %esp, %ebx\n\t"
    "call _RecompHostStackTop\n\t"
    "movl %eax, %esp\n\t"
    "pushl %ebx\n\t"
    "call _RecompEnterC\n\t"
    "movl %eax, %esi\n\t"
    "cmpl $0, 44(%esi)\n\t"
    "je 1f\n\t"
    "fldl 36(%esi)\n"
    "1:\n\t"
    "movl 0(%esi), %eax\n\t"
    "movl 4(%esi), %ecx\n\t"
    "movl 8(%esi), %edx\n\t"
    "movl 12(%esi), %ebx\n\t"
    "movl 16(%esi), %ebp\n\t"
    "movl 24(%esi), %edi\n\t"
    "movl 28(%esi), %esp\n\t"
    "pushl 32(%esi)\n\t"
    "movl 20(%esi), %esi\n\t"
    "ret\n"

    // void RecompCallNativeAsm(Ctx* c, uint32_t target): cdecl.
    ".globl _RecompCallNativeAsm\n"
    "_RecompCallNativeAsm:\n\t"
    "pushl %ebp\n\t"
    "pushl %ebx\n\t"
    "pushl %esi\n\t"
    "pushl %edi\n\t"
    "movl %fs:0x14, %eax\n\t"
    "movl %esp, 8(%eax)\n\t"     // host_resume
    "movl 20(%esp), %ecx\n\t"    // c
    "movl %ecx, 4(%eax)\n\t"     // native_ctx
    "movl %esp, 0(%eax)\n\t"     // host_top: nested entries go below this frame
    "subl $256, 0(%eax)\n\t"
    "movl 24(%esp), %edx\n\t"    // target
    "movl 16(%ecx), %ebx\n\t"    // guest esp
    "movl $_RecompNativeReturn, (%ebx)\n\t"  // replace the pushed return address
    "movl %edx, -4(%ebx)\n\t"    // target, read back after esp moves to the guest stack (dead slot below it)
    "movl 0(%ecx), %eax\n\t"
    "movl 8(%ecx), %edx\n\t"
    "movl 12(%ecx), %ebx\n\t"
    "movl 20(%ecx), %ebp\n\t"
    "movl 24(%ecx), %esi\n\t"
    "movl 28(%ecx), %edi\n\t"
    "movl 16(%ecx), %esp\n\t"
    "movl 4(%ecx), %ecx\n\t"
    "jmp *-4(%esp)\n"
    ".globl _RecompNativeReturn\n"
    "_RecompNativeReturn:\n\t"
    "pushl %eax\n\t"
    "movl %fs:0x14, %eax\n\t"
    "movl 4(%eax), %eax\n\t"     // ctx
    "movl %ecx, 4(%eax)\n\t"
    "movl %edx, 8(%eax)\n\t"
    "movl %ebx, 12(%eax)\n\t"
    "movl %ebp, 20(%eax)\n\t"
    "movl %esi, 24(%eax)\n\t"
    "movl %edi, 28(%eax)\n\t"
    "popl %ecx\n\t"
    "movl %ecx, 0(%eax)\n\t"
    "movl %esp, 16(%eax)\n\t"
    "movl %fs:0x14, %ecx\n\t"
    "movl $0, 20(%ecx)\n\t"      // has_st0 = 0
    "fnstsw %ax\n\t"
    "shrl $11, %eax\n\t"
    "andl $7, %eax\n\t"
    "je 2f\n\t"
    "fstpl 12(%ecx)\n\t"
    "movl $1, 20(%ecx)\n"
    "2:\n\t"
    "movl 8(%ecx), %esp\n\t"
    "popl %edi\n\t"
    "popl %esi\n\t"
    "popl %ebx\n\t"
    "popl %ebp\n\t"
    "ret\n");

extern "C" void RecompEnter();
extern "C" void RecompCallNativeAsm(Ctx* c, uint32_t target);

namespace {

int Compare(const void* key, const void* entry) {
    uint32_t va = *static_cast<const uint32_t*>(key);
    uint32_t other = static_cast<const RecompTableEntry*>(entry)->va;
    return va < other ? -1 : va > other ? 1 : 0;
}

}  // namespace

extern "C" RecompFn RecompLookup(uint32_t target) {
    const void* hit = bsearch(&target, g_recomp_table, g_recomp_count, sizeof(RecompTableEntry), &Compare);
    return hit ? static_cast<const RecompTableEntry*>(hit)->fn : nullptr;
}

extern "C" void RecompDispatch(Ctx* ctx, uint32_t target) {
    if (!protore::TestDetourActive(target)) {
        if (RecompFn fn = RecompLookup(target)) {
            fn(ctx);
            return;
        }
    }
    Bridge* b = ThreadBridge();
    // Native calls may nest (native code calling back into recompiled code calling native again).
    Ctx* saved_ctx = b->native_ctx;
    const uint32_t saved_resume = b->host_resume;
    const uint32_t saved_top = b->host_top;
    RecompCallNativeAsm(ctx, target);
    b->native_ctx = saved_ctx;
    b->host_resume = saved_resume;
    b->host_top = saved_top;
    if (b->has_st0) {
        ctx->st_top = (ctx->st_top - 1) & 7;
        ctx->st[ctx->st_top] = b->st0;
        b->has_st0 = 0;
    }
}

extern "C" void RecompUnimplemented(Ctx* ctx, uint32_t va, const char* what) {
    protore::Log("recomp: unimplemented instruction at %08x (%s), esp=%08x", va, what, ctx->esp);
    TerminateProcess(GetCurrentProcess(), 0x7e);
}

namespace protore {

void* RecompEntryStub(uint32_t va) {
    RecompFn fn = RecompLookup(va);
    if (!fn) {
        return nullptr;
    }
    static uint8_t* pool = nullptr;
    static size_t used = 0;
    if (!pool) {
        pool = static_cast<uint8_t*>(VirtualAlloc(nullptr, 1 << 20, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    }
    uint8_t* stub = pool + used;
    used += 16;
    stub[0] = 0x68;  // push imm32
    uint32_t fn_value = reinterpret_cast<uint32_t>(fn);
    memcpy(stub + 1, &fn_value, 4);
    stub[5] = 0xE9;  // jmp rel32
    int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&RecompEnter) - reinterpret_cast<uintptr_t>(stub + 10));
    memcpy(stub + 6, &rel, 4);
    return stub;
}

}  // namespace protore
