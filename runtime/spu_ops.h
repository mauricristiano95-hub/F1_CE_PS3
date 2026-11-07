// SPU instruction semantics (helpers + one handler per instruction), shared by the interpreter (spu.cpp) and
// the ahead-of-time translated blocks (generated/spu_aot*.cpp), which call the handlers with constant words.
#pragma once
#include "spu.h"

#include <cmath>
#include <cstdlib>
#include <cfloat>
#include <vector>
#include <algorithm>
#include <atomic>
#include <type_traits>

using u128 = unsigned __int128;

namespace {

FORCE_INLINE __m128i rev() { return _mm_set_epi8(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15); }
FORCE_INLINE u128 q(const v128& v) { u128 x; memcpy(&x, &v, 16); return x; }
FORCE_INLINE void setq(v128& v, u128 x) { memcpy(&v, &x, 16); }
FORCE_INLINE u8& Bb(v128& v, int i) { return v.b[15 - i]; }      // BE byte i
FORCE_INLINE u8 Bb(const v128& v, int i) { return v.b[15 - i]; }
FORCE_INLINE s32 sext(u32 v, int bits) { return (s32)(v << (32 - bits)) >> (32 - bits); }

// instruction fields
FORCE_INLINE int RT(u32 w) { return w & 0x7F; }
FORCE_INLINE int RA(u32 w) { return (w >> 7) & 0x7F; }
FORCE_INLINE int RB(u32 w) { return (w >> 14) & 0x7F; }
FORCE_INLINE int RRR_RT(u32 w) { return (w >> 21) & 0x7F; }
FORCE_INLINE int RRR_RC(u32 w) { return w & 0x7F; }
FORCE_INLINE s32 I7(u32 w) { return sext((w >> 14) & 0x7F, 7); }
FORCE_INLINE s32 I10(u32 w) { return sext((w >> 14) & 0x3FF, 10); }
FORCE_INLINE s32 I16(u32 w) { return sext((w >> 7) & 0xFFFF, 16); }
FORCE_INLINE u32 U16(u32 w) { return (w >> 7) & 0xFFFF; }
FORCE_INLINE u32 I18(u32 w) { return (w >> 7) & 0x3FFFF; }
FORCE_INLINE u32 I8(u32 w) { return (w >> 14) & 0xFF; }


// Handlers are templates over the register context S: SPUThread (interpreter, single-block translations) or SpuLocal
// (cluster translations: a register file on the host stack that the compiler keeps in host registers, see below).
// SPU_AOT_FAST (generated translation units): no debug hooks in loads/stores, handlers forced inline
#ifdef SPU_AOT_FAST
#define SPU_DEBUG_HOOKS 0
#define OP(name) template <class S> [[maybe_unused]] static inline __attribute__((always_inline)) void op_##name(S& s, u32 w)
#else
#define SPU_DEBUG_HOOKS 1
#define OP(name) template <class S> [[maybe_unused]] static inline void op_##name(S& s, u32 w)
#endif
#define R(n) s.gpr[n]

// Register context of cluster translations: gpr holds local copies of the SPU registers the cluster uses (copied in
// on entry, back on exit and around channel/stop instructions, which run on the thread itself). It never escapes,
// so stores into the local store cannot alias it and register values stay in host registers between instructions.
struct SpuLocal {
    v128 gpr[128];
    SPUThread& t;
    u8* ls;
    u32 pc = 0;
    bool* line_code;
    u64& code_writes;
    u32& srr0;
    bool& interrupts;
    explicit SpuLocal(SPUThread& th)
        : t(th), ls(th.ls), line_code(th.line_code), code_writes(th.code_writes), srr0(th.srr0),
          interrupts(th.interrupts) {}
    u32 pref(int r) const { return gpr[r].w[3]; }
    void set_pref(int r, u32 v) { gpr[r].pi = _mm_setzero_si128(); gpr[r].w[3] = v; }
    // inlined: an out-of-line call would take L's address, and L would then live in memory (every register access a
    // load or store, ~2.5 ns per SPU instruction)
    FORCE_INLINE void code_hit(u32 l) { t.code_hit(l); }
};

// ------------------------------------------------------------------ memory
void trace_ls(SPUThread& s, const char* op, u32 a, int rt) {
    if (s.trace_budget == 0) return;
    s.trace_budget--;
    LOG_I("SPU %s: %s ls[%05x] r%d=%08x %08x %08x %08x at %05x", s.name.c_str(), op, a & 0x3FFF0, rt, s.gpr[rt].w[3],
          s.gpr[rt].w[2], s.gpr[rt].w[1], s.gpr[rt].w[0], s.pc);
}
template <class S> FORCE_INLINE void lq(S& s, int rt, u32 a) {
    R(rt).pi = _mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)(s.ls + (a & 0x3FFF0))), rev());
    if constexpr (std::is_same_v<S, SPUThread>)
        if (SPU_DEBUG_HOOKS && (a & 0x3FFF0) < s.trace_ls_limit) trace_ls(s, "load ", a, rt);
}
// debug: F1CE_SPU_LS_WATCH=<ls addr>[,<ls addr>...] logs SPU stores to those quadwords (and DMA GETs covering them)
std::vector<u32> g_ls_watch = [] {
    std::vector<u32> v;
    if (const char* e = getenv("F1CE_SPU_LS_WATCH"))
        for (const char* p = e; *p;) { v.push_back((u32)strtoul(p, nullptr, 16) & 0x3FFF0); while (*p && *p != ',') p++; if (*p) p++; }
    return v;
}();
void ls_watch_hit(SPUThread& s, u32 a, int rt) {
    static std::atomic<int> n{0};
    if (n++ < 300)
        LOG_I("ls watch %s store [%05x] = %08x %08x %08x %08x pc=%05x", s.name.c_str(), a, s.gpr[rt].w[3], s.gpr[rt].w[2],
              s.gpr[rt].w[1], s.gpr[rt].w[0], s.pc);
}
template <class S> FORCE_INLINE void stq(S& s, int rt, u32 a) {
    if constexpr (std::is_same_v<S, SPUThread>) {
        if (SPU_DEBUG_HOOKS && (a & 0x3FFF0) < s.trace_ls_limit) trace_ls(s, "store", a, rt);
        if (SPU_DEBUG_HOOKS && __builtin_expect(!g_ls_watch.empty(), 0))
            for (u32 x : g_ls_watch) if (x == (a & 0x3FFF0)) ls_watch_hit(s, x, rt);
        if (SPU_DEBUG_HOOKS && __builtin_expect(s.lsw_base != 0, 0) && (a & 0x3FFF0) - s.lsw_base < 0x800) {
            u32 k = ((a & 0x3FFF0) - s.lsw_base) >> 4;
            s.lsw_pc[k] = s.pc;
            s.lsw_val[k] = s.gpr[rt].w[3];
        }
    }
    __m128i v = _mm_shuffle_epi8(R(rt).pi, rev());
    __m128i* p = (__m128i*)(s.ls + (a & 0x3FFF0));
    if (__builtin_expect(s.line_code[(a >> 7) & 2047], 0)) {
        // store into decoded code: invalidate only when the bytes change. The job manager's interrupt handler
        // writes the same 80 register-save instructions onto its stack at every interrupt; counting those as code
        // changes revalidated every block of the SPU (and emptied its dispatch cache) after each interrupt.
        if (_mm_movemask_epi8(_mm_cmpeq_epi8(_mm_loadu_si128(p), v)) == 0xFFFF) return;
        _mm_storeu_si128(p, v);
        // the line's version is stale; the line counts as code again only when a block there is checked. Lines stayed
        // marked forever: the stack, where that handler generates its
        // code, then invalidated on every ordinary store (~7 million times per second per SPU)
        s.code_hit((a >> 7) & 2047);
        return;
    }
    _mm_storeu_si128(p, v);
}
OP(lqd) { lq(s, RT(w), s.pref(RA(w)) + I10(w) * 16); }
OP(lqx) { lq(s, RT(w), s.pref(RA(w)) + s.pref(RB(w))); }
OP(lqa) { lq(s, RT(w), (u32)(I16(w) << 2)); }
OP(lqr) { lq(s, RT(w), s.pc + (I16(w) << 2)); }
OP(stqd) { stq(s, RT(w), s.pref(RA(w)) + I10(w) * 16); }
OP(stqx) { stq(s, RT(w), s.pref(RA(w)) + s.pref(RB(w))); }
OP(stqa) { stq(s, RT(w), (u32)(I16(w) << 2)); }
OP(stqr) { stq(s, RT(w), s.pc + (I16(w) << 2)); }

// insertion controls: default pattern 0x10..0x1F with the selected element replaced
// (computed with vector compares: a variable byte index into the register would keep cluster registers in memory)
template <class S> FORCE_INLINE void gen_ctrl(S& s, int rt, u32 t, int size) {
    u32 pos = t & (16 - size) & 15;
    __m128i be = rev();   // BE byte index of each host lane
    __m128i rel = _mm_sub_epi8(be, _mm_set1_epi8((char)pos));
    __m128i in = _mm_cmpeq_epi8(_mm_min_epu8(rel, _mm_set1_epi8((char)(size - 1))), rel);   // pos <= i < pos + size
    // element bytes: byte 3 (size 1), bytes 2-3 (size 2), 0-3 (size 4), 0-7 (size 8) of the preferred slot
    __m128i val = _mm_add_epi8(rel, _mm_set1_epi8((char)(size == 1 ? 3 : size == 2 ? 2 : 0)));
    R(rt).pi = _mm_blendv_epi8(_mm_add_epi8(be, _mm_set1_epi8(0x10)), val, in);
}
OP(cbd) { gen_ctrl(s, RT(w), s.pref(RA(w)) + I7(w), 1); }
OP(chd) { gen_ctrl(s, RT(w), s.pref(RA(w)) + I7(w), 2); }
OP(cwd) { gen_ctrl(s, RT(w), s.pref(RA(w)) + I7(w), 4); }
OP(cdd) { gen_ctrl(s, RT(w), s.pref(RA(w)) + I7(w), 8); }
OP(cbx) { gen_ctrl(s, RT(w), s.pref(RA(w)) + s.pref(RB(w)), 1); }
OP(chx) { gen_ctrl(s, RT(w), s.pref(RA(w)) + s.pref(RB(w)), 2); }
OP(cwx) { gen_ctrl(s, RT(w), s.pref(RA(w)) + s.pref(RB(w)), 4); }
OP(cdx) { gen_ctrl(s, RT(w), s.pref(RA(w)) + s.pref(RB(w)), 8); }

