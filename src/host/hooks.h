// Hook-and-replace infrastructure: each reconstructed function is registered with the RVA of the original
// in prototypeenginef.dll and the original's first bytes, so a build mismatch is detected instead of
// silently patching the wrong code.
#pragma once

#include <cstddef>
#include <cstdint>

namespace protore {

// Engine build this table was made for (PE TimeDateStamp of prototypeenginef.dll, Steam build 19788432).
constexpr uint32_t kEngineTimeDateStamp = 0x689E2026;
// Preferred image base; Ghidra addresses are kEngineImageBase + RVA.
constexpr uintptr_t kEngineImageBase = 0x10000000;

struct Hook {
    const char* name;        // reconstructed symbol, e.g. "core::HashName"
    uint32_t rva;            // original function RVA (Ghidra address - kEngineImageBase)
    void* replacement;       // reconstructed implementation
    uint8_t expected[8];     // original first bytes, checked before patching
    size_t expected_len;
    bool enabled;            // disabled hooks stay in the table for differential testing
};

// Returns pointer to the original function in the loaded engine (for differential tests).
void* EngineAddress(uint32_t rva);

// Patches every enabled hook with a JMP rel32; returns number installed, logs each failure.
int InstallHooks(Hook* hooks, size_t count);

void Log(const char* fmt, ...);

}  // namespace protore
