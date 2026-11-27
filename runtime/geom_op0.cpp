// Native op0 of the geometry job (0x8C80, GEOMETRY_JOB.md "op0"): the piece's output and its in-order completion.
// Declined (before any side effect) outside the path covered and skinned pieces take; F1CE_GEOM_OP0=0: SPU code.
//
//   0x8970(ctx, 1)   the vertex table, when op40 did not make it
//   0x44C8(ctx, 0)   the output buffer's pending bytes (ctx[336] .. ctx[320]) out: ring space from 0x4220 (line
//                    p[16]: word 0 = write pointer + size rounded to 128, word 4 = end; when it does not fit, word 8
//                    grows instead and ctx[1168] = -1: no more puts in this job), the records ctx[1200 + 4n] get
//                    (ea - p[20]) added (offsets the RSX sees) and are cleared with ctx[1184], then 0x4368 puts them
//   GET              when p[24] (the in-order area) is set: the six SPUs' done records, 0x60 bytes from p[24] + 128
//                    to ctx + 128 (tag ctx[264])
//   command block    p[36] bytes from the output buffer (0x4C18, aligned to 16): per vertex record ctx[864 + 4k] and
//                    attribute of format set k (format at ctx[816]) whose id is among the bytes p[0..15] at index i,
//                    SET_VERTEX_DATA_ARRAY_OFFSET[i] = record + attribute offset; when op35 ran (ctx[1152]) the
//                    index array (ctx[404]) and the draws (0x8690); zeros to the end (0x8660); put to p[32] (0x4368)
//   completion       (when p[24] is set) 0x4C18 gives a 128-aligned buffer of 0xA0 bytes; the done records are
//                    combined into this SPU's (base = the largest base, masks shifted to it and ORed, whole bytes of
//                    done jobs move the base on); a job more than 95 past the base reads them again and waits; the
//                    in-order line p[24] (word 0 lock, word 4 count, word 8 kick address, words 12 / 13) decides
//                    whether this job takes the lock (PUTLLC word 0 = -1) and kicks: done record put, then a 4-byte
//                    put of p[28] (or word 13) to the kick address and the line back unlocked with word 4 = the job
//                    index (PUTLLUC); else only the done record, between barriers.
// Every MFC command is issued through SPUThread::dma with the channel values the SPU code writes, so the dry mode of
// the checks (F1CE_GEOM_OP0_CHECK, hle_geom.cpp) sees the same commands.
#include <atomic>
#include <cstring>

#include "kernel.h"
#include "spu.h"

namespace {
inline u32 rd(const SPUThread& s, u32 a) { return s.ls32(a); }
inline void wr(SPUThread& s, u32 a, u32 v) { s.ls32(a, v); }
inline u16 h16(const SPUThread& s, u32 a) { a &= 0x3FFFF; return (u16)(s.ls[a] << 8 | s.ls[(a + 1) & 0x3FFFF]); }

struct Op0 {
    SPUThread& s;
    u32 ctx, p;
    bool toggled = false;   // a channel sequence ran (each ends with interrupts enabled)
    int spins = 0;          // waits for other SPUs (window, lock): bounded in the checks
    int max_spins;

    // MFC: the channel writes of the SPU code (only those it writes; the others keep their values)
    void lsa(u32 v) { s.mfc_lsa = v; }
    void eal(u32 v) { s.mfc_eal = v; }
    void size(u32 v) { s.mfc_size = v; }
    void tag(u32 v) { s.mfc_tag = v; }
    void cmd(u32 c) { s.dma(c); toggled = true; }
    u32 atomic() { const u32 v = s.atomic_stat; s.atomic_stat = 0; return v; }   // rdch MFC_RdAtomicStat
    void wait_tag(u32 t) { s.tag_mask = 1u << (t & 63); toggled = true; }        // wrch 22 / 23, rdch 24

    u32 out() const { return rd(s, ctx + 320); }
    u32 base() const { return rd(s, ctx + 336); }
    u32 buf_start() const { return rd(s, ctx + 308 + (rd(s, ctx + 304) & ~3u)) & 0x3FFFF; }
    u32 buf_tag() const { return rd(s, ctx + 308 + (rd(s, ctx + 304) & ~3u)) >> 24; }