// ------------------------------------------------------------------ immediates
OP(il) { R(RT(w)).pi = _mm_set1_epi32(I16(w)); }
OP(ilh) { R(RT(w)).pi = _mm_set1_epi16((short)U16(w)); }
OP(ilhu) { R(RT(w)).pi = _mm_set1_epi32((int)(U16(w) << 16)); }
OP(iohl) { R(RT(w)).pi = _mm_or_si128(R(RT(w)).pi, _mm_set1_epi32((int)U16(w))); }
OP(ila) { R(RT(w)).pi = _mm_set1_epi32((int)I18(w)); }
OP(fsmbi) {
    u32 m = U16(w);
    for (int i = 0; i < 16; i++) Bb(R(RT(w)), i) = (m >> (15 - i)) & 1 ? 0xFF : 0;
}

// ------------------------------------------------------------------ integer arithmetic
OP(a) { R(RT(w)).pi = _mm_add_epi32(R(RA(w)).pi, R(RB(w)).pi); }
OP(ah) { R(RT(w)).pi = _mm_add_epi16(R(RA(w)).pi, R(RB(w)).pi); }
OP(ai) { R(RT(w)).pi = _mm_add_epi32(R(RA(w)).pi, _mm_set1_epi32(I10(w))); }
OP(ahi) { R(RT(w)).pi = _mm_add_epi16(R(RA(w)).pi, _mm_set1_epi16((short)I10(w))); }
OP(sf) { R(RT(w)).pi = _mm_sub_epi32(R(RB(w)).pi, R(RA(w)).pi); }
OP(sfh) { R(RT(w)).pi = _mm_sub_epi16(R(RB(w)).pi, R(RA(w)).pi); }
OP(sfi) { R(RT(w)).pi = _mm_sub_epi32(_mm_set1_epi32(I10(w)), R(RA(w)).pi); }
OP(sfhi) { R(RT(w)).pi = _mm_sub_epi16(_mm_set1_epi16((short)I10(w)), R(RA(w)).pi); }
OP(addx) {
    v128 r; for (int i = 0; i < 4; i++) r.w[i] = R(RA(w)).w[i] + R(RB(w)).w[i] + (R(RT(w)).w[i] & 1); R(RT(w)) = r;
}
OP(sfx) {
    v128 r; for (int i = 0; i < 4; i++) r.w[i] = R(RB(w)).w[i] - R(RA(w)).w[i] - (1 - (R(RT(w)).w[i] & 1)); R(RT(w)) = r;
}
OP(cg) {
    v128 r; for (int i = 0; i < 4; i++) r.w[i] = ((u64)R(RA(w)).w[i] + R(RB(w)).w[i]) >> 32; R(RT(w)) = r;
}
OP(bg) {
    v128 r; for (int i = 0; i < 4; i++) r.w[i] = R(RB(w)).w[i] >= R(RA(w)).w[i]; R(RT(w)) = r;
}
OP(cgx) {
    v128 r; for (int i = 0; i < 4; i++) r.w[i] = ((u64)R(RA(w)).w[i] + R(RB(w)).w[i] + (R(RT(w)).w[i] & 1)) >> 32; R(RT(w)) = r;
}
OP(bgx) {
    v128 r;
    for (int i = 0; i < 4; i++) {
        s64 d = (s64)R(RB(w)).w[i] - R(RA(w)).w[i] - (1 - (R(RT(w)).w[i] & 1));
        r.w[i] = d >= 0;
    }
    R(RT(w)) = r;
}
OP(mpy) { v128 r; for (int i = 0; i < 4; i++) r.sw[i] = (s32)(s16)R(RA(w)).w[i] * (s16)R(RB(w)).w[i]; R(RT(w)) = r; }
OP(mpyu) { v128 r; for (int i = 0; i < 4; i++) r.w[i] = (u32)(u16)R(RA(w)).w[i] * (u16)R(RB(w)).w[i]; R(RT(w)) = r; }
OP(mpyi) { v128 r; for (int i = 0; i < 4; i++) r.sw[i] = (s32)(s16)R(RA(w)).w[i] * I10(w); R(RT(w)) = r; }
OP(mpyui) { v128 r; for (int i = 0; i < 4; i++) r.w[i] = (u32)(u16)R(RA(w)).w[i] * (u16)I10(w); R(RT(w)) = r; }
OP(mpya) {
    v128 r;
    for (int i = 0; i < 4; i++) r.sw[i] = (s32)(s16)R(RA(w)).w[i] * (s16)R(RB(w)).w[i] + R(RRR_RC(w)).sw[i];
    R(RRR_RT(w)) = r;
}
OP(mpyh) { v128 r; for (int i = 0; i < 4; i++) r.w[i] = ((R(RA(w)).w[i] >> 16) * (u16)R(RB(w)).w[i]) << 16; R(RT(w)) = r; }
OP(mpys) { v128 r; for (int i = 0; i < 4; i++) r.sw[i] = ((s32)(s16)R(RA(w)).w[i] * (s16)R(RB(w)).w[i]) >> 16; R(RT(w)) = r; }
OP(mpyhh) { v128 r; for (int i = 0; i < 4; i++) r.sw[i] = (s32)(s16)(R(RA(w)).w[i] >> 16) * (s16)(R(RB(w)).w[i] >> 16); R(RT(w)) = r; }
OP(mpyhha) { for (int i = 0; i < 4; i++) R(RT(w)).sw[i] += (s32)(s16)(R(RA(w)).w[i] >> 16) * (s16)(R(RB(w)).w[i] >> 16); }
OP(mpyhhu) { v128 r; for (int i = 0; i < 4; i++) r.w[i] = (R(RA(w)).w[i] >> 16) * (R(RB(w)).w[i] >> 16); R(RT(w)) = r; }
OP(mpyhhau) { for (int i = 0; i < 4; i++) R(RT(w)).w[i] += (R(RA(w)).w[i] >> 16) * (R(RB(w)).w[i] >> 16); }
OP(clz) { v128 r; for (int i = 0; i < 4; i++) r.w[i] = R(RA(w)).w[i] ? __builtin_clz(R(RA(w)).w[i]) : 32; R(RT(w)) = r; }
OP(cntb) { v128 r; for (int i = 0; i < 16; i++) r.b[i] = (u8)__builtin_popcount(R(RA(w)).b[i]); R(RT(w)) = r; }
OP(fsmb) {
    u32 m = s.pref(RA(w)) & 0xFFFF; v128 r;
    for (int i = 0; i < 16; i++) Bb(r, i) = (m >> (15 - i)) & 1 ? 0xFF : 0;
    R(RT(w)) = r;
}
OP(fsmh) {
    u32 m = s.pref(RA(w)) & 0xFF; v128 r;
    for (int i = 0; i < 8; i++) r.h[7 - i] = (m >> (7 - i)) & 1 ? 0xFFFF : 0;
    R(RT(w)) = r;
}
OP(fsm) {
    u32 m = s.pref(RA(w)) & 0xF; v128 r;
    for (int i = 0; i < 4; i++) r.w[3 - i] = (m >> (3 - i)) & 1 ? 0xFFFFFFFF : 0;
    R(RT(w)) = r;
}
OP(gb) { u32 m = 0; for (int i = 0; i < 4; i++) m |= (R(RA(w)).w[3 - i] & 1) << (3 - i); s.set_pref(RT(w), m); }
OP(gbh) { u32 m = 0; for (int i = 0; i < 8; i++) m |= (R(RA(w)).h[7 - i] & 1) << (7 - i); s.set_pref(RT(w), m); }
OP(gbb) { u32 m = 0; for (int i = 0; i < 16; i++) m |= (Bb(R(RA(w)), i) & 1) << (15 - i); s.set_pref(RT(w), m); }
OP(avgb) { R(RT(w)).pi = _mm_avg_epu8(R(RA(w)).pi, R(RB(w)).pi); }
OP(absdb) { v128 r; for (int i = 0; i < 16; i++) r.b[i] = (u8)std::abs((int)R(RA(w)).b[i] - (int)R(RB(w)).b[i]); R(RT(w)) = r; }
OP(sumb) {
    v128 r;
    for (int i = 0; i < 4; i++) {
        u32 sa = 0, sb = 0;
        for (int k = 0; k < 4; k++) { sa += R(RA(w)).b[i * 4 + k]; sb += R(RB(w)).b[i * 4 + k]; }
        r.w[i] = (sb << 16) | (sa & 0xFFFF);   // BE halfword 0 = sum of rb, halfword 1 = sum of ra
    }
    R(RT(w)) = r;
}
OP(xsbh) { v128 r; for (int i = 0; i < 8; i++) r.sh[i] = (s8)(R(RA(w)).h[i] & 0xFF); R(RT(w)) = r; }
OP(xshw) { v128 r; for (int i = 0; i < 4; i++) r.sw[i] = (s16)(R(RA(w)).w[i] & 0xFFFF); R(RT(w)) = r; }
OP(xswd) { v128 r; for (int i = 0; i < 2; i++) r.d[i] = (u64)(s64)(s32)(R(RA(w)).d[i] & 0xFFFFFFFF); R(RT(w)) = r; }

