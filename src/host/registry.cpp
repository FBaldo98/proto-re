#include "registry.h"

#include "../core/report.h"
#include "../math/vector_trace.h"

namespace protore {

namespace {

// Entries are added as functions are reconstructed: {name, rva, replacement, expected bytes, len, enabled}.
Hook g_hooks[] = {
    {"core::Report", 0x0063c750, reinterpret_cast<void*>(&core::Report),
     {0x8b, 0x4c, 0x24, 0x18, 0x8b, 0x54, 0x24, 0x14}, 8, false},  // disabled: see re/issues.md #1
    {"math::TraceVector", 0x007cea20, reinterpret_cast<void*>(&math::TraceVector),
     {0x83, 0xec, 0x0c, 0x80, 0x3d, 0xcd, 0xb9, 0x1b}, 8, true},
};

}  // namespace

Hook* Registry(size_t* count) {
    *count = sizeof(g_hooks) / sizeof(g_hooks[0]);
    return g_hooks;
}

}  // namespace protore
