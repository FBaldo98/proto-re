#include "hooks.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace protore {

namespace {

HMODULE g_engine = nullptr;
FILE* g_log = nullptr;

HMODULE Engine() {
    if (!g_engine) {
        g_engine = GetModuleHandleA("prototypeenginef.dll");
    }
    return g_engine;
}

}  // namespace

void Log(const char* fmt, ...) {
    if (!g_log) {
        // Append, and tag every line with the process id: the game may run more than one process.
        g_log = fopen("proto-re.log", "a");
        if (!g_log) {
            return;
        }
    }
    fprintf(g_log, "[%lu] ", GetCurrentProcessId());
    va_list args;
    va_start(args, fmt);
    vfprintf(g_log, fmt, args);
    va_end(args);
    fputc('\n', g_log);
    fflush(g_log);
}

void* EngineAddress(uint32_t rva) {
    HMODULE engine = Engine();
    return engine ? reinterpret_cast<uint8_t*>(engine) + rva : nullptr;
}

int InstallHooks(Hook* hooks, size_t count) {
    // PROTORE_ONLY=<name> installs just that hook, for bisecting behaviour changes to one function.
    char only[128] = "";
    GetEnvironmentVariableA("PROTORE_ONLY", only, sizeof(only));
    int installed = 0;
    for (size_t i = 0; i < count; ++i) {
        Hook& h = hooks[i];
        if (!h.enabled || (only[0] && strcmp(only, h.name) != 0)) {
            continue;
        }
        auto* target = static_cast<uint8_t*>(EngineAddress(h.rva));
        if (!target) {
            Log("hook %s: engine not loaded", h.name);
            continue;
        }
        if (memcmp(target, h.expected, h.expected_len) != 0) {
            Log("hook %s: bytes at rva 0x%08x do not match, skipped", h.name, h.rva);
            continue;
        }
        DWORD old_protect;
        if (!VirtualProtect(target, 5, PAGE_EXECUTE_READWRITE, &old_protect)) {
            Log("hook %s: VirtualProtect failed (%lu)", h.name, GetLastError());
            continue;
        }
        // PROTORE_NULLPATCH=1 performs the same protect/write/restore sequence but writes back the original
        // bytes, to separate effects of touching the code page from effects of the redirected function.
        if (GetEnvironmentVariableA("PROTORE_NULLPATCH", nullptr, 0) != 0) {
            uint8_t same[5];
            memcpy(same, target, sizeof(same));
            memcpy(target, same, sizeof(same));
        } else {
            int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(h.replacement) -
                                               (reinterpret_cast<uintptr_t>(target) + 5));
            target[0] = 0xE9;
            memcpy(target + 1, &rel, sizeof(rel));
        }
        VirtualProtect(target, 5, old_protect, &old_protect);
        FlushInstructionCache(GetCurrentProcess(), target, 5);
        Log("hook %s: installed at rva 0x%08x", h.name, h.rva);
        ++installed;
    }
    return installed;
}

}  // namespace protore