// ------------------------------------------------------------------ logical
OP(and_) { R(RT(w)).pi = _mm_and_si128(R(RA(w)).pi, R(RB(w)).pi); }
OP(andc) { R(RT(w)).pi = _mm_andnot_si128(R(RB(w)).pi, R(RA(w)).pi); }
OP(andi) { R(RT(w)).pi = _mm_and_si128(R(RA(w)).pi, _mm_set1_epi32(I10(w))); }
OP(andhi) { R(RT(w)).pi = _mm_and_si128(R(RA(w)).pi, _mm_set1_epi16((short)I10(w))); }
OP(andbi) { R(RT(w)).pi = _mm_and_si128(R(RA(w)).pi, _mm_set1_epi8((char)I10(w))); }
OP(or_) { R(RT(w)).pi = _mm_or_si128(R(RA(w)).pi, R(RB(w)).pi); }
OP(orc) { R(RT(w)).pi = _mm_or_si128(R(RA(w)).pi, _mm_xor_si128(R(RB(w)).pi, _mm_set1_epi32(-1))); }
OP(ori) { R(RT(w)).pi = _mm_or_si128(R(RA(w)).pi, _mm_set1_epi32(I10(w))); }
OP(orhi) { R(RT(w)).pi = _mm_or_si128(R(RA(w)).pi, _mm_set1_epi16((short)I10(w))); }
OP(orbi) { R(RT(w)).pi = _mm_or_si128(R(RA(w)).pi, _mm_set1_epi8((char)I10(w))); }
OP(xor_) { R(RT(w)).pi = _mm_xor_si128(R(RA(w)).pi, R(RB(w)).pi); }
OP(xori) { R(RT(w)).pi = _mm_xor_si128(R(RA(w)).pi, _mm_set1_epi32(I10(w))); }
OP(xorhi) { R(RT(w)).pi = _mm_xor_si128(R(RA(w)).pi, _mm_set1_epi16((short)I10(w))); }
OP(xorbi) { R(RT(w)).pi = _mm_xor_si128(R(RA(w)).pi, _mm_set1_epi8((char)I10(w))); }
OP(orx) { const v128& a = R(RA(w)); s.set_pref(RT(w), a.w[0] | a.w[1] | a.w[2] | a.w[3]); }
OP(nand) { R(RT(w)).pi = _mm_xor_si128(_mm_and_si128(R(RA(w)).pi, R(RB(w)).pi), _mm_set1_epi32(-1)); }
OP(nor) { R(RT(w)).pi = _mm_xor_si128(_mm_or_si128(R(RA(w)).pi, R(RB(w)).pi), _mm_set1_epi32(-1)); }
OP(eqv) { R(RT(w)).pi = _mm_xor_si128(_mm_xor_si128(R(RA(w)).pi, R(RB(w)).pi), _mm_set1_epi32(-1)); }
OP(selb) {
    const v128& c = R(RRR_RC(w));
    R(RRR_RT(w)).pi = _mm_or_si128(_mm_and_si128(c.pi, R(RB(w)).pi), _mm_andnot_si128(c.pi, R(RA(w)).pi));
}
OP(shufb) {
    // byte-reversed layout: host lane j selects source byte (c & 31): index (c & 15) ^ 15 in a (bit 4 clear) or b;
    // control bytes with bit 7 set give 0x00 (10xxxxxx), 0xFF (110xxxxx) or 0x80 (111xxxxx)
    __m128i c = R(RRR_RC(w)).pi;
    __m128i x = _mm_xor_si128(_mm_and_si128(c, _mm_set1_epi8(0x1F)), _mm_set1_epi8(15));   // bit 4 clear: a, set: b
    // + 0x70 moves bit 4 into bit 7, where pshufb gives 0; bit 7 of c also zeroes the byte
    __m128i top = _mm_and_si128(c, _mm_set1_epi8((char)0x80));
    __m128i ia = _mm_or_si128(_mm_add_epi8(x, _mm_set1_epi8(0x70)), top);
    __m128i ib = _mm_or_si128(_mm_add_epi8(_mm_xor_si128(x, _mm_set1_epi8(0x10)), _mm_set1_epi8(0x70)), top);
    __m128i r = _mm_or_si128(_mm_shuffle_epi8(R(RA(w)).pi, ia), _mm_shuffle_epi8(R(RB(w)).pi, ib));
    // 0x80 is done; 110xxxxx and 111xxxxx give 0xFF and 0x80 (rare, but seen in masks)
    __m128i hi = _mm_cmpgt_epi8(_mm_set1_epi8((char)0xC0), c);     // c < -64: 0x80..0xBF, else all clear below
    __m128i sp = _mm_andnot_si128(hi, _mm_cmpgt_epi8(_mm_setzero_si128(), c));   // c in 0xC0..0xFF
    if (__builtin_expect(!_mm_testz_si128(sp, sp), 0)) {
        __m128i e = _mm_and_si128(c, _mm_set1_epi8((char)0xE0));
        __m128i v = _mm_or_si128(_mm_cmpeq_epi8(e, _mm_set1_epi8((char)0xC0)),
                                 _mm_and_si128(_mm_cmpeq_epi8(e, _mm_set1_epi8((char)0xE0)), _mm_set1_epi8((char)0x80)));
        r = _mm_blendv_epi8(r, v, sp);
    }
    R(RRR_RT(w)).pi = r;
}

// ------------------------------------------------------------------ shifts / rotates (per element)
OP(shl) { v128 r; for (int i = 0; i < 4; i++) { u32 n = R(RB(w)).w[i] & 63; r.w[i] = n > 31 ? 0 : R(RA(w)).w[i] << n; } R(RT(w)) = r; }
OP(shli) { u32 n = I7(w) & 63; v128 r; for (int i = 0; i < 4; i++) r.w[i] = n > 31 ? 0 : R(RA(w)).w[i] << n; R(RT(w)) = r; }
OP(shlh) { v128 r; for (int i = 0; i < 8; i++) { u32 n = R(RB(w)).h[i] & 31; r.h[i] = n > 15 ? 0 : (u16)(R(RA(w)).h[i] << n); } R(RT(w)) = r; }
OP(shlhi) { u32 n = I7(w) & 31; v128 r; for (int i = 0; i < 8; i++) r.h[i] = n > 15 ? 0 : (u16)(R(RA(w)).h[i] << n); R(RT(w)) = r; }
FORCE_INLINE u32 rotl(u32 x, u32 n) { n &= 31; return n ? (x << n) | (x >> (32 - n)) : x; }
FORCE_INLINE u16 rotl16(u16 x, u32 n) { n &= 15; return (u16)(n ? (x << n) | (x >> (16 - n)) : x); }
OP(rot) { v128 r; for (int i = 0; i < 4; i++) r.w[i] = rotl(R(RA(w)).w[i], R(RB(w)).w[i]); R(RT(w)) = r; }
OP(roti) { v128 r; for (int i = 0; i < 4; i++) r.w[i] = rotl(R(RA(w)).w[i], (u32)I7(w)); R(RT(w)) = r; }
OP(roth) { v128 r; for (int i = 0; i < 8; i++) r.h[i] = rotl16(R(RA(w)).h[i], R(RB(w)).h[i]); R(RT(w)) = r; }
OP(rothi) { v128 r; for (int i = 0; i < 8; i++) r.h[i] = rotl16(R(RA(w)).h[i], (u32)I7(w)); R(RT(w)) = r; }
OP(rotm) { v128 r; for (int i = 0; i < 4; i++) { u32 n = (0 - R(RB(w)).w[i]) & 63; r.w[i] = n > 31 ? 0 : R(RA(w)).w[i] >> n; } R(RT(w)) = r; }
OP(rotmi) { u32 n = (0 - I7(w)) & 63; v128 r; for (int i = 0; i < 4; i++) r.w[i] = n > 31 ? 0 : R(RA(w)).w[i] >> n; R(RT(w)) = r; }
OP(rothm) { v128 r; for (int i = 0; i < 8; i++) { u32 n = (0 - R(RB(w)).h[i]) & 31; r.h[i] = n > 15 ? 0 : R(RA(w)).h[i] >> n; } R(RT(w)) = r; }
OP(rothmi) { u32 n = (0 - I7(w)) & 31; v128 r; for (int i = 0; i < 8; i++) r.h[i] = n > 15 ? 0 : R(RA(w)).h[i] >> n; R(RT(w)) = r; }
OP(rotma) { v128 r; for (int i = 0; i < 4; i++) { u32 n = (0 - R(RB(w)).w[i]) & 63; r.sw[i] = R(RA(w)).sw[i] >> (n > 31 ? 31 : n); } R(RT(w)) = r; }
OP(rotmai) { u32 n = (0 - I7(w)) & 63; v128 r; for (int i = 0; i < 4; i++) r.sw[i] = R(RA(w)).sw[i] >> (n > 31 ? 31 : n); R(RT(w)) = r; }
OP(rotmah) { v128 r; for (int i = 0; i < 8; i++) { u32 n = (0 - R(RB(w)).h[i]) & 31; r.sh[i] = (s16)(R(RA(w)).sh[i] >> (n > 15 ? 15 : n)); } R(RT(w)) = r; }
OP(rotmahi) { u32 n = (0 - I7(w)) & 31; v128 r; for (int i = 0; i < 8; i++) r.sh[i] = (s16)(R(RA(w)).sh[i] >> (n > 15 ? 15 : n)); R(RT(w)) = r; }

