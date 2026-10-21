#include "cement_library.h"

#include <cstring>

#include "../host/hooks.h"
#include "name_hash.h"

namespace core {

namespace {

// Field offsets of CementLibrary used by the lookup (from FUN_10650ab0).
constexpr uint32_t kOwnerOffset = 0x18;         // object exposing a sub-object at +0x14 with two words
constexpr uint32_t kDriveOffset = 0x20;         // drive the .rcf file lives on, passed to the request
constexpr uint32_t kStateOffset = 0x24;         // 1 = header and directory loaded
constexpr uint32_t kCompactOffset = 0x49;       // nonzero: 12-byte entries, zero: 20-byte entries
constexpr uint32_t kRequestTagOffset = 0x5c;    // word passed through to the request
constexpr uint32_t kEntryCountOffset = 0x60;
constexpr uint32_t kEntriesOffset = 0x64;

constexpr int32_t kStateLoaded = kCementStateLoaded;

// Header copy read by the loader (0x3c bytes at +0x28), and the fields ValidateHeader checks.
constexpr uint32_t kHeaderOffset = 0x28;
constexpr char kMagic[] = "ATG CORE CEMENT LIBRARY";  // compared including its terminating NUL (24 bytes)
static_assert(sizeof(kMagic) == 24, "magic is 24 bytes with NUL");
constexpr uint32_t kVersionMajorOffset = 0x48;
constexpr uint32_t kVersionMinorOffset = kCompactOffset;  // 0x49: 1 = compact entries, 0 = wide
constexpr uint32_t kVersionReservedOffset = 0x4a;
constexpr uint32_t kVersionFlagOffset = 0x4b;
constexpr uint32_t kWideCheckAOffset = 0x18;  // must equal +0x1c for wide-entry archives
constexpr uint32_t kWideCheckBOffset = 0x1c;

// Pending completion object notified by SetState.
constexpr uint32_t kCompletionOffset = 0x70;
constexpr uint32_t kCompletionArgOffset = 0x74;
constexpr int kCompletionSlotNotify = 3;   // vtable +0x0c, takes the stored argument
constexpr int kCompletionSlotRelease = 1;  // vtable +0x04

template <typename T>
void SetField(void* base, uint32_t offset, T value) {
    *reinterpret_cast<T*>(static_cast<uint8_t*>(base) + offset) = value;
}

using NotifyFn = void(__thiscall*)(void* completion, uint32_t arg);
using ReleaseFn = void(__thiscall*)(void* completion);

template <typename Fn>
Fn Slot(void* object, int index) {
    void** vtable = *static_cast<void***>(object);
    return reinterpret_cast<Fn>(vtable[index]);
}

// Owner sub-object (FUN_1064ff10 returns owner + 0x14) and the two words FUN_10650690 copies out of it.
constexpr uint32_t kOwnerSubOffset = 0x14;
constexpr uint32_t kOwnerWordAOffset = 0x2c;
constexpr uint32_t kOwnerWordBOffset = 0x30;

constexpr uint32_t kOpenRangeRva = 0x00651130;

template <typename T>
T Field(const void* base, uint32_t offset) {
    return *reinterpret_cast<const T*>(static_cast<const uint8_t*>(base) + offset);
}

// FUN_10651130 (core::CementFileRangeInit): thiscall, 8 stack arguments (ret 20h). Called through its live engine
// entry point so a test detour or the reconstruction hook installed there is what runs.
using OpenRangeFn = void(__thiscall*)(CementOpenRequest* request, void* drive, uint32_t offset, uint32_t size,
                                      uint32_t entry_size, uint32_t extra, uint32_t owner_word_a,
                                      uint32_t owner_word_b, uint32_t tag);

}  // namespace

bool __thiscall CementLibraryValidateHeader(CementLibrary* self) {
    const auto* header = reinterpret_cast<const uint8_t*>(self) + kHeaderOffset;
    bool ok = memcmp(header, kMagic, sizeof(kMagic)) == 0;
    if (Field<uint8_t>(self, kVersionMajorOffset) != 2 || Field<uint8_t>(self, kVersionFlagOffset) == 0) {
        ok = false;
    }
    const uint8_t minor = Field<uint8_t>(self, kVersionMinorOffset);
    if (minor >= 2) {
        ok = false;
    }
    if (minor == 0 && Field<uint32_t>(self, kWideCheckBOffset) != Field<uint32_t>(self, kWideCheckAOffset)) {
        ok = false;
    }
    if (Field<uint8_t>(self, kVersionReservedOffset) != 0) {
        ok = false;
    }
    return ok;
}

void __thiscall CementLibrarySetState(CementLibrary* self, int32_t state) {
    SetField<int32_t>(self, kStateOffset, state);
    void* completion = Field<void*>(self, kCompletionOffset);
    if (!completion) {
        return;
    }
    Slot<NotifyFn>(completion, kCompletionSlotNotify)(completion, Field<uint32_t>(self, kCompletionArgOffset));
    completion = Field<void*>(self, kCompletionOffset);  // re-read, as the original does after the first call
    Slot<ReleaseFn>(completion, kCompletionSlotRelease)(completion);
    SetField<void*>(self, kCompletionOffset, nullptr);
    SetField<uint32_t>(self, kCompletionArgOffset, 0);
}

const void* CementFindEntry(const void* entries, uint32_t count, uint32_t key, uint32_t stride) {
    if (count == 0) {
        return nullptr;
    }
    const auto* base = static_cast<const uint8_t*>(entries);
    uint32_t lo = 0;
    uint32_t hi = count - 1;
    do {
        uint32_t mid = (hi >> 1) + (lo >> 1);
        if ((lo & 1) && (hi & 1)) {
            ++mid;
        }
        const uint8_t* record = base + mid * stride;
        uint32_t record_key = *reinterpret_cast<const uint32_t*>(record);
        if (record_key == key) {
            return record;
        }
        if (record_key < key) {
            lo = mid + 1;
        } else {
            if (mid == 0) {
                return nullptr;
            }
            hi = mid - 1;
        }
    } while (lo <= hi);
    return nullptr;
}

bool __thiscall CementLibraryFindAndOpen(CementLibrary* self, uint32_t key, CementOpenRequest* request) {
    if (Field<int32_t>(self, kStateOffset) != kStateLoaded) {
        return false;
    }
    const bool compact = Field<uint8_t>(self, kCompactOffset) != 0;
    const uint32_t stride = compact ? sizeof(CementEntryCompact) : sizeof(CementEntryWide);
    const void* found = CementFindEntry(Field<const void*>(self, kEntriesOffset),
                                        Field<uint32_t>(self, kEntryCountOffset), key, stride);
    if (!found) {
        return false;
    }
    const auto* entry = static_cast<const CementEntryWide*>(found);  // compact entries share the first 3 words
    uint32_t size_arg = entry->size;
    uint32_t extra = 0;
    if (!compact) {
        extra = entry->value_10;
        size_arg = entry->value_c;
    }
    const void* owner_sub = Field<const uint8_t*>(self, kOwnerOffset) + kOwnerSubOffset;
    const uint32_t word_a = Field<uint32_t>(owner_sub, kOwnerWordAOffset);
    const uint32_t word_b = Field<uint32_t>(owner_sub, kOwnerWordBOffset);

    auto open_range = reinterpret_cast<OpenRangeFn>(protore::EngineAddress(kOpenRangeRva));
    open_range(request, Field<void*>(self, kDriveOffset), entry->offset, size_arg, entry->size, extra, word_a, word_b,
               Field<uint32_t>(self, kRequestTagOffset));
    return true;
}

bool __thiscall CementLibraryOpenByName(CementLibrary* self, const char* path, CementOpenRequest* request) {
    if (*path == '\\') {
        ++path;
    }
    return CementLibraryFindAndOpen(self, HashFileName(path), request);
}

}  // namespace core