    // 0x4C18 without its flush / init paths (checked in plan()): the output pointer aligned, size rounded to 16
    u32 alloc(u32 n, u32 align) {
        const u32 a = (out() + align - 1) & (0u - align);
        wr(s, ctx + 320, a + ((n + 15) & ~15u));
        return a;
    }
    // 0x4368(ctx, ea, 0): the pending bytes to ea (0x4000 a put), the buffer pointers moved on to 128 bytes
    void issue(u32 ea) {
        const u32 b = base(), o = out();
        u32 n = o - b;
        if (!n) return;
        wr(s, ctx + 336, b + ((n + 127) & ~127u));
        wr(s, ctx + 320, (o + 127) & ~127u);
        if (rd(s, ctx + 1168)) return;
        const u32 t = buf_tag();
        u32 l = b;
        while (n > 0x4000) {
            lsa(l); eal(ea); size(0x4000); tag(t); cmd(0x20);
            n -= 0x4000; ea += 0x4000; l += 0x4000;
        }
        lsa(l); eal(ea); size(n); tag(t); cmd(0x20);
    }
    // 0x4220: ring space for n bytes (rounded to 128); 0 when the ring has none (ctx[1168] = -1)
    u32 ring_alloc(u32 n) {
        const u32 sz = (n + 127) & ~127u, line = rd(s, p + 16);
        for (;;) {
            lsa(ctx); eal(line); cmd(0xD0); atomic();
            u32 wp = rd(s, ctx), end = rd(s, ctx + 16);
            if (wp + sz > end) {
                wp = 0;
                wr(s, ctx + 32, rd(s, ctx + 32) + sz);
            } else {
                wr(s, ctx, wp + sz);
            }
            lsa(ctx); eal(line); cmd(0xB4);
            if (atomic() & 1) continue;
            if (!wp) wr(s, ctx + 1168, ~0u);
            return wp;
        }
    }
    // 0x8690: index array at off (bit 31: location), count u16 indices, primitive prim
    u32 draws(u32 c, u32 off, u32 count, u32 prim) {
        if (!count) return c;
        wr(s, c, 0x0008181C); wr(s, c + 4, off & 0x7FFFFFFF); wr(s, c + 8, (off >> 31) | 16);
        wr(s, c + 12, 0x00041808); wr(s, c + 16, prim & 255);
        s32 first = (s32)((0x200 - (off & 511)) >> 1);
        const s32 cnt = (s32)count;
        const bool big = first > cnt;
        const u32 rest = big ? 0 : (u32)(cnt - first);
        if (big) first = cnt;
        const s32 r2 = (s32)rest >= 0 ? (s32)rest : (s32)rest + 255;
        const u32 full = (u32)(r2 >> 8), part = rest - (full << 8);
        const u32 has_first = first != 0, has_part = part != 0;
        wr(s, c + 20, ((has_first + has_part + full) << 18) | 0x40001824);
        u32 o = c + 24, ix = (u32)first;
        if (has_first) { wr(s, o, ((u32)first - 1) << 24); o = c + 28; }
        for (u32 i = 0; i < full; i++, ix += 256, o += 4) wr(s, o, ix | 0xFF000000);
        if (has_part) { wr(s, o, ((part - 1) << 24) | ix); o += 4; }
        wr(s, o, 0x00041808); wr(s, o + 4, 0);
        return o + 8;
    }

    // what the declined paths are, before anything is written
    bool plan() {
        if (rd(s, ctx + 528) || rd(s, ctx + 1136)) return false;
        if (rd(s, p + 40) || rd(s, p + 44)) return false;
        if ((p & 3) || p + 48 > 0x40000) return false;
        const u32 start = buf_start();
        if (!start) return false;
        u32 b = base(), o = out(), recs = rd(s, ctx + 1184);
        if (rd(s, ctx + 928) == 0) {   // the table goes to the bump pointer, not the output buffer
            const u32 nv = rd(s, ctx + 376);
            if (nv > 0x8000 || rd(s, ctx + 272) + 2 * ((nv + 31) & ~15u) + 16 > 0x40000) return false;
        }
        if (o != b) { b += ((o - b) + 127) & ~127u; o = (o + 127) & ~127u; recs = 0; }
        auto fits = [&](u32 n, u32 align) {   // 0x4C18 without a flush
            const u32 a = (o + align - 1) & (0u - align), n16 = (n + 15) & ~15u;
            if (n16 > start - a + 0x7800 || recs > 15) return false;
            o = a + n16;
            return true;
        };
        if (!fits(rd(s, p + 36), 16)) return false;
        if (o - b > 0x40000) return false;
        b += ((o - b) + 127) & ~127u; o = (o + 127) & ~127u;
        if (rd(s, p + 24) && !fits(0xA0, 0x80)) return false;
        return true;
    }