// ------------------------------------------------------------------ quadword shifts / rotates
FORCE_INLINE u128 qshl(u128 x, u32 n) { return n >= 128 ? 0 : x << n; }
FORCE_INLINE u128 qshr(u128 x, u32 n) { return n >= 128 ? 0 : x >> n; }
FORCE_INLINE u128 qrot(u128 x, u32 n) { n &= 127; return n ? (x << n) | (x >> (128 - n)) : x; }
OP(shlqbi) { setq(R(RT(w)), qshl(q(R(RA(w))), s.pref(RB(w)) & 7)); }
OP(shlqbii) { setq(R(RT(w)), qshl(q(R(RA(w))), (u32)I7(w) & 7)); }
OP(shlqby) { setq(R(RT(w)), qshl(q(R(RA(w))), (s.pref(RB(w)) & 31) * 8)); }
OP(shlqbyi) { setq(R(RT(w)), qshl(q(R(RA(w))), ((u32)I7(w) & 31) * 8)); }
OP(shlqbybi) { setq(R(RT(w)), qshl(q(R(RA(w))), ((s.pref(RB(w)) >> 3) & 31) * 8)); }
OP(rotqbi) { setq(R(RT(w)), qrot(q(R(RA(w))), s.pref(RB(w)) & 7)); }
OP(rotqbii) { setq(R(RT(w)), qrot(q(R(RA(w))), (u32)I7(w) & 7)); }
OP(rotqby) { setq(R(RT(w)), qrot(q(R(RA(w))), (s.pref(RB(w)) & 15) * 8)); }
OP(rotqbyi) { setq(R(RT(w)), qrot(q(R(RA(w))), ((u32)I7(w) & 15) * 8)); }
OP(rotqbybi) { setq(R(RT(w)), qrot(q(R(RA(w))), ((s.pref(RB(w)) >> 3) & 15) * 8)); }
OP(rotqmbi) { setq(R(RT(w)), qshr(q(R(RA(w))), (0 - s.pref(RB(w))) & 7)); }
OP(rotqmbii) { setq(R(RT(w)), qshr(q(R(RA(w))), (0 - (u32)I7(w)) & 7)); }
OP(rotqmby) { setq(R(RT(w)), qshr(q(R(RA(w))), ((0 - s.pref(RB(w))) & 31) * 8)); }
OP(rotqmbyi) { setq(R(RT(w)), qshr(q(R(RA(w))), ((0 - (u32)I7(w)) & 31) * 8)); }
OP(rotqmbybi) { setq(R(RT(w)), qshr(q(R(RA(w))), ((0 - (s.pref(RB(w)) >> 3)) & 31) * 8)); }

// ------------------------------------------------------------------ compares
OP(ceq) { R(RT(w)).pi = _mm_cmpeq_epi32(R(RA(w)).pi, R(RB(w)).pi); }
OP(ceqh) { R(RT(w)).pi = _mm_cmpeq_epi16(R(RA(w)).pi, R(RB(w)).pi); }
OP(ceqb) { R(RT(w)).pi = _mm_cmpeq_epi8(R(RA(w)).pi, R(RB(w)).pi); }
OP(ceqi) { R(RT(w)).pi = _mm_cmpeq_epi32(R(RA(w)).pi, _mm_set1_epi32(I10(w))); }
OP(ceqhi) { R(RT(w)).pi = _mm_cmpeq_epi16(R(RA(w)).pi, _mm_set1_epi16((short)I10(w))); }
OP(ceqbi) { R(RT(w)).pi = _mm_cmpeq_epi8(R(RA(w)).pi, _mm_set1_epi8((char)I10(w))); }
OP(cgt) { R(RT(w)).pi = _mm_cmpgt_epi32(R(RA(w)).pi, R(RB(w)).pi); }
OP(cgth) { R(RT(w)).pi = _mm_cmpgt_epi16(R(RA(w)).pi, R(RB(w)).pi); }
OP(cgtb) { R(RT(w)).pi = _mm_cmpgt_epi8(R(RA(w)).pi, R(RB(w)).pi); }
OP(cgti) { R(RT(w)).pi = _mm_cmpgt_epi32(R(RA(w)).pi, _mm_set1_epi32(I10(w))); }
OP(cgthi) { R(RT(w)).pi = _mm_cmpgt_epi16(R(RA(w)).pi, _mm_set1_epi16((short)I10(w))); }
OP(cgtbi) { R(RT(w)).pi = _mm_cmpgt_epi8(R(RA(w)).pi, _mm_set1_epi8((char)I10(w))); }
FORCE_INLINE __m128i ucmpgt32(__m128i a, __m128i b) { __m128i s = _mm_set1_epi32((int)0x80000000); return _mm_cmpgt_epi32(_mm_xor_si128(a, s), _mm_xor_si128(b, s)); }
FORCE_INLINE __m128i ucmpgt16(__m128i a, __m128i b) { __m128i s = _mm_set1_epi16((short)0x8000); return _mm_cmpgt_epi16(_mm_xor_si128(a, s), _mm_xor_si128(b, s)); }
FORCE_INLINE __m128i ucmpgt8(__m128i a, __m128i b) { __m128i s = _mm_set1_epi8((char)0x80); return _mm_cmpgt_epi8(_mm_xor_si128(a, s), _mm_xor_si128(b, s)); }
OP(clgt) { R(RT(w)).pi = ucmpgt32(R(RA(w)).pi, R(RB(w)).pi); }
OP(clgth) { R(RT(w)).pi = ucmpgt16(R(RA(w)).pi, R(RB(w)).pi); }
OP(clgtb) { R(RT(w)).pi = ucmpgt8(R(RA(w)).pi, R(RB(w)).pi); }
OP(clgti) { R(RT(w)).pi = ucmpgt32(R(RA(w)).pi, _mm_set1_epi32(I10(w))); }
OP(clgthi) { R(RT(w)).pi = ucmpgt16(R(RA(w)).pi, _mm_set1_epi16((short)I10(w))); }
OP(clgtbi) { R(RT(w)).pi = ucmpgt8(R(RA(w)).pi, _mm_set1_epi8((char)I10(w))); }

// halt-if (treated as fatal: indicates a failed SPU assertion)
template <class S> void halt(S& s, const char* what) { fatal("SPU %s: halt (%s) at %05x", s.name.c_str(), what, s.pc); }
OP(heq) { if (s.pref(RA(w)) == s.pref(RB(w))) halt(s, "heq"); }
OP(heqi) { if ((s32)s.pref(RA(w)) == I10(w)) halt(s, "heqi"); }
OP(hgt) { if ((s32)s.pref(RA(w)) > (s32)s.pref(RB(w))) halt(s, "hgt"); }
OP(hgti) { if ((s32)s.pref(RA(w)) > I10(w)) halt(s, "hgti"); }
OP(hlgt) { if (s.pref(RA(w)) > s.pref(RB(w))) halt(s, "hlgt"); }
OP(hlgti) { if (s.pref(RA(w)) > (u32)I10(w)) halt(s, "hlgti"); }

