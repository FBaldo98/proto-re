// core file system globals and the shared I/O buffer that drives use for reads.
#pragma once

#include <cstdint>

namespace core {

// Original: FUN_1064a130 (rva 0x0064a130), no arguments. Returns the file system's I/O buffer size, the third
// argument given to file system initialisation (FUN_10649f90) and stored at rva 0x0127ca14.
uint32_t FileSystemIoBufferSize();

// Shared I/O buffer, embedded at +0x14 in a drive; FindAndOpen hands (buffer, size) to every file range.
struct IoBufferHolder {
    uint8_t unknown[0x2c];
    void* buffer;         // +0x2c
    uint32_t size;        // +0x30
    int32_t references;   // +0x34
    uint32_t heap;        // +0x38
};
static_assert(sizeof(IoBufferHolder) == 0x3c, "IoBufferHolder layout");

// Original: FUN_106506b0 (rva 0x006506b0), thiscall, ret 0Ch. On the first reference allocates the buffer from
// `heap`: size = RoundUp(FileSystemIoBufferSize(), granularity), Alloc(heap, size, alignment). Always counts the
// reference.
void __thiscall IoBufferHolderAcquire(IoBufferHolder* self, uint32_t heap, uint32_t alignment, uint32_t granularity);

}  // namespace core