    // the six done records at ctx + 128 combined (0x90F0 .. 0x91B0): base, and the record quad (word 0 = base)
    void combine(u32& base, u8 rec[16]) {
        u32 b = rd(s, ctx + 128);
        for (int k = 1; k < 6; k++) b = std::max(b, rd(s, ctx + 128 + 16 * k));
        u8 m[16] = {};
        for (int k = 0; k < 6; k++) {
            u32 d = b - rd(s, ctx + 128 + 16 * k);
            if (d > 128) d = 128;
            const u32 sh = (d >> 3) & 31;   // shlqbybi
            const u8* q = s.ls + ctx + 128 + 16 * k;
            for (u32 i = 0; i + sh < 16; i++) m[i] |= q[i + sh];
        }
        while (m[4] == 0xFF) {   // whole bytes of done jobs
            b += 8;
            memmove(m, m + 1, 15);
            m[15] = 0;
        }
        memcpy(rec, m, 16);
        const u32 be = __builtin_bswap32(b);
        memcpy(rec, &be, 4);
        base = b;
    }

    bool run() {
        if (!plan()) return false;
        // 0x8970
        if (rd(s, ctx + 928) == 0) {
            const u32 nv = rd(s, ctx + 376), nv8 = (nv + 7) & ~7u, tab = rd(s, ctx + 272);
            wr(s, ctx + 272, tab + ((2 * ((nv + 31) & ~15u) + 15) & ~15u));
            for (u32 v = 0; v < nv8; v++) { const u32 a = (tab + 2 * v) & 0x3FFFF; s.ls[a] = (u8)((v << 4) >> 8); s.ls[(a + 1) & 0x3FFFF] = (u8)(v << 4); }
            wr(s, ctx + 928, tab); wr(s, ctx + 932, nv); wr(s, ctx + 936, ~0u);
        }
        // 0x44C8(ctx, 0)
        if (out() != base()) {
            const u32 ea = ring_alloc(out() - base()), adj = ea - rd(s, p + 20);
            for (u32 k = 0; k < 16; k++) {
                const u32 r = rd(s, ctx + 1200 + 4 * k);
                if (r) { wr(s, r, rd(s, r) + adj); wr(s, ctx + 1200 + 4 * k, 0); }
            }
            wr(s, ctx + 1184, 0);
            issue(ea);
        }
        const u32 B = rd(s, p + 24);
        if (B) { lsa(ctx + 128); eal(B + 128); size(0x60); tag(rd(s, ctx + 264)); cmd(0x40); }
        // the command block
        const u32 bs = rd(s, p + 36), icount = rd(s, ctx + 384) - rd(s, ctx + 388);
        u32 c = alloc(bs, 16);
        const u32 end = c + (bs & ~3u);
        const u32 desc = rd(s, ctx + 816);
        for (u32 k = 0; k < 8; k++) {
            const u32 rec = rd(s, ctx + 864 + 4 * k);
            const u32 set = (desc + h16(s, desc + 2 * k + 2)) & 0xFFFFFFFF, cnt = h16(s, set);
            if (set == 0 || rec == 0 || cnt == 0) continue;
            for (u32 j = 0; j < cnt; j++) {
                const u32 e = set + 4 + 4 * j, id = s.ls[e & 0x3FFFF];
                for (u32 i = 0; i < 16; i++)
                    if (s.ls[(p + i) & 0x3FFFF] == id) {
                        wr(s, c, 0x00040000 | (0x1680 + 4 * i));
                        wr(s, c + 4, rec + s.ls[(e + 3) & 0x3FFFF]);
                        c += 8;
                        break;
                    }
            }
        }
        if (rd(s, ctx + 1152)) c = draws(c, rd(s, ctx + 404), icount, 5);
        for (; end > c; c += 4) wr(s, c, 0);
        issue(rd(s, p + 32));
        if (B) complete(B);
        if (toggled) s.interrupts = true;
        return spins <= max_spins;
    }