// ------------------------------------------------------------------ floating point
// SPU single precision is not IEEE: no infinities or NaNs (results saturate at +-FLT_MAX) and denormals are
// flushed to zero (FTZ/DAZ are set for SPU threads). Clamping +-inf keeps later 0 * x products at 0 instead of NaN.
FORCE_INLINE __m128 sat4(__m128 x) { return _mm_max_ps(_mm_min_ps(x, _mm_set1_ps(FLT_MAX)), _mm_set1_ps(-FLT_MAX)); }
OP(fa) { R(RT(w)).ps = sat4(_mm_add_ps(R(RA(w)).ps, R(RB(w)).ps)); }
OP(fs) { R(RT(w)).ps = sat4(_mm_sub_ps(R(RA(w)).ps, R(RB(w)).ps)); }
OP(fm) { R(RT(w)).ps = sat4(_mm_mul_ps(R(RA(w)).ps, R(RB(w)).ps)); }
OP(fma) { R(RRR_RT(w)).ps = sat4(_mm_fmadd_ps(R(RA(w)).ps, R(RB(w)).ps, R(RRR_RC(w)).ps)); }
OP(fms) { R(RRR_RT(w)).ps = sat4(_mm_fmsub_ps(R(RA(w)).ps, R(RB(w)).ps, R(RRR_RC(w)).ps)); }
OP(fnms) { R(RRR_RT(w)).ps = sat4(_mm_fnmadd_ps(R(RA(w)).ps, R(RB(w)).ps, R(RRR_RC(w)).ps)); }
OP(fceq) { R(RT(w)).ps = _mm_cmpeq_ps(R(RA(w)).ps, R(RB(w)).ps); }
OP(fcgt) { R(RT(w)).ps = _mm_cmpgt_ps(R(RA(w)).ps, R(RB(w)).ps); }
FORCE_INLINE __m128 fabs4(__m128 x) { return _mm_and_ps(x, _mm_castsi128_ps(_mm_set1_epi32(0x7FFFFFFF))); }
OP(fcmeq) { R(RT(w)).ps = _mm_cmpeq_ps(fabs4(R(RA(w)).ps), fabs4(R(RB(w)).ps)); }
OP(fcmgt) { R(RT(w)).ps = _mm_cmpgt_ps(fabs4(R(RA(w)).ps), fabs4(R(RB(w)).ps)); }
OP(frest) { R(RT(w)).ps = sat4(_mm_div_ps(_mm_set1_ps(1.0f), R(RA(w)).ps)); }   // 1/+-0 -> +-FLT_MAX
OP(frsqest) { R(RT(w)).ps = sat4(_mm_div_ps(_mm_set1_ps(1.0f), _mm_sqrt_ps(fabs4(R(RA(w)).ps)))); }
OP(fi) { R(RT(w)) = R(RB(w)); }   // estimates above are exact, interpolation is the identity
// 2^e: built from the exponent bits for normal results (the conversions run in inner loops)
FORCE_INLINE float pow2f(int e) {
    if (e >= -126 && e <= 127) { u32 bits = (u32)(e + 127) << 23; float f; memcpy(&f, &bits, 4); return f; }
    return std::ldexp(1.0f, e);
}
OP(csflt) { R(RT(w)).ps = _mm_mul_ps(_mm_cvtepi32_ps(R(RA(w)).pi), _mm_set1_ps(pow2f(-(int)(155 - I8(w))))); }
OP(cuflt) { float sc = pow2f(-(int)(155 - I8(w))); v128 r; for (int i = 0; i < 4; i++) r.f[i] = (float)R(RA(w)).w[i] * sc; R(RT(w)) = r; }
OP(cflts) {
    float sc = pow2f((int)(173 - I8(w))); v128 r;
    __m256d d = _mm256_mul_pd(_mm256_cvtps_pd(R(RA(w)).ps), _mm256_set1_pd(sc));
    // in range (a NaN compares false): truncating is all there is to do
    if (__builtin_expect(_mm256_movemask_pd(_mm256_cmp_pd(_mm256_andnot_pd(_mm256_set1_pd(-0.0), d), _mm256_set1_pd(2147483647.0), _CMP_LT_OQ)) == 15, 1)) {
        R(RT(w)).pi = _mm256_cvttpd_epi32(d);
        return;
    }
    for (int i = 0; i < 4; i++) {
        double x = (double)R(RA(w)).f[i] * sc;
        r.sw[i] = std::isnan(x) ? 0 : x >= 2147483647.0 ? INT32_MAX : x <= -2147483648.0 ? INT32_MIN : (s32)x;
    }
    R(RT(w)) = r;
}
OP(cfltu) {
    float sc = pow2f((int)(173 - I8(w))); v128 r;
    for (int i = 0; i < 4; i++) {
        double x = (double)R(RA(w)).f[i] * sc;
        r.w[i] = std::isnan(x) || x <= 0 ? 0 : x >= 4294967295.0 ? UINT32_MAX : (u32)x;
    }
    R(RT(w)) = r;
}
// double precision: two BE doublewords; BE dword 0 = host d[1]
OP(dfa) { v128 r; for (int i = 0; i < 2; i++) { double a, b; memcpy(&a, &R(RA(w)).d[i], 8); memcpy(&b, &R(RB(w)).d[i], 8); a += b; memcpy(&r.d[i], &a, 8); } R(RT(w)) = r; }
OP(dfs) { v128 r; for (int i = 0; i < 2; i++) { double a, b; memcpy(&a, &R(RA(w)).d[i], 8); memcpy(&b, &R(RB(w)).d[i], 8); a -= b; memcpy(&r.d[i], &a, 8); } R(RT(w)) = r; }
OP(dfm) { v128 r; for (int i = 0; i < 2; i++) { double a, b; memcpy(&a, &R(RA(w)).d[i], 8); memcpy(&b, &R(RB(w)).d[i], 8); a *= b; memcpy(&r.d[i], &a, 8); } R(RT(w)) = r; }
template <int K, class S>
FORCE_INLINE void dfmx(S& s, u32 w) {   // K: 0 dfma, 1 dfms, 2 dfnms, 3 dfnma ; rt is also the addend
    v128 r;
    for (int i = 0; i < 2; i++) {
        double a, b, t;
        memcpy(&a, &R(RA(w)).d[i], 8); memcpy(&b, &R(RB(w)).d[i], 8); memcpy(&t, &R(RT(w)).d[i], 8);
        double v = K == 0 ? std::fma(a, b, t) : K == 1 ? std::fma(a, b, -t) : K == 2 ? -std::fma(a, b, -t) : -std::fma(a, b, t);
        memcpy(&r.d[i], &v, 8);
    }
    R(RT(w)) = r;
}
template <int K, class S>
FORCE_INLINE void dfcmp(S& s, u32 w) {   // 0 eq, 1 gt, 2 mag eq, 3 mag gt
    v128 r;
    for (int i = 0; i < 2; i++) {
        double a, b;
        memcpy(&a, &R(RA(w)).d[i], 8); memcpy(&b, &R(RB(w)).d[i], 8);
        if (K >= 2) { a = std::fabs(a); b = std::fabs(b); }
        r.d[i] = ((K & 1) ? a > b : a == b) ? ~0ull : 0;
    }
    R(RT(w)) = r;
}
OP(dftsv) { R(RT(w)).pi = _mm_setzero_si128(); }
OP(fesd) {   // BE words 0,2 (floats) -> doubles
    v128 r;
    double hi = R(RA(w)).f[3], lo = R(RA(w)).f[1];
    memcpy(&r.d[1], &hi, 8); memcpy(&r.d[0], &lo, 8);
    R(RT(w)) = r;
}
OP(frds) {
    v128 r; r.pi = _mm_setzero_si128();
    double hi, lo; memcpy(&hi, &R(RA(w)).d[1], 8); memcpy(&lo, &R(RA(w)).d[0], 8);
    r.f[3] = (float)hi; r.f[1] = (float)lo;
    R(RT(w)) = r;
}
OP(fscrrd) { R(RT(w)).pi = _mm_setzero_si128(); }
OP(fscrwr) {}

// ------------------------------------------------------------------ branches (pc already points at this insn)
#define JUMP(t) do { s.pc = (t) & 0x3FFFC; return; } while (0)
// indirect branches carry interrupt enable (E, 0x10 in the RB field) / disable (D, 0x20) bits
template <class S> FORCE_INLINE void ints(S& s, u32 w) {
    u32 f = RB(w);
    if (f & 0x10) s.interrupts = true;
    if (f & 0x20) s.interrupts = false;
}
OP(br) { JUMP(s.pc + (I16(w) << 2)); }
OP(bra) { JUMP((u32)(I16(w) << 2)); }
OP(brsl) { u32 t = s.pc + (I16(w) << 2); s.set_pref(RT(w), s.pc + 4); JUMP(t); }
OP(brasl) { u32 t = (u32)(I16(w) << 2); s.set_pref(RT(w), s.pc + 4); JUMP(t); }
OP(brnz) { if (s.pref(RT(w)) != 0) JUMP(s.pc + (I16(w) << 2)); s.pc += 4; }
OP(brz) { if (s.pref(RT(w)) == 0) JUMP(s.pc + (I16(w) << 2)); s.pc += 4; }
OP(brhnz) { if ((s.pref(RT(w)) & 0xFFFF) != 0) JUMP(s.pc + (I16(w) << 2)); s.pc += 4; }
OP(brhz) { if ((s.pref(RT(w)) & 0xFFFF) == 0) JUMP(s.pc + (I16(w) << 2)); s.pc += 4; }
OP(bi) { ints(s, w); JUMP(s.pref(RA(w))); }
OP(iret) { ints(s, w); JUMP(s.srr0); }
OP(bisl) { ints(s, w); u32 t = s.pref(RA(w)); s.set_pref(RT(w), s.pc + 4); JUMP(t); }
OP(bisled) { s.pc += 4; }   // no external events are ever pending
OP(biz) { if (s.pref(RT(w)) == 0) { ints(s, w); JUMP(s.pref(RA(w))); } s.pc += 4; }
OP(binz) { if (s.pref(RT(w)) != 0) { ints(s, w); JUMP(s.pref(RA(w))); } s.pc += 4; }
OP(bihz) { if ((s.pref(RT(w)) & 0xFFFF) == 0) { ints(s, w); JUMP(s.pref(RA(w))); } s.pc += 4; }
OP(bihnz) { if ((s.pref(RT(w)) & 0xFFFF) != 0) { ints(s, w); JUMP(s.pref(RA(w))); } s.pc += 4; }
OP(nop) {}

// ------------------------------------------------------------------ channels
enum {
    SPU_RdEventStat = 0, SPU_WrEventMask = 1, SPU_WrEventAck = 2, SPU_RdSigNotify1 = 3, SPU_RdSigNotify2 = 4,
    SPU_WrDec = 7, SPU_RdDec = 8, SPU_RdEventMask = 11, SPU_RdMachStat = 13, SPU_WrSRR0 = 14, SPU_RdSRR0 = 15,
    MFC_LSA = 16, MFC_EAH = 17, MFC_EAL = 18, MFC_Size = 19, MFC_TagID = 20, MFC_Cmd = 21, MFC_WrTagMask = 22,
    MFC_WrTagUpdate = 23, MFC_RdTagStat = 24, MFC_RdListStallStat = 25, MFC_WrListStallAck = 26,
    MFC_RdAtomicStat = 27, SPU_WrOutMbox = 28, SPU_RdInMbox = 29, SPU_WrOutIntrMbox = 30,
};
enum : u32 { EV_LR = 0x400, EV_TM = 0x20 };

u32 decrementer(SPUThread& s) { return s.dec_start - (u32)((time_usec() - s.dec_time) * 80); }

// reservation-lost detection: compare the reserved line with memory
bool reservation_lost(SPUThread& s) {
    return s.has_reservation && !same_bytes(gmem(s.res_addr), s.res_data, 128);
}

u32 read_event_stat(SPUThread& s, bool block) {
    for (;;) {
        if ((s.event_mask & EV_LR) && reservation_lost(s)) { s.event_stat |= EV_LR; s.has_reservation = false; }
        if ((s.event_mask & EV_TM) && (s32)decrementer(s) < 0) s.event_stat |= EV_TM;
        u32 v = s.event_stat & s.event_mask;
        if (v || !block) return v;
        sleep_usec(20);
    }
}

