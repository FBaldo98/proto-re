// Native replacements of hot engine functions (tools/recomp/recomp.py NATIVE_OVERRIDES): the generated f_<addr>
// forwards to Native_<addr>. Each one follows the original's calling convention and leaves callee-saved registers
// and the x87 stack as the original did.
//
// Audio DSP (the engine's software mixer, the busiest code on phones): written as plain float C - the original
// ran x87 in single-precision mode and SSE, so per-operation float rounding matches; FP contraction is off to keep
// multiply/add pairs unfused.
#include <cmath>
#include <cstring>

#include "guest.h"

#pragma STDC FP_CONTRACT OFF
#pragma clang fp contract(off)

namespace native {

namespace {

// Engine addresses in this file are the DLL's (preferred base 0x10000000); the engine is loaded at kEngineBase.
inline uint32_t E(uint32_t a) { return a - 0x10000000u + kEngineBase; }

inline float F(uint32_t a) { return RF32(a); }
inline const float* FP(uint32_t a) { return Ptr<const float>(a); }

}  // namespace

// FUN_105e8860 (cdecl, 4 args): resamples each channel of a voice into a 256-sample block (linear interpolation,
// 20.12 fixed-point phase over a power-of-two ring buffer), optionally meters it, and mixes it into the output
// bins with the voice/bin/output gains.
//   p1: mix context (+0xc output count, +0x10 focused output)
//   p2: voice (+0x42 meter flag, +0x44/+0x488/+0x48c/+0x4a8 gains, +0x48 rate, +0x94 channels, +0x98 ring size,
//       +0xa8 64-bit phase, +0xb0 per-channel gain, +0xf8 bin gains, +0x118 per-channel bin gains,
//       +0x3a0/+0x358 bin/output masks (per channel, +4 each), +0x3e8 output gains, +0x4b0 per-channel meter,
//       +0x4f8 first sample)
//   p3: source channels (stride 24, ring buffer pointer at +4)
//   p4: output bins (pointer per bin, 8 per output)
extern "C" void Native_105e8860(Ctx* c) {
    const uint32_t esp = c->esp;
    const uint32_t p1 = R32(esp + 4), p2 = R32(esp + 8), p3 = R32(esp + 12), p4 = R32(esp + 16);

    const float rate = F(p2 + 0x48) * F(E(0x10e2e9dc));
    const int64_t step = (rate != rate || rate >= 9223372036854775808.0f || rate < -9223372036854775808.0f)
                             ? INT64_MIN
                             : int64_t(rate);  // _ftol2: truncation
    const uint32_t mask = R32(p2 + 0x98) - 1;
    const uint32_t channels = R32(p2 + 0x94);
    const float frac_scale = F(E(0x10e2e864));

    float block[259] = {};  // the original reads (and mixes) up to 3 floats past the 256 it fills
    for (uint32_t ch = 0; ch < channels; ++ch) {
        const uint32_t meter = p2 + 0x4b0 + 4 * ch;
        const float gain = F(meter - 0x400);
        const float* ring = FP(R32(p3 + 4 + 24 * ch));
        int64_t phase = int64_t(R64(p2 + 0xa8));
        for (int i = 0; i < 256; ++i) {
            const float frac = float(uint32_t(phase) & 0xfff) * frac_scale;
            const uint32_t i0 = uint32_t(uint64_t(phase) >> 12) & mask;
            phase += step;
            const float a = (1.0f - frac) * ring[i0];
            const float b = ring[(i0 + 1) & mask] * frac;
            block[i] = a + b;
        }
        if (R8(p2 + 0x42) == 1) {  // FUN_105e85b0: RMS of the block
            float sum = 0.0f;
            for (int i = 0; i < 256; ++i) sum = block[i] * block[i] + sum;  // SSE, strictly in order
            const float scaled = sum * F(E(0x10e313a4));                   // x87, single precision
            WF32(meter, std::sqrt(scaled));
        }
        const uint32_t outputs = R32(p1 + 0xc);
        for (uint32_t o = 0; o < outputs; ++o) {
            const float out_gain = R32(p1 + 0x10) == o ? F(p2 + 0x48c) : F(p2 + 0x488);
            const float og = (out_gain * F(p2 + 0x4a8)) * F(p2 + 0x44);
            const float out_level = F(p2 + 1000 + 4 * o);
            for (uint32_t bin = 0; bin < 8; ++bin) {
                if (!(R32(meter - 0x110) & (1u << bin)) || !(R32(meter - 0x158) & (1u << (o & 31)))) continue;
                const float v = ((((F(p2 + 0x118 + (bin + ch * 8) * 4) * F(p2 + 0xf8 + 0x350 + bin * 4)) * out_level) *
                                  F(p2 + 0xf8 + bin * 4)) * og) * gain;
                if (!(0.0f < v)) continue;
                const uint32_t first = R32(p2 + 0x4f8);
                if (first >= 0x100) continue;
                float* dst = Ptr<float>(R32(p4 + (bin + o * 8) * 4));
                const uint32_t groups = ((0xff - first) >> 2) + 1;
                for (uint32_t k = first; k < first + groups * 4; ++k) dst[k] = block[k] * v + dst[k];
            }
        }
    }
    c->ret_target = Pop32(c);  // cdecl
}

}  // namespace native
