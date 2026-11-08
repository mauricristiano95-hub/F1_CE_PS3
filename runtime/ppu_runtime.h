// Core runtime interface shared by the generated PPU code and the HLE/LV2 layers.
#pragma once
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <immintrin.h>

using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
using s8 = int8_t;
using s16 = int16_t;
using s32 = int32_t;
using s64 = int64_t;

#if defined(_MSC_VER) && !defined(__clang__)
#error "Build with clang (llvm-mingw on Windows, zig cc on Linux)"
#endif

#define FORCE_INLINE inline __attribute__((always_inline))

// ------------------------------------------------------------------ guest memory
// The whole 32-bit PS3 effective address space is reserved in one host block.
extern u8* g_base;

FORCE_INLINE u8* gmem(u32 ea) { return g_base + ea; }

FORCE_INLINE u8 ld8(u32 ea) { return *gmem(ea); }
FORCE_INLINE u16 ld16(u32 ea) { u16 v; memcpy(&v, gmem(ea), 2); return __builtin_bswap16(v); }
FORCE_INLINE u32 ld32(u32 ea) { u32 v; memcpy(&v, gmem(ea), 4); return __builtin_bswap32(v); }
FORCE_INLINE u64 ld64(u32 ea) { u64 v; memcpy(&v, gmem(ea), 8); return __builtin_bswap64(v); }
// Debug watchpoint: stores to the 128-byte line g_watch_line (F1CE_WATCH_LINE) are logged.
extern u32 g_watch_line;
void watch_store(u32 ea, u64 value, u32 size);
#ifdef F1CE_TRACE_PC
#define WATCH_STORE(ea, v, sz) do { if (__builtin_expect(((ea) & ~127u) == g_watch_line, 0)) watch_store(ea, v, sz); } while (0)
#else
#define WATCH_STORE(ea, v, sz) ((void)0)
#endif
FORCE_INLINE void st8(u32 ea, u8 v) { WATCH_STORE(ea, v, 1); *gmem(ea) = v; }
FORCE_INLINE void st16(u32 ea, u16 v) { WATCH_STORE(ea, v, 2); v = __builtin_bswap16(v); memcpy(gmem(ea), &v, 2); }
FORCE_INLINE void st32(u32 ea, u32 v) { WATCH_STORE(ea, v, 4); v = __builtin_bswap32(v); memcpy(gmem(ea), &v, 4); }
FORCE_INLINE void st64(u32 ea, u64 v) { WATCH_STORE(ea, v, 8); v = __builtin_bswap64(v); memcpy(gmem(ea), &v, 8); }
FORCE_INLINE u16 ld16le(u32 ea) { u16 v; memcpy(&v, gmem(ea), 2); return v; }
FORCE_INLINE u32 ld32le(u32 ea) { u32 v; memcpy(&v, gmem(ea), 4); return v; }
FORCE_INLINE u64 ld64le(u32 ea) { u64 v; memcpy(&v, gmem(ea), 8); return v; }
FORCE_INLINE void st16le(u32 ea, u16 v) { memcpy(gmem(ea), &v, 2); }
FORCE_INLINE void st32le(u32 ea, u32 v) { memcpy(gmem(ea), &v, 4); }
FORCE_INLINE void st64le(u32 ea, u64 v) { memcpy(gmem(ea), &v, 8); }
FORCE_INLINE float ldf32(u32 ea) { u32 v = ld32(ea); float f; memcpy(&f, &v, 4); return f; }
FORCE_INLINE void stf32(u32 ea, float f) { u32 v; memcpy(&v, &f, 4); st32(ea, v); }

// Big-endian value stored in guest memory (for HLE structure definitions).
template <typename T>
struct be_t {
    T raw;
    static T swap(T v) {
        if constexpr (sizeof(T) == 1) return v;
        else if constexpr (sizeof(T) == 2) { u16 x; memcpy(&x, &v, 2); x = __builtin_bswap16(x); memcpy(&v, &x, 2); return v; }
        else if constexpr (sizeof(T) == 4) { u32 x; memcpy(&x, &v, 4); x = __builtin_bswap32(x); memcpy(&v, &x, 4); return v; }
        else { u64 x; memcpy(&x, &v, 8); x = __builtin_bswap64(x); memcpy(&v, &x, 8); return v; }
    }
    operator T() const { return swap(raw); }
    be_t& operator=(T v) { raw = swap(v); return *this; }
};
template <typename T> FORCE_INLINE T* gptr(u32 ea) { return reinterpret_cast<T*>(gmem(ea)); }

// ------------------------------------------------------------------ CPU state
union alignas(16) v128 {
    u8 b[16]; u16 h[8]; u32 w[4]; u64 d[2]; s8 sb[16]; s16 sh[8]; s32 sw[4]; float f[4];
    __m128 ps; __m128i pi;
};

union FPR { double d; u64 u; };

struct PPUThread;