OP(rdch) {
    u32 ch = RA(w), v = 0;
    switch (ch) {
        case SPU_RdEventStat: v = read_event_stat(s, true); break;
        case SPU_RdSigNotify1: case SPU_RdSigNotify2: v = s.snr[ch - 3]; s.snr[ch - 3] = 0; break;
        case SPU_RdDec: v = decrementer(s); break;
        case SPU_RdEventMask: v = s.event_mask; break;
        case SPU_RdMachStat: v = (s.interrupts || s.report_ie) ? 1 : 0; break;   // bit 0: interrupts enabled
        case SPU_RdSRR0: v = s.srr0; break;
        case MFC_RdTagStat: v = s.tag_mask; break;          // all DMA completes synchronously
        case MFC_RdListStallStat: v = s.stall_stat; s.stall_stat = 0; break;
        case MFC_RdAtomicStat: v = s.atomic_stat; s.atomic_stat = 0; break;
        case SPU_RdInMbox: {
            std::unique_lock lk(s.mbox_m);
            if (!s.in_mbox_valid && s.event_completion_pending) {   // no PPU handler here: complete at once
                s.event_completion_pending = false;
                v = 0;
                break;
            }
            s.mbox_cv.wait(lk, [&] { return s.in_mbox_valid; });
            v = s.in_mbox;
            s.in_mbox_valid = false;
            s.mbox_cv.notify_all();
            break;
        }
        default: LOG_W("SPU %s: rdch %u at %05x", s.name.c_str(), ch, s.pc);
    }
    s.set_pref(RT(w), v);
}
OP(rchcnt) {
    u32 ch = RA(w), v = 1;
    switch (ch) {
        case SPU_RdEventStat: v = read_event_stat(s, false) ? 1 : 0; break;
        case SPU_RdInMbox: { std::lock_guard lk(s.mbox_m); v = s.in_mbox_valid; break; }
        case SPU_WrOutMbox: case SPU_WrOutIntrMbox: { std::lock_guard lk(s.mbox_m); v = !s.out_mbox_valid; break; }
        case SPU_RdSigNotify1: case SPU_RdSigNotify2: v = s.snr[ch - 3] != 0; break;
        case MFC_Cmd: v = 16; break;
        case MFC_RdListStallStat: v = s.stall_stat != 0; break;
        default: v = 1;
    }
    s.set_pref(RT(w), v);
}
OP(wrch) {
    u32 ch = RA(w), v = s.pref(RT(w));
    switch (ch) {
        case SPU_WrEventMask: s.event_mask = v; break;
        case SPU_WrEventAck: s.event_stat &= ~v; break;
        case SPU_WrDec: s.dec_start = v; s.dec_time = time_usec(); break;
        case SPU_WrSRR0: s.srr0 = v; break;
        case MFC_LSA: s.mfc_lsa = v; break;
        case MFC_EAH: s.mfc_eah = v; break;
        case MFC_EAL: s.mfc_eal = v; break;
        case MFC_Size: s.mfc_size = v; break;
        case MFC_TagID: s.mfc_tag = v; break;
        case MFC_Cmd: s.dma(v); break;
        case MFC_WrTagMask: s.tag_mask = v; break;
        case MFC_WrTagUpdate: break;
        case MFC_WrListStallAck:
            if (s.stalled[v & 31].active) s.run_list(v & 31);
            break;
        case SPU_WrOutIntrMbox:
            if (s.on_send_event) {   // handled like lv2 does for SPU threads: consume both mailboxes, reply
                u32 data1;
                {
                    std::lock_guard lk(s.mbox_m);
                    data1 = s.out_mbox_valid ? s.out_mbox : 0;
                    s.out_mbox_valid = false;
                }
                u32 r = s.on_send_event(s, v, data1);
                std::lock_guard lk(s.mbox_m);
                s.in_mbox = r;
                s.in_mbox_valid = true;
                s.event_completion_pending = s.event_needs_completion;
                s.mbox_cv.notify_all();
                break;
            }
            [[fallthrough]];
        case SPU_WrOutMbox: {
            static std::atomic<int> logged{0};
            if (logged++ < 30) LOG_I("SPU %s: wrch %u <- %08x at %05x", s.name.c_str(), ch, v, s.pc);
            std::unique_lock lk(s.mbox_m);
            s.mbox_cv.wait(lk, [&] { return !s.out_mbox_valid; });
            s.out_mbox = v;
            s.out_mbox_valid = true;
            s.mbox_cv.notify_all();
            break;
        }
        default: LOG_W("SPU %s: wrch %u <- %08x at %05x", s.name.c_str(), ch, v, s.pc);
    }
}
OP(mfspr) { s.set_pref(RT(w), 0); }
OP(mtspr) {}
OP(stop) {
    u32 code = w & 0x3FFF;
    if (!s.on_stop || !s.on_stop(s, code)) s.halted = true;
}
OP(stopd) {   // debug halt (failed assertions in SPU code): logged, it used to stop SPUs silently
    static std::atomic<int> n{0};
    if (n++ < 20) LOG_W("SPU %s: stopd at %05x (halted)", s.name.c_str(), s.pc);
    s.halted = true;
}
OP(invalid) { fatal("SPU %s: invalid instruction %08x at %05x", s.name.c_str(), w, s.pc); }


}  // namespace

// ------------------------------------------------------------------ idioms
// Hot job-code loops that build tables one element at a time (tools/spu_aot.py IDIOMS). Each runs at the head of the
// loop's block and does all iterations but the last in bulk, leaving the counter as those iterations would; the last one
// then runs as translated, so every other register ends as in the original. Conditions the bulk code does not cover
// (decoded code in the range, misaligned addresses) just leave the work to the original loop. Checked against the
// interpreter by F1CE_SPU_BENCH=1; F1CE_SPU_NOIDIOM=1 switches them off.
extern bool g_spu_idiom;
template <class S> FORCE_INLINE bool ls_plain_range(S& s, u32 a, u32 n) {
    if (a + n > 0x40000) return false;
    for (u32 l = a >> 7, e = (a + n - 1) >> 7; l <= e; l++) if (s.line_code[l]) return false;
    return true;
}
template <class S> FORCE_INLINE void add_all_words(S& s, int r, u32 v) { R(r).pi = _mm_add_epi32(R(r).pi, _mm_set1_epi32((int)v)); }

// 04ae4: for (i = r8; i != r4; i += 4) *(u32*)(r81 + i) = r7
template <class S> void spu_idiom_fill_word(S& s) {
    u32 i = s.pref(8), end = s.pref(4), a = (i + s.pref(81)) & 0x3FFFF;
    if (!g_spu_idiom || end <= i || ((end - i) & 3) || (a & 3)) return;
    u32 bulk = (end - i) / 4 - 1;
    if (!bulk || !ls_plain_range(s, a, bulk * 4)) return;
    u32 v = __builtin_bswap32(s.pref(7));
    u32* p = (u32*)(s.ls + a);
    for (u32 k = 0; k < bulk; k++) p[k] = v;
    add_all_words(s, 8, bulk * 4);
}
// 08910: for (i = r7; i != r10; i++) *(u16*)(r8 + 2 * i) = i << 4
template <class S> void spu_idiom_ramp_u16(S& s) {
    u32 i = s.pref(7), end = s.pref(10), a = (i * 2 + s.pref(8)) & 0x3FFFF;
    if (!g_spu_idiom || end <= i || (a & 1)) return;
    u32 bulk = end - i - 1;
    if (!bulk || !ls_plain_range(s, a, bulk * 2)) return;
    u16* p = (u16*)(s.ls + a);
    for (u32 k = 0; k < bulk; k++) p[k] = __builtin_bswap16((u16)((i + k) << 4));
    add_all_words(s, 7, bulk);
}
// 0e3f0: per quad q of [r5 + r80, r5 + r8): x = q - r6 (halfwords); q = (x & r7) > r9 ? r7 : x, bitwise per halfword
template <class S> void spu_idiom_age_table(S& s) {
    u32 o = s.pref(80), end = s.pref(8), a = (o + s.pref(5)) & 0x3FFFF;
    if (!g_spu_idiom || end <= o || ((end - o) & 15) || (a & 15)) return;
    u32 bulk = (end - o) / 16 - 1;
    if (!bulk || !ls_plain_range(s, a, bulk * 16)) return;
    const __m256i sub = _mm256_broadcastsi128_si256(R(6).pi), mask = _mm256_broadcastsi128_si256(R(7).pi);
    const __m256i limit = _mm256_broadcastsi128_si256(R(9).pi), order = _mm256_broadcastsi128_si256(rev());
    u8* p = s.ls + a;
    u32 k = 0;
    for (; k + 2 <= bulk; k += 2, p += 32) {
        __m256i x = _mm256_sub_epi16(_mm256_shuffle_epi8(_mm256_loadu_si256((const __m256i*)p), order), sub);
        __m256i c = _mm256_cmpgt_epi16(_mm256_and_si256(x, mask), limit);
        x = _mm256_or_si256(_mm256_and_si256(c, mask), _mm256_andnot_si256(c, x));
        _mm256_storeu_si256((__m256i*)p, _mm256_shuffle_epi8(x, order));
    }
    if (k < bulk) {
        __m128i x = _mm_sub_epi16(_mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)p), rev()), R(6).pi);
        __m128i c = _mm_cmpgt_epi16(_mm_and_si128(x, R(7).pi), R(9).pi);
        x = _mm_or_si128(_mm_and_si128(c, R(7).pi), _mm_andnot_si128(c, x));
        _mm_storeu_si128((__m128i*)p, _mm_shuffle_epi8(x, rev()));
    }
    add_all_words(s, 80, bulk * 16);
}

