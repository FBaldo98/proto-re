#include "file_system.h"

#include "../host/hooks.h"

namespace core {

namespace {

constexpr uint32_t kIoBufferSizeRva = 0x0127ca14;

// Engine entry points called through their live addresses (reconstructed or not).
constexpr uint32_t kFileSystemIoBufferSizeRva = 0x0064a130;
constexpr uint32_t kRoundUpRva = 0x00645300;
constexpr uint32_t kAllocRva = 0x0063a8d0;

using IoBufferSizeFn = uint32_t(__cdecl*)();
using RoundUpFn = uint32_t(__cdecl*)(uint32_t, uint32_t);
using AllocFn = void*(__cdecl*)(uint32_t, uint32_t, uint32_t);

template <typename Fn>
Fn Engine(uint32_t rva) {
    return reinterpret_cast<Fn>(protore::EngineAddress(rva));
}

}  // namespace

uint32_t FileSystemIoBufferSize() {
    return *static_cast<uint32_t*>(protore::EngineAddress(kIoBufferSizeRva));
}

void __thiscall IoBufferHolderAcquire(IoBufferHolder* self, uint32_t heap, uint32_t alignment, uint32_t granularity) {
    if (self->references == 0) {
        self->heap = heap;
        uint32_t size = Engine<RoundUpFn>(kRoundUpRva)(Engine<IoBufferSizeFn>(kFileSystemIoBufferSizeRva)(),
                                                       granularity);
        self->size = size;
        self->buffer = Engine<AllocFn>(kAllocRva)(self->heap, size, alignment);
    }
    ++self->references;
}

}  // namespace core
