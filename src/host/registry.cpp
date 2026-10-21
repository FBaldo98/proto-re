#include "registry.h"

namespace protore {

namespace {

// Entries are added as functions are reconstructed: {name, rva, replacement, expected bytes, len, enabled}.
Hook g_hooks[] = {
    {"<none>", 0, nullptr, {}, 0, false},
};

}  // namespace

Hook* Registry(size_t* count) {
    *count = sizeof(g_hooks) / sizeof(g_hooks[0]);
    return g_hooks;
}

}  // namespace protore