// tail of the 05778 loop: from the phase r27 and the output pointers, the sample addresses and insert patterns of the next pass
template <class S> FORCE_INLINE void audio_voice_tail(S& s) {
    op_rotmai(s, 0x0F5D0D82u);
    op_cwd(s, 0x3EC00518u);
    op_cwd(s, 0x3EC0049Au);
    op_and_(s, 0x182ACD86u);
    op_cwd(s, 0x3EC0041Cu);
    op_a(s, 0x18088A10u);
    op_cwd(s, 0x3EC0059Du);
    op_shli(s, 0x0F608105u);
    op_ai(s, 0x1C004103u);
    op_shli(s, 0x0F608184u);
    op_a(s, 0x1804C28Du);
}
// a shufb pattern with both sources the same register, as a pshufb control (no constant bytes)
FORCE_INLINE bool one_reg_pattern(const v128& p, __m128i& ctl) {
    alignas(16) u8 c[16];
    for (int i = 0; i < 16; i++) {
        if (p.b[i] & 0xC0) return false;
        c[i] = 15 - (p.b[i] & 15);
    }
    ctl = _mm_load_si128((const __m128i*)c);
    return true;
}
// 05778, audio voice, per sample: resample [r19 + (r27 >> 12) * 4] by the phase r27 += r38, four one-pole low-passes
// (r23 r21 by r31 r33, r22 r18 by r30 r32), a clipper against r25 with the word at [r20 + r34], then mix into the outputs at
// r10 r9 r8 r11 with the gains r41 r40 r37 r39. All four lanes run as on the SPU, since the filter state is kept in full.
template <class S> void spu_idiom_audio_voice(S& s) {
    const s32 bulk = (s32)s.pref(17);
    u32 out[4] = {s.pref(10), s.pref(9), s.pref(8), s.pref(11)};
    if (!g_spu_idiom || bulk <= 0 || bulk > 0x8000 || ((out[0] | out[1] | out[2] | out[3]) & 3)) return;
    for (u32& o : out) { o &= 0x3FFFF; if (!ls_plain_range(s, o, (u32)bulk * 4)) return; }
    __m128i clip_order;
    if (!one_reg_pattern(R(35), clip_order)) return;

    static constexpr u8 rot_tab[32] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    const auto rotq = [](__m128i v, u32 n) { return _mm_shuffle_epi8(v, _mm_loadu_si128((const __m128i*)(rot_tab + (16 - (n & 15))))); };
    const auto load = [&](u32 a) { return _mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)(s.ls + (a & 0x3FFF0))), rev()); };
    const auto word = [&](u32 a) { return _mm_set1_epi32((int)__builtin_bswap32(*(const u32*)(s.ls + a))); };
    const auto mix = [&](u32 a, __m128 gain, __m128 v) {
        __m128 x = sat4(_mm_fmadd_ps(gain, v, _mm_castsi128_ps(word(a))));
        *(u32*)(s.ls + a) = __builtin_bswap32((u32)_mm_extract_ps(x, 3));
    };

    __m128i phase = R(27).pi, frac = R(6).pi;
    const __m128i step = R(38).pi, mask = R(43).pi;
    __m128 f23 = R(23).ps, f21 = R(21).ps, f22 = R(22).ps, f18 = R(18).ps;
    const __m128 c44 = R(44).ps, c45 = R(45).ps, c31 = R(31).ps, c33 = R(33).ps, c30 = R(30).ps, c32 = R(32).ps;
    const __m128 c25 = R(25).ps, c42 = R(42).ps, c36 = R(36).ps, c41 = R(41).ps, c40 = R(40).ps, c37 = R(37).ps, c39 = R(39).ps;
    const u32 base = s.pref(19), side = s.pref(20) + s.pref(34);
    u32 a0 = s.pref(13), a1 = s.pref(4) + base;
    for (s32 k = 0; k < bulk; k++) {
        const __m128 fr = _mm_cvtepi32_ps(frac);
        const __m128 s1 = _mm_castsi128_ps(rotq(load(a1), a1)), s0 = _mm_castsi128_ps(rotq(load(a0), a0));
        const __m128 sd = _mm_castsi128_ps(rotq(load(side), side));
        phase = _mm_add_epi32(phase, step);

        __m128 x = sat4(_mm_sub_ps(c44, fr));
        x = sat4(_mm_fmadd_ps(x, s0, sat4(_mm_mul_ps(fr, s1))));
        x = sat4(_mm_mul_ps(x, c45));
        f23 = sat4(_mm_fmadd_ps(c33, f23, sat4(_mm_mul_ps(c31, x))));
        f21 = sat4(_mm_fmadd_ps(c33, f21, sat4(_mm_mul_ps(c31, f23))));
        f22 = sat4(_mm_fmadd_ps(c32, f22, sat4(_mm_mul_ps(c30, f21))));
        f18 = sat4(_mm_fmadd_ps(c32, f18, sat4(_mm_mul_ps(c30, f22))));

        __m128 hi = sat4(_mm_add_ps(f18, sd)), lo = sat4(_mm_sub_ps(f18, sd));
        hi = _mm_castsi128_ps(_mm_shuffle_epi8(_mm_castps_si128(hi), clip_order));
        lo = _mm_castsi128_ps(_mm_shuffle_epi8(_mm_castps_si128(lo), clip_order));
        hi = _mm_blendv_ps(hi, c25, _mm_cmpgt_ps(hi, c25));
        lo = _mm_blendv_ps(lo, c25, _mm_cmpgt_ps(c25, lo));
        __m128 v = sat4(_mm_add_ps(lo, hi));
        v = sat4(_mm_fmadd_ps(v, c42, f18));
        v = sat4(_mm_mul_ps(c36, v));
        mix(out[0] + (u32)k * 4, c41, v);
        mix(out[1] + (u32)k * 4, c40, v);
        mix(out[2] + (u32)k * 4, c37, v);
        mix(out[3] + (u32)k * 4, c39, v);

        const s32 idx = (s32)_mm_extract_epi32(phase, 3) >> 12;
        a0 = (u32)idx * 4 + base;
        a1 = a0 + 4;
        frac = _mm_and_si128(phase, mask);
    }
    R(27).pi = phase;
    R(23).ps = f23; R(21).ps = f21; R(22).ps = f22; R(18).ps = f18;
    add_all_words(s, 10, (u32)bulk * 4);
    add_all_words(s, 9, (u32)bulk * 4);
    add_all_words(s, 8, (u32)bulk * 4);
    add_all_words(s, 11, (u32)bulk * 4);
    add_all_words(s, 17, (u32)-bulk);
    audio_voice_tail(s);
}

// F1CE_SPU_NOCULL=1, at 0d7f8: the geometry job keeps all triangles. Its loop compacts the list at r3 in place (8 bytes per
// triangle) with back-face and small-triangle tests on the transformed vertices; the tail at 0ddf0 reports r13 / 8 of the
// r33 triangles. Skipping it moves the work to the renderer and the GPU.
extern bool g_spu_nocull, g_spu_fastcull;
extern int g_spu_cull_check;
void spu_cull_check(const v128* gpr, const u8* ls);
void spu_xform_check(const v128* gpr, const u8* ls);