struct PPUContext {
    u64 r[32];
    FPR f[32];
    v128 v[32];
    u8 cr[32];            // one byte per CR bit (bit i = field i/4, lt/gt/eq/so)
    u64 lr, ctr;
    u8 xer_so, xer_ov, xer_ca, xer_cnt;
    u32 fpscr;
    u32 vscr;
    u32 vrsave;
    u32 pc;               // last syscall/trap address, for diagnostics
    u32 cur_fn;           // guest address of the function being executed, for diagnostics
    // reservation for lwarx/stwcx.
    u32 res_addr;
    u64 res_value;
    PPUThread* thread;
};

// Bring-up aid: record the guest address of each memory access/call so faults report the exact
// PPU instruction. Compiled out unless F1CE_TRACE_PC is defined.
#ifdef F1CE_TRACE_PC
#define PPU_PC(a) (c.pc = (a))
#else
#define PPU_PC(a) ((void)0)
#endif

// The context never lies in guest memory: __restrict tells the compiler that guest loads and stores (char pointers
// that may alias anything) leave it alone, so guest registers stay in host registers across memory accesses. The same
// freedom lets it hoist guest loads out of loops, so backward branch targets carry PPU_LOOP: spin loops on memory
// shared with other threads must read it again. The barrier does not touch the context while c has not escaped.
#define PPU_DECL(name) void name(PPUContext& __restrict c)
#define PPU_FUNC(name) void name(PPUContext& __restrict c)
#define PPU_LOOP() __asm__ volatile("" : : : "memory")
// Head of a loop that only polls memory written by the SPUs (tools/recomp.py SPIN_LOOPS: the job manager's waits for job
// counters): after a short spin it blocks until an SPU writes the counters' line (see LineWait), so a waiting thread
// takes no core.
void ppu_spin(PPUContext& c);   // core.cpp
#define PPU_SPIN() ppu_spin(c)

// Blocking waits on guest memory (core.cpp). A waiting thread registers up to two 128-byte lines; writers of guest memory
// from other threads (SPU DMA puts and atomics, PPU atomics, HLE updates: everything that calls line_written) call
// guest_written, which wakes the waiters of an overlapping line. Plain PPU stores do not: waits time out after ~1 ms.
extern std::atomic<u32> g_line_waiters;
void guest_written_slow(u32 ea, u32 size);
FORCE_INLINE void guest_written(u32 ea, u32 size) {
    std::atomic_thread_fence(std::memory_order_seq_cst);   // (the write before, the registration check after)
    if (__builtin_expect(g_line_waiters.load(std::memory_order_relaxed) != 0, 0)) guest_written_slow(ea, size);
}
struct LineWait {   // one per wait site; the slot it registers in belongs to the thread (one active wait per thread)
    u32 line0 = ~0u, line1 = ~0u;
    u32 seq = 0;
    bool armed = false;
};
// arm: registers the lines (a new value of seq); the caller must then check its condition once more before block
void line_wait_arm(LineWait& w, u32 line0, u32 line1);
// block: sleeps until a write to the lines since arm, or the timeout; re-arms (check the condition again afterwards).
// Returns whether a write woke it (false: timeout, or it only registered again).
bool line_wait_block(LineWait& w, u32 timeout_ms);
void line_wait_release(LineWait& w);
void line_wait_thread_exit();   // returns the thread's slot (PPU thread exit)
using PPUFunc = void (*)(PPUContext&);

struct FuncEntry { u32 addr; PPUFunc fn; };
struct ImportInfo { const char* library; const char* name; u32 nid; u32 slot; };
extern const FuncEntry g_func_table[];
extern const u32 g_func_count;
extern const ImportInfo g_imports[];
extern const u32 g_import_count;

// ------------------------------------------------------------------ runtime services
void call_indirect(PPUContext& c, u32 addr);
void hle_call(PPUContext& c, u32 import_index);
void ppu_syscall(PPUContext& c);
[[noreturn]] void trap_invalid(PPUContext& c, u32 addr, u32 word);
[[noreturn]] void trap_unsupported(PPUContext& c, u32 addr, const char* what);
[[noreturn]] void trap_hit(PPUContext& c, u32 addr);
u64 ppu_timebase();
u32 ppu_lwarx(PPUContext& c, u32 ea);
u64 ppu_ldarx(PPUContext& c, u32 ea);
void ppu_stwcx(PPUContext& c, u32 ea, u32 v);
void ppu_stdcx(PPUContext& c, u32 ea, u64 v);

// ------------------------------------------------------------------ inline helpers
FORCE_INLINE u64 rotl64(u64 x, u32 n) { n &= 63; return n ? (x << n) | (x >> (64 - n)) : x; }
FORCE_INLINE u64 rotl32x(u32 x, u32 n) {  // 32-bit rotate replicated into both halves (PPC64 rlw*)
    n &= 31;
    u32 r = n ? (x << n) | (x >> (32 - n)) : x;
    return ((u64)r << 32) | r;
}
FORCE_INLINE u64 clz32(u32 x) { return x ? __builtin_clz(x) : 32; }
FORCE_INLINE u64 clz64(u64 x) { return x ? __builtin_clzll(x) : 64; }