    void complete(u32 B) {
        const u32 k = rd(s, rd(s, 0x12C00)), idx = rd(s, 0x1350) & 0xFFFF;
        const u32 buf = alloc(0xA0, 0x80), recp = buf + 144;
        u32 r22 = 0;
        u8 rec[16];
        u32 r5, w13 = 0, r9 = 0;
    A:  // 0x90B0
        if (++spins > max_spins + 1) return;
        wait_tag(rd(s, ctx + 264));
        {
            u32 base;
            combine(base, rec);
            r5 = idx - base;
        }
        if (r5 > 95) goto RG;
        {
            const u32 sh = (8 - r5) & 63, m = (sh > 31 ? 0 : 0xFFu << sh) & 255;
            const bool in_order = (rec[4] & m) == m && !(r5 > 7);
            const bool not_in_order = !in_order;
            u32 r7, r4, r3;
        C:  // 0x9210
            if (++spins > max_spins + 1) return;
            lsa(ctx); eal(B); cmd(0xD0); atomic();
            {
                const u32 w0 = rd(s, ctx), w4 = rd(s, ctx + 16), w12 = rd(s, ctx + 48);
                w13 = rd(s, ctx + 52);
                r7 = w0 != 0;
                r4 = w4 + 64;
                r9 = idx == w12 - 1;
            }
            if (!(r7 || not_in_order)) {   // 0x96FC: free and in order: take the lock (the `or` reads r9 before 0x92C8 sets it)
                wr(s, ctx, ~0u);
                lsa(ctx); eal(B); cmd(0xB4);
                r22 = (atomic() & 1) == 0;
                goto D;
            }
            if (not_in_order) goto E;
        D:  // 0x92D8
            if (r22 || r7) goto F;
            r22 = 0;
            goto C;
        F:  // 0x9680
            if (!r9) goto G;
            if (r7) goto C;
            r3 = 1; r9 = 1;
            goto H;
        G:  // 0x9A28
            r3 = 0;
            if (r7) goto I;
            goto J;
        E:  // 0x9740
            r3 = r9;
            if (!r7) goto J;
        I:  // 0x9750
            if (r3) goto RG;
            goto K;
        J:  // 0x9A38
            r3 = r9;
        H:  // 0x9694
            if (r22) goto K;
            if (!(r3 || !(r4 > idx))) { done(B, k, recp, rec, r5, true, 0, 0); return; }   // 0x9FCC
            r22 = 0;
            goto RG;
        K:  // 0x9760
            done(B, k, recp, rec, r5, r22 == 0, buf, r9 ? w13 : rd(s, p + 28));
            return;
        }
    RG:  // 0x96C0: the done records again, then wait
        lsa(ctx + 128); eal(B + 128); size(0x60); tag(rd(s, ctx + 264)); cmd(0x40);
        goto A;
    }
    // 0x9760: this job's bit in the record, the record out; with the lock: the kick and the line back
    void done(u32 B, u32 k, u32 recp, u8 rec[16], u32 r5, bool no_kick, u32 buf, u32 kick) {
        rec[4 + ((r5 >> 3) & 15)] |= (u8)(0x80u >> (r5 & 7));   // (r5 <= 95 here)
        memcpy(s.ls + recp, rec, 16);
        cmd(0xC0);
        const u32 t = buf_tag();
        lsa(recp); eal(B + (k << 4) + 128); size(16); tag(t); cmd(0x20);
        if (!no_kick) {
            const u32 idx = rd(s, 0x1350) & 0xFFFF;
            memcpy(s.ls + buf, s.ls + ctx, 128);
            wr(s, buf, 0);
            wr(s, buf + 16, idx);
            wr(s, buf + 128, kick);
            lsa(buf + 128); eal(rd(s, ctx + 32)); size(4); tag(t); cmd(0x20);
            lsa(buf); eal(B); tag(t); cmd(0xB8);
        }
        cmd(0xC0);
    }
};
}  // namespace

// The job's op0 natively (hle_geom.cpp nj_handler): false when declined (nothing written). max_spins: the waits for
// other SPUs it may do (checks: a few, as their snapshot of the lines does not move); returns false after more.
bool geom_op0_native(SPUThread& s, int max_spins) {
    Op0 o{s, s.pref(3) & 0x3FFF0, 0, false, 0, max_spins};
    o.p = s.ls32(o.ctx + 576) & 0x3FFFF;
    return o.run();
}
bool geom_op0_declines(SPUThread& s) {
    Op0 o{s, s.pref(3) & 0x3FFF0, 0, false, 0, 0};
    o.p = s.ls32(o.ctx + 576) & 0x3FFFF;
    return !o.plan();
}