FORCE_INLINE __m128i cflts_scale1(__m128 x) {
    __m128i v = _mm_cvttps_epi32(x);
    if (__builtin_expect(_mm_movemask_epi8(_mm_cmpeq_epi32(v, _mm_set1_epi32(INT32_MIN))) != 0, 0)) {   // out of range or NaN
        alignas(16) float f[4]; alignas(16) s32 r[4];
        _mm_store_ps(f, x);
        for (int i = 0; i < 4; i++) r[i] = std::isnan(f[i]) ? 0 : f[i] >= 2147483648.0f ? INT32_MAX : f[i] <= -2147483648.0f ? INT32_MIN : (s32)f[i];
        return _mm_load_si128((const __m128i*)r);
    }
    return v;
}
// 0d7f8-0da0c without its software pipeline: four triangles per pass (8 bytes each, u16 a b c + one more, indices into the
// quads at r8: x y z w, big-endian). A triangle is dropped if it covers no pixel row or column, is behind the edges r71 r69
// or below 0, or faces away (sign of an area with an error margin r34 r55, r57 flips it, r54 = 16 turns the test off);
// any vertex nearer than r52 keeps it. Kept ones compact the list at r3 in place. After the tail at 0ddf0 only r13 and that
// list matter. Indices of 4096 or more (the two quads of a pass address them differently) stay with the translated loop.
template <class S> bool spu_fast_triangle_cull(S& s) {
    const u32 n = s.pref(33), list = s.pref(3), verts = s.pref(8), area_shift = s.pref(54) & 31;
    if (!g_spu_fastcull || !n || n > 0x8000 || (list & 15) || (area_shift && area_shift < 16) || !ls_plain_range(s, list, n * 8)) return false;
    u64* const tris = (u64*)(s.ls + list);
    u64 seen = 0;
    for (u32 i = 0; i < n; i++) seen |= tris[i];
    if (seen & 0x000000F000F000F0ull) return false;

    const auto splat = [&](int r) { return _mm_castsi128_ps(_mm_set1_epi32((int)s.pref(r))); };
    const __m128 c34 = splat(34), c55 = splat(55), c45 = splat(45), c52 = splat(52), c69 = splat(69), c71 = splat(71), c57 = splat(57);
    const __m128 sign = _mm_castsi128_ps(_mm_set1_epi32(INT32_MIN));
    const __m128i swap = _mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12);
    const auto vtx = [&](u32 i) { return _mm_castsi128_ps(_mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)(s.ls + ((verts + (i << 4)) & 0x3FFF0))), swap)); };
    const auto idx = [](u64 t, int k) { return (u32)__builtin_bswap16((u16)(t >> (k * 16))); };

    u64* out = tris;
    for (u32 i = 0; i < n; i += 4) {
        const u32 rem = n - i;
        const u64 t0 = tris[i], t1 = rem > 1 ? tris[i + 1] : 0, t2 = rem > 2 ? tris[i + 2] : 0, t3 = rem > 3 ? tris[i + 3] : 0;
        __m128 ax = vtx(idx(t0, 0)), ay = vtx(idx(t1, 0)), az = vtx(idx(t2, 0)), aw = vtx(idx(t3, 0));
        __m128 bx = vtx(idx(t0, 1)), by = vtx(idx(t1, 1)), bz = vtx(idx(t2, 1)), bw = vtx(idx(t3, 1));
        __m128 cx = vtx(idx(t0, 2)), cy = vtx(idx(t1, 2)), cz = vtx(idx(t2, 2)), cw = vtx(idx(t3, 2));
        _MM_TRANSPOSE4_PS(ax, ay, az, aw);
        _MM_TRANSPOSE4_PS(bx, by, bz, bw);
        _MM_TRANSPOSE4_PS(cx, cy, cz, cw);

        const __m128 dcy = sat4(_mm_sub_ps(cy, ay)), dbx = sat4(_mm_sub_ps(bx, ax)), dby = sat4(_mm_sub_ps(by, ay)), dcx = sat4(_mm_sub_ps(cx, ax));
        const __m128 gx = _mm_cmpgt_ps(ax, bx), gy = _mm_cmpgt_ps(ay, by);
        __m128 maxx = _mm_blendv_ps(bx, ax, gx), minx = _mm_blendv_ps(ax, bx, gx);
        __m128 maxy = _mm_blendv_ps(by, ay, gy), miny = _mm_blendv_ps(ay, by, gy);
        maxy = _mm_blendv_ps(maxy, cy, _mm_cmpgt_ps(cy, maxy));
        miny = _mm_blendv_ps(cy, miny, _mm_cmpgt_ps(cy, miny));
        maxx = _mm_blendv_ps(maxx, cx, _mm_cmpgt_ps(cx, maxx));
        minx = _mm_blendv_ps(cx, minx, _mm_cmpgt_ps(cx, minx));

        const __m128 sum_y = sat4(_mm_add_ps(_mm_andnot_ps(sign, dcy), _mm_andnot_ps(sign, dby)));
        const __m128 sum_x = sat4(_mm_add_ps(_mm_andnot_ps(sign, dbx), _mm_andnot_ps(sign, dcx)));
        const __m128 margin = sat4(_mm_fmadd_ps(c34, sat4(_mm_add_ps(sum_x, sum_y)), c55));
        const __m128 area = sat4(_mm_fmadd_ps(dcx, dby, sat4(_mm_fnmadd_ps(dcy, dbx, margin))));
        const __m128 facing = area_shift ? _mm_setzero_ps() : _mm_xor_ps(c57, area);

        const __m128 hx = sat4(_mm_add_ps(c45, maxx)), hy = sat4(_mm_add_ps(c45, maxy));
        const __m128 same_col = _mm_castsi128_ps(_mm_cmpeq_epi32(cflts_scale1(minx), cflts_scale1(hx)));
        const __m128 same_row = _mm_castsi128_ps(_mm_cmpeq_epi32(cflts_scale1(miny), cflts_scale1(hy)));
        const __m128 drop = _mm_or_ps(_mm_or_ps(_mm_or_ps(same_col, same_row), _mm_or_ps(hx, hy)),
                                      _mm_or_ps(facing, _mm_or_ps(_mm_cmpgt_ps(minx, c71), _mm_cmpgt_ps(miny, c69))));
        const __m128 near = _mm_or_ps(_mm_cmpgt_ps(c52, az), _mm_or_ps(_mm_cmpgt_ps(c52, bz), _mm_cmpgt_ps(c52, cz)));
        const u32 keep = (u32)(~_mm_movemask_ps(drop) | _mm_movemask_ps(near));
        *out = t0; out += keep & 1;
        if (rem > 1) { *out = t1; out += (keep >> 1) & 1; }
        if (rem > 2) { *out = t2; out += (keep >> 2) & 1; }
        if (rem > 3) { *out = t3; out += (keep >> 3) & 1; }
    }
    s.set_pref(13, (u32)(out - tris) * 8);
    s.pc = 0xDDF0;
    return true;
}
// 0d628-0d6d4, the vertex transform before that cull: the block before it has run the pipeline's first pass, whose stores
// only touch the first output quads. Vertices (x y z, r37 rounded to 4) go through a 3x4 matrix (r53 r18 r54 + r6, r28 r30 r31
// + r13, r48 r50 r51 + r52) and a 1/w with one refinement step. Output per four: x y w + x3, x y w + y3, x y w, x y w. Nothing
// after the loop reads the registers it changes.
template <class S> bool spu_fast_vertex_transform(S& s) {
    const u32 size = s.pref(37) << 4, src = (s.pref(29) - size) & 0x3FFFF, dst = (s.pref(41) - size) & 0x3FFFF;
    if (!g_spu_fastcull || !size || size > 0x20000 || (size & 63) || ((src | dst) & 15)) return false;
    if (src + size > 0x40000 || dst + size > 0x40000 || (src < dst + size && dst < src + size) || !ls_plain_range(s, dst, size)) return false;

    const auto splat = [&](int r) { return _mm256_castsi256_ps(_mm256_set1_epi32((int)s.pref(r))); };
    const __m256 c53 = splat(53), c6 = splat(6), c28 = splat(28), c13 = splat(13), c48 = splat(48), c52 = splat(52);
    const __m256 c18 = splat(18), c30 = splat(30), c50 = splat(50), c54 = splat(54), c51 = splat(51), c31 = splat(31);
    const __m256 c34 = splat(34), c16 = splat(16), one = _mm256_set1_ps(1.0f);
    const __m256 hi = _mm256_set1_ps(FLT_MAX), lo = _mm256_set1_ps(-FLT_MAX);
    const auto sat = [&](__m256 x) { return _mm256_max_ps(_mm256_min_ps(x, hi), lo); };
    const __m256i swap = _mm256_broadcastsi128_si256(_mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12));
    const u8* in = s.ls + src;
    u8* out = s.ls + dst;
    for (u32 o = 0; o < size; o += 128) {
        const u32 b = size - o > 64 ? 64 : 0;   // a last group of four fills both halves
        const auto row = [&](u32 k) {
            __m256i v = _mm256_inserti128_si256(_mm256_castsi128_si256(_mm_loadu_si128((const __m128i*)(in + o + k))),
                                                _mm_loadu_si128((const __m128i*)(in + o + b + k)), 1);
            return _mm256_castsi256_ps(_mm256_shuffle_epi8(v, swap));
        };
        const __m256 r0 = row(0), r1 = row(16), r2 = row(32), r3 = row(48);
        const __m256 t0 = _mm256_unpacklo_ps(r0, r1), t1 = _mm256_unpackhi_ps(r0, r1), t2 = _mm256_unpacklo_ps(r2, r3), t3 = _mm256_unpackhi_ps(r2, r3);
        const __m256 x = _mm256_shuffle_ps(t0, t2, 0x44), y = _mm256_shuffle_ps(t0, t2, 0xEE), z = _mm256_shuffle_ps(t1, t3, 0x44);

        const __m256 p = sat(_mm256_fmadd_ps(c18, y, sat(_mm256_fmadd_ps(x, c53, c6))));
        const __m256 q = sat(_mm256_fmadd_ps(c30, y, sat(_mm256_fmadd_ps(x, c28, c13))));
        const __m256 u = sat(_mm256_fmadd_ps(c50, y, sat(_mm256_fmadd_ps(x, c48, c52))));
        const __m256 w = sat(_mm256_fmadd_ps(z, c54, p)), hy = sat(_mm256_fmadd_ps(z, c51, u)), hx = sat(_mm256_fmadd_ps(z, c31, q));
        const __m256 est = sat(_mm256_div_ps(one, w));
        const __m256 inv = sat(_mm256_fmadd_ps(sat(_mm256_fnmadd_ps(est, w, c16)), est, est));
        const __m256 ox = sat(_mm256_fmsub_ps(inv, hx, c34)), oy = sat(_mm256_fmsub_ps(inv, hy, c34));

        const __m256 u0 = _mm256_unpacklo_ps(ox, oy), u1 = _mm256_unpackhi_ps(ox, oy);
        const __m256 last = _mm256_castsi256_ps(_mm256_srli_si256(_mm256_castps_si256(u1), 8));   // x3 y3 0 0
        const __m256 v0 = _mm256_unpacklo_ps(w, last), v1 = _mm256_unpackhi_ps(w, last);
        const auto put = [&](u32 k, __m256 v) {
            const __m256i e = _mm256_shuffle_epi8(_mm256_castps_si256(v), swap);
            _mm_storeu_si128((__m128i*)(out + o + k), _mm256_castsi256_si128(e));
            if (b) _mm_storeu_si128((__m128i*)(out + o + b + k), _mm256_extracti128_si256(e, 1));
        };
        put(0, _mm256_shuffle_ps(u0, v0, 0x44));
        put(16, _mm256_shuffle_ps(u0, v0, 0xEE));
        put(32, _mm256_shuffle_ps(u1, v1, 0x44));
        put(48, _mm256_shuffle_ps(u1, v1, 0xEE));
    }
    s.pc = 0xD6D8;
    return true;
}
template <class S> bool spu_skip_vertex_transform(S& s) {
    if (g_spu_cull_check) spu_xform_check(s.gpr, s.ls);
    return spu_fast_vertex_transform(s);
}
template <class S> bool spu_skip_triangle_cull(S& s) {
    if (g_spu_cull_check) spu_cull_check(s.gpr, s.ls);
    if (g_spu_nocull) {
        s.set_pref(13, s.pref(33) * 8);
        s.pc = 0xDDF0;
        return true;
    }
    return spu_fast_triangle_cull(s);
}

// ------------------------------------------------------------------ native job manager (runtime/spu_pm.cpp)
// Each runs on the thread (cluster code with local registers writes them back first, tools/spu_aot.py) and returns
// false to leave the call to the SPU code.
bool spu_pm_buffers(SPUThread& t);     // 0x19f8
bool spu_pm_dma_info(SPUThread& t);    // 0x2d48
bool spu_pm_release(SPUThread& t);     // 0x2ce8
bool spu_pm_job_done(SPUThread& t);    // 0x1670
bool spu_pm_grab(SPUThread& t);        // 0x2288
bool spu_pm_irq_enter(SPUThread& t);   // 0x0a2c
bool spu_pm_irq_leave(SPUThread& t);   // 0x0a98
template <class S> FORCE_INLINE bool spu_pm_call(S& s, bool (*f)(SPUThread&)) {
    static_assert(std::is_same_v<S, SPUThread>);
    return s.pm_native && f(s);
}
template <class S> bool spu_skip_pm_buffers(S& s) { return spu_pm_call(s, spu_pm_buffers); }
template <class S> bool spu_skip_pm_dma_info(S& s) { return spu_pm_call(s, spu_pm_dma_info); }
template <class S> bool spu_skip_pm_release(S& s) { return spu_pm_call(s, spu_pm_release); }
template <class S> bool spu_skip_pm_job_done(S& s) { return spu_pm_call(s, spu_pm_job_done); }
template <class S> bool spu_skip_pm_grab(S& s) { return spu_pm_call(s, spu_pm_grab); }
template <class S> bool spu_skip_pm_irq_enter(S& s) { return spu_pm_call(s, spu_pm_irq_enter); }
template <class S> bool spu_skip_pm_irq_leave(S& s) { return spu_pm_call(s, spu_pm_irq_leave); }