template <typename T>
FORCE_INLINE void set_cr_cmp(PPUContext& c, int f, T a, T b) {
    u8* p = &c.cr[f * 4];
    p[0] = a < b; p[1] = a > b; p[2] = a == b; p[3] = c.xer_so;
}
FORCE_INLINE void set_cr_s(PPUContext& c, int f, s64 v) { set_cr_cmp<s64>(c, f, v, 0); }
FORCE_INLINE void set_cr_fcmp(PPUContext& c, int f, double a, double b) {
    u8* p = &c.cr[f * 4];
    bool un = std::isnan(a) || std::isnan(b);
    p[0] = !un && a < b; p[1] = !un && a > b; p[2] = !un && a == b; p[3] = un;
}
FORCE_INLINE u64 ppu_get_cr(PPUContext& c) {
    u32 v = 0;
    for (int i = 0; i < 32; i++) v |= (u32)(c.cr[i] & 1) << (31 - i);
    return v;
}
FORCE_INLINE void ppu_set_cr(PPUContext& c, u32 v, u32 crm) {
    for (int f = 0; f < 8; f++)
        if (crm & (0x80 >> f))
            for (int k = 0; k < 4; k++) c.cr[f * 4 + k] = (v >> (31 - (f * 4 + k))) & 1;
}
FORCE_INLINE void ppu_mcrf(PPUContext& c, int d, int s) { memmove(&c.cr[d * 4], &c.cr[s * 4], 4); }
FORCE_INLINE u64 ppu_get_xer(PPUContext& c) {
    return ((u64)c.xer_so << 31) | ((u64)c.xer_ov << 30) | ((u64)c.xer_ca << 29) | c.xer_cnt;
}
FORCE_INLINE void ppu_set_xer(PPUContext& c, u64 v) {
    c.xer_so = (v >> 31) & 1; c.xer_ov = (v >> 30) & 1; c.xer_ca = (v >> 29) & 1; c.xer_cnt = v & 0x7F;
}
FORCE_INLINE s32 ppu_divw(s32 a, s32 b) { return (b == 0 || (a == INT32_MIN && b == -1)) ? 0 : a / b; }
FORCE_INLINE s64 ppu_divd(s64 a, s64 b) { return (b == 0 || (a == INT64_MIN && b == -1)) ? 0 : a / b; }
FORCE_INLINE bool ppu_trap_cond(u32 to, s64 a, s64 b) {
    return ((to & 16) && a < b) || ((to & 8) && a > b) || ((to & 4) && a == b) ||
           ((to & 2) && (u64)a < (u64)b) || ((to & 1) && (u64)a > (u64)b);
}
FORCE_INLINE s32 ppu_fctiw(double x, bool trunc) {
    if (std::isnan(x)) return INT32_MIN;
    double r = trunc ? std::trunc(x) : std::nearbyint(x);
    if (r >= 2147483647.0) return INT32_MAX;
    if (r <= -2147483648.0) return INT32_MIN;
    return (s32)r;
}
FORCE_INLINE s64 ppu_fctid(double x, bool trunc) {
    if (std::isnan(x)) return INT64_MIN;
    double r = trunc ? std::trunc(x) : std::nearbyint(x);
    if (r >= 9223372036854775807.0) return INT64_MAX;
    if (r <= -9223372036854775808.0) return INT64_MIN;
    return (s64)r;
}
FORCE_INLINE u32 ppu_mtfsf(u32 fpscr, u32 v, u32 fm) {
    u32 m = 0;
    for (int i = 0; i < 8; i++) if (fm & (0x80 >> i)) m |= 0xF0000000u >> (i * 4);
    return (fpscr & ~m) | (v & m);
}
FORCE_INLINE u32 ppu_mtfsfi(u32 fpscr, u32 field, u32 imm) {
    u32 sh = 28 - field * 4;
    return (fpscr & ~(0xFu << sh)) | (imm << sh);
}
FORCE_INLINE void ppu_mcrfs(PPUContext& c, int d, int s) {
    for (int k = 0; k < 4; k++) c.cr[d * 4 + k] = (c.fpscr >> (31 - (s * 4 + k))) & 1;
}
FORCE_INLINE void ppu_fp_rc(PPUContext& c) { for (int k = 0; k < 4; k++) c.cr[4 + k] = (c.fpscr >> (31 - k)) & 1; }
// sync/lwsync: full fence (x86 needs it only for store->load ordering, but it also stops compiler
// reordering); eieio/isync: compiler barrier (x86 TSO already keeps store->store and load->load order)
#define PPU_SYNC() std::atomic_thread_fence(std::memory_order_seq_cst)
#define PPU_EIEIO() std::atomic_signal_fence(std::memory_order_seq_cst)
FORCE_INLINE void ppu_dcbz(u32 ea) { memset(gmem(ea & ~127u), 0, 128); }

#include "vmx.h"
