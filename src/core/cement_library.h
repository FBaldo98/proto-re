// core::CementLibrary — an .rcf archive mounted as a drive (code\core\src\file\common\cementer.cpp).
// Reconstructed so far: directory lookup and open-by-name. The object is still allocated and laid out by the
// original engine, so it is accessed through field offsets observed in the disassembly.
#pragma once

#include <cstdint>

namespace core {

struct CementLibrary;      // opaque: engine object, fields read via offsets in cement_library.cpp
struct CementOpenRequest;  // opaque: engine request object that receives the file's byte range

// Directory entry formats; the library's "compact" flag (+0x49) selects one.
struct CementEntryCompact {
    uint32_t key;     // core::HashFileName of the path; directory is sorted by key (unsigned)
    uint32_t offset;  // byte offset of the file inside the .rcf
    uint32_t size;
};
static_assert(sizeof(CementEntryCompact) == 12, "compact entry is 12 bytes");

struct CementEntryWide {
    uint32_t key;
    uint32_t offset;
    uint32_t size;
    uint32_t value_c;   // passed to the request in place of `size` (meaning not yet known)
    uint32_t value_10;  // passed to the request as an extra word, 0 for compact entries
};
static_assert(sizeof(CementEntryWide) == 20, "wide entry is 20 bytes");

// Load states stored at +0x24 (observed: FindAndOpen requires 1; the loader writes 1 and 2).
enum CementLibraryState : int32_t {
    kCementStateLoaded = 1,
    kCementStateFailed = 2,
};

// Original: FUN_106503c0 (rva 0x006503c0), thiscall, no stack arguments. Validates the 0x3c-byte .rcf header that
// the loader reads to +0x28: the 24-byte magic "ATG CORE CEMENT LIBRARY\0", version bytes at +0x48..+0x4b
// (major must be 2, +0x4b nonzero, minor at +0x49 below 2, +0x4a zero) and, for minor 0 (wide entries), that the
// words at +0x18 and +0x1c are equal. Every check is evaluated; the result is their conjunction (in al).
bool __thiscall CementLibraryValidateHeader(CementLibrary* self);

// Original: FUN_10650410 (rva 0x00650410), thiscall, ret 4. Stores the state; if a completion object is pending at
// +0x70, calls its slot 3 with the word at +0x74, then its slot 1, and clears both fields.
void __thiscall CementLibrarySetState(CementLibrary* self, int32_t state);

// Original: FUN_106502c0 (rva 0x006502c0), cdecl. Binary search over `count` records of `stride` bytes whose
// first word is an unsigned key; returns the matching record or null. The midpoint is computed as
// (hi >> 1) + (lo >> 1) + (lo & hi & 1) and the probe order must be kept: with duplicate keys it decides which
// record is returned.
const void* CementFindEntry(const void* entries, uint32_t count, uint32_t key, uint32_t stride);

// Original: FUN_10650ab0 (rva 0x00650ab0), thiscall, ret 8. If the directory is loaded (state 1), finds `key` and
// hands the file's byte range to `request` (FUN_10651130); returns true when found.
bool __thiscall CementLibraryFindAndOpen(CementLibrary* self, uint32_t key, CementOpenRequest* request);

// Original: FUN_10650cf0 (rva 0x00650cf0), thiscall, ret 8. Drops one leading '\' (HashFileName drops another),
// hashes the path and calls CementLibraryFindAndOpen. Returns its result (the original leaves it in al).
bool __thiscall CementLibraryOpenByName(CementLibrary* self, const char* path, CementOpenRequest* request);

}  // namespace core
