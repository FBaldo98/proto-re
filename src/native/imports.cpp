// Import thunks: each imported dll!name gets a 16-byte slot at g_thunk_base + 16*index (an arena block: a fixed
// address may be taken on Android, where ART maps its heap below 4 GiB). Generated code calls through
// the IAT with RecompDispatch(target), and target falls in this range. Data imports (MSVCR80 _HUGE, _adjust_fdiv)
// keep their value in the slot itself.
#include <sys/mman.h>

#include <cmath>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "guest.h"

namespace native {

namespace {

uint32_t g_thunk_base = 0;
constexpr uint32_t kThunkSize = 0x00100000u;
constexpr uint32_t kSlot = 16;

struct Slot {
    std::string dll, name;
    ImportFn fn;
};

std::map<std::string, ImportFn>& Handlers() {
    static std::map<std::string, ImportFn> handlers;
    return handlers;
}

std::vector<Slot>& Slots() {
    static std::vector<Slot> slots;
    return slots;
}

std::string Key(const char* dll, const char* name) { return std::string(dll) + "!" + name; }

}  // namespace

void RegisterImports(const ImportDef* defs, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        Handlers()[Key(defs[i].dll, defs[i].name)] = defs[i].fn;
    }
}

uint32_t ImportThunk(const char* dll, const char* name) {
    if (!g_thunk_base) {
        g_thunk_base = PageAlloc(kThunkSize);
        if (!g_thunk_base) Fatal("cannot allocate the import thunk area");
    }
    std::string key = Key(dll, name);
    auto& slots = Slots();
    for (size_t i = 0; i < slots.size(); ++i) {
        if (slots[i].dll == dll && slots[i].name == name) return g_thunk_base + kSlot * uint32_t(i);
    }
    auto h = Handlers().find(key);
    slots.push_back({dll, name, h == Handlers().end() ? nullptr : h->second});
    uint32_t addr = g_thunk_base + kSlot * uint32_t(slots.size() - 1);
    if (strcmp(name, "_HUGE") == 0) {
        WF64(addr, HUGE_VAL);
    }
    return addr;
}

bool DispatchImport(Ctx* ctx, uint32_t target) {
    if (!g_thunk_base || target < g_thunk_base || target >= g_thunk_base + kThunkSize) return false;
    uint32_t index = (target - g_thunk_base) / kSlot;
    auto& slots = Slots();
    if (index >= slots.size() || (target & (kSlot - 1))) {
        Fatal("call into the import area at %08x", target);
    }
    const Slot& s = slots[index];
    if (!s.fn) {
        Fatal("unimplemented import %s!%s (called from %08x, args %08x %08x %08x %08x)", s.dll.c_str(),
              s.name.c_str(), R32(ctx->esp), Arg(ctx, 0), Arg(ctx, 1), Arg(ctx, 2), Arg(ctx, 3));
    }
    s.fn(ctx);
    return true;
}

}  // namespace native
