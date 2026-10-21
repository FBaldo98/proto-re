/* Runtime for statically recompiled engine code (tools/recomp/recomp.py output).
 *
 * Every recompiled function has the signature `void f_<va>(Ctx*)` and works on an explicit guest CPU state.
 * Guest memory is addressed with 32-bit guest addresses relative to RecompMemoryBase: 0 when running inside the
 * original 32-bit process (prototype, hooked into the engine), a reserved 4 GiB region on 64-bit hosts. */
#ifndef PROTORE_RECOMP_RUNTIME_H
#define PROTORE_RECOMP_RUNTIME_H

#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Ctx {
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
    uint8_t cf, zf, sf, of, pf, df;
    uint8_t pad[2];
    uint32_t ret_target;  /* guest return address popped by the last `ret`, used by the native bridge */
    /* x87 stack (prototype precision: double), top index and status bits */
    double st[8];
    uint32_t st_top;
    uint16_t fpu_sw, fpu_cw;
    /* SSE registers */
    union {
        float f[4];
        double d[2];
        uint32_t u32[4];
        uint64_t u64[2];
    } xmm[8];
} Ctx;

extern uint8_t* RecompMemoryBase;

typedef void (*RecompFn)(Ctx*);

/* Calls guest address `target`: a recompiled function when one exists, otherwise the host fallback (the
 * original native code in the prototype). The caller has already pushed the return address. */
void RecompDispatch(Ctx* ctx, uint32_t target);

/* Looks up the recompiled implementation of a guest function entry, or null. */
RecompFn RecompLookup(uint32_t target);

/* ---- memory ---- */
static inline uint8_t* G(uint32_t a) { return RecompMemoryBase + a; }
static inline uint8_t R8(uint32_t a) { return *G(a); }
static inline uint16_t R16(uint32_t a) { uint16_t v; memcpy(&v, G(a), 2); return v; }
static inline uint32_t R32(uint32_t a) { uint32_t v; memcpy(&v, G(a), 4); return v; }
static inline uint64_t R64(uint32_t a) { uint64_t v; memcpy(&v, G(a), 8); return v; }
static inline float RF32(uint32_t a) { float v; memcpy(&v, G(a), 4); return v; }
static inline double RF64(uint32_t a) { double v; memcpy(&v, G(a), 8); return v; }
static inline void W8(uint32_t a, uint8_t v) { *G(a) = v; }
static inline void W16(uint32_t a, uint16_t v) { memcpy(G(a), &v, 2); }
static inline void W32(uint32_t a, uint32_t v) { memcpy(G(a), &v, 4); }
static inline void W64(uint32_t a, uint64_t v) { memcpy(G(a), &v, 8); }
static inline void WF32(uint32_t a, float v) { memcpy(G(a), &v, 4); }
static inline void WF64(uint32_t a, double v) { memcpy(G(a), &v, 8); }

static inline void Push32(Ctx* c, uint32_t v) { c->esp -= 4; W32(c->esp, v); }
static inline uint32_t Pop32(Ctx* c) { uint32_t v = R32(c->esp); c->esp += 4; return v; }

/* ---- flags ---- */
static inline uint8_t Parity(uint32_t v) {
    v &= 0xff;
    v ^= v >> 4;
    v ^= v >> 2;
    v ^= v >> 1;
    return (uint8_t)(~v & 1);
}

#define RECOMP_FLAGS_LOGIC(c, r, bits)                                   \
    do {                                                                 \
        (c)->cf = 0;                                                     \
        (c)->of = 0;                                                     \
        (c)->zf = ((r) & (uint32_t)((1ull << (bits)) - 1)) == 0;         \
        (c)->sf = (((r) >> ((bits) - 1)) & 1);                           \
        (c)->pf = Parity(r);                                             \
    } while (0)

static inline uint32_t Mask(int bits) { return bits == 32 ? 0xffffffffu : ((1u << bits) - 1); }

static inline uint32_t AddFlags(Ctx* c, uint32_t a, uint32_t b, uint32_t carry, int bits) {
    uint64_t wide = (uint64_t)(a & Mask(bits)) + (b & Mask(bits)) + carry;
    uint32_t r = (uint32_t)wide & Mask(bits);
    uint32_t sign = 1u << (bits - 1);
    c->cf = (wide >> bits) & 1;
    c->zf = r == 0;
    c->sf = (r & sign) != 0;
    c->of = ((~(a ^ b) & (a ^ r)) & sign) != 0;
    c->pf = Parity(r);
    return r;
}

static inline uint32_t SubFlags(Ctx* c, uint32_t a, uint32_t b, uint32_t borrow, int bits) {
    uint32_t am = a & Mask(bits), bm = b & Mask(bits);
    uint64_t wide = (uint64_t)am - bm - borrow;
    uint32_t r = (uint32_t)wide & Mask(bits);
    uint32_t sign = 1u << (bits - 1);
    c->cf = (uint64_t)am < (uint64_t)bm + borrow;
    c->zf = r == 0;
    c->sf = (r & sign) != 0;
    c->of = (((a ^ b) & (a ^ r)) & sign) != 0;
    c->pf = Parity(r);
    return r;
}

/* ---- host fallbacks provided by the embedding ---- */
void RecompUnimplemented(Ctx* ctx, uint32_t va, const char* what);

#ifdef __cplusplus
}
#endif

#endif
