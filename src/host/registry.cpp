#include "registry.h"

#include "../core/file_system.h"
#include "../core/memory.h"
#include "../core/cement_file_range.h"
#include "../core/cement_library.h"
#include "../core/name_hash.h"
#include "../core/report.h"
#include "../math/vector_trace.h"

namespace protore {

namespace {

// Entries are added as functions are reconstructed:
// {name, rva, replacement, expected first bytes, expected len, stolen len, enabled}.
Hook g_hooks[] = {
    // mov ecx,[esp+18h]; mov edx,[esp+14h]
    {"core::Report", 0x0063c750, reinterpret_cast<void*>(&core::Report),
     {0x8b, 0x4c, 0x24, 0x18, 0x8b, 0x54, 0x24, 0x14}, 8, 8, false, {}},  // disabled: see re/issues.md #1
    // sub esp,0Ch; cmp byte ptr [111BB9CDh],0
    {"math::TraceVector", 0x007cea20, reinterpret_cast<void*>(&math::TraceVector),
     {0x83, 0xec, 0x0c, 0x80, 0x3d, 0xcd, 0xb9, 0x1b, 0x11, 0x00}, 10, 10, true, {}},
    // mov ecx,[esp+4]; test ecx,ecx
    {"core::HashString", 0x0063a5d0, reinterpret_cast<void*>(&core::HashString),
     {0x8b, 0x4c, 0x24, 0x04, 0x85, 0xc9, 0x74, 0x3d}, 8, 6, true, {}},
    // mov eax,[esp+4]; cmp byte ptr [eax],5Ch
    {"core::HashFileName", 0x00643ed0, reinterpret_cast<void*>(&core::HashFileName),
     {0x8b, 0x44, 0x24, 0x04, 0x80, 0x38, 0x5c, 0x75, 0x03}, 9, 7, true, {}},
    // push ecx; mov ecx,[esp+0Ch]
    {"core::CementFindEntry", 0x006502c0, reinterpret_cast<void*>(&core::CementFindEntry),
     {0x51, 0x8b, 0x4c, 0x24, 0x0c, 0x33, 0xc0}, 7, 5, true, {}},
    // push ecx; push edi; mov edi,ecx; cmp dword ptr [edi+24h],1
    {"core::CementLibraryFindAndOpen", 0x00650ab0, reinterpret_cast<void*>(&core::CementLibraryFindAndOpen),
     {0x51, 0x57, 0x8b, 0xf9, 0x83, 0x7f, 0x24, 0x01}, 8, 8, true, {}},
    // mov eax,[esp+4]; cmp byte ptr [eax],5Ch
    {"core::CementLibraryOpenByName", 0x00650cf0, reinterpret_cast<void*>(&core::CementLibraryOpenByName),
     {0x8b, 0x44, 0x24, 0x04, 0x80, 0x38, 0x5c, 0x56}, 8, 7, true, {}},
    // push ebx; mov ebx,[esp+8]
    {"core::CementFileRangeInit", 0x00651130, reinterpret_cast<void*>(&core::CementFileRangeInit),
     {0x53, 0x8b, 0x5c, 0x24, 0x08, 0x8b, 0x03}, 7, 5, true, {}},
    // push ebx; push esi; push edi; mov edx,ecx
    {"core::CementLibraryValidateHeader", 0x006503c0, reinterpret_cast<void*>(&core::CementLibraryValidateHeader),
     {0x53, 0x56, 0x57, 0x8b, 0xd1, 0x33, 0xdb}, 7, 5, true, {}},
    // mov eax,[esp+4]; push esi
    {"core::CementLibrarySetState", 0x00650410, reinterpret_cast<void*>(&core::CementLibrarySetState),
     {0x8b, 0x44, 0x24, 0x04, 0x56, 0x8b, 0xf1}, 7, 5, true, {}},
    // push ebx; push ebp; push esi; push edi; mov edi,[esp+1Ch]
    {"core::Alloc", 0x0063a8d0, reinterpret_cast<void*>(&core::Alloc),
     {0x53, 0x55, 0x56, 0x57, 0x8b, 0x7c, 0x24, 0x1c}, 8, 8, true, {}},
    // mov ecx,[esp+8]; test ecx,ecx
    {"core::RoundUp", 0x00645300, reinterpret_cast<void*>(&core::RoundUp),
     {0x8b, 0x4c, 0x24, 0x08, 0x85, 0xc9, 0x56}, 7, 6, true, {}},
    // mov eax,[1127CA14h]
    {"core::FileSystemIoBufferSize", 0x0064a130, reinterpret_cast<void*>(&core::FileSystemIoBufferSize),
     {0xa1, 0x14, 0xca, 0x27, 0x11, 0xc3}, 6, 5, true, {}},
    // push esi; mov esi,ecx; cmp dword ptr [esi+34h],0
    {"core::IoBufferHolderAcquire", 0x006506b0, reinterpret_cast<void*>(&core::IoBufferHolderAcquire),
     {0x56, 0x8b, 0xf1, 0x83, 0x7e, 0x34, 0x00}, 7, 7, true, {}},
};

}  // namespace

Hook* Registry(size_t* count) {
    *count = sizeof(g_hooks) / sizeof(g_hooks[0]);
    return g_hooks;
}

}  // namespace protore
