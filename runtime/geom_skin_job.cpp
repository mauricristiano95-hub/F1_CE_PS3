// Native geometry job, skinned pieces (GEOMETRY_JOB.md "Next steps" 1): the shape
//   4 6 8[.. 00140000] 9[3] 12 14 23 24 30 32 35 37 40 0
// (~10% of the jobs at the 60 fps race start: drivers, pit crew). Each command handler is replaced by native code that
// leaves the local store as the SPU handler does (outside the stack), so the SPU handlers that stay (4 12 30 37 0) see
// what they always saw. F1CE_GEOM_SK_CHECK=1 runs both from the same state and compares them.
//
// What the commands do, from the job's code (LS addresses of the job binary) and snapshots (F1CE_GEOM_SNAP_OP=23):
//   op8 (fmt)        vertex format at set 1 + fmt: here two attributes, stride 18: float3 at 0, half3 at 12
//   op40 (0x6A30)    the vertices out, in the input format, to the output buffer at ctx[320] (after op37's indices):
//                    nv rounded up to 4 vertices (size to 16 bytes), position bytes copied (encoder 0xE200), the normal's three floats
//                    to halves (0xE700: fma(x, 1, 0), truncated, exponent < 112 -> 0, >= 144 -> 0x7BFF); before it
//                    (0x8970 -> 0x41C0 / 0x88E0) a table of nv8 halfwords v << 4 at the bump pointer ctx[272],
//                    2 * ((nv + 31) & ~15) bytes, ctx[928] = table, ctx[932] = nv, ctx[936] = -1; after it
//                    ctx[864] = vertex data offset in the output, ctx[1200 + 4 n] = ctx + 864, n = ctx[1184]++.
//                    Arrays (op9 / op32): positions at ctx[800], normals at ctx[804] (16 bytes per vertex).
#include <algorithm>
#include <atomic>
#include <cfloat>
#include <cstring>
#include <immintrin.h>
#include <vector>

#include "kernel.h"
#include "spu.h"

namespace {
inline u16 be16(const u8* p) { return (u16)(p[0] << 8 | p[1]); }
inline void wbe16(u8* p, u32 v) { p[0] = (u8)(v >> 8); p[1] = (u8)v; }
inline u32 be32(const u8* p) { u32 v; memcpy(&v, p, 4); return __builtin_bswap32(v); }
inline u32 f2u_(float f) { u32 v; memcpy(&v, &f, 4); return v; }
inline float u2f_(u32 v) { float f; memcpy(&f, &v, 4); return f; }

std::atomic<u64> g_sk_jobs{0}, g_sk_native[41], g_sk_same{0}, g_sk_differ{0};
const bool g_check = getenv("F1CE_GEOM_SK_CHECK") != nullptr;

// op40: see the header
bool op40(SPUThread& s) {
    const u32 ctx = s.pref(3) & 0x3FFF0;
    const u32 k = (s.gpr[4].w[3] >> 20) & 7;   // (bits 4-6 of the command's low byte)
    if (s.ls32(ctx + 896 + 4 * k) || s.ls32(ctx + 928)) return false;   // a format kernel / a table already made
    const u32 nv = s.ls32(ctx + 376), nv8 = (nv + 7) & ~7u, nv4 = (nv + 3) & ~3u, size = (18 * nv4 + 15) & ~15u;
    const u32 pos = s.ls32(ctx + 800) & 0x3FFF0, nrm = s.ls32(ctx + 804) & 0x3FFF0;
    // the output space (0x4C18: aligned to 16; when the vertices do not fit in the 0x7800-byte buffer it puts the
    // buffer out first and moves to the other one: left to the SPU code)
    const u32 out = (s.ls32(ctx + 320) + 15) & 0x3FFF0, base = s.ls32(ctx + 336) & 0x3FFFF, n = s.ls32(ctx + 1184);
    const u32 start = s.ls32(ctx + 308 + (s.ls32(ctx + 304) & ~3u)) & 0x3FFFF;
    if (!start || size > start - out + 0x7800 || n > 7) return false;
    if (pos + 16 * nv4 > 0x40000 || nrm + 16 * nv4 > 0x40000 || out + size > 0x40000) return false;
    // the table (0x8970)
    const u32 tab = s.ls32(ctx + 272);
    s.ls32(ctx + 272, tab + ((2 * ((nv + 31) & ~15u) + 15) & ~15u));
    for (u32 v = 0; v < nv8; v++) wbe16(s.ls + ((tab + 2 * v) & 0x3FFFF), v << 4);
    s.ls32(ctx + 928, tab);
    s.ls32(ctx + 932, nv);
    s.ls32(ctx + 936, ~0u);
    // the vertices
    u8* o = s.ls + out;
    for (u32 v = 0; v < nv4; v++, o += 18) {
        memcpy(o, s.ls + pos + 16 * v, 12);
        for (int c = 0; c < 3; c++) {
            float x;
            u32 b = be32(s.ls + nrm + 16 * v + 4 * c);
            memcpy(&x, &b, 4);
            x = std::fmaf(x, 1.0f, 0.0f);   // (-0 -> +0; denormals are flushed by the thread's FTZ / DAZ)
            if (x != x) x = 3.40282347e38f;
            memcpy(&b, &x, 4);
            const u32 top4 = (b >> 27) & 15;
            const u32 h = top4 > 8 ? 0x7BFF : top4 < 7 ? 0 : ((b >> 16) & 0xC000) | ((b >> 13) & 0x3FFF);
            wbe16(o + 12 + 2 * c, h);
        }
    }
    s.ls32(ctx + 320, out + size);
    s.ls32(ctx + 864, out - base);
    s.ls32(ctx + 1200 + 4 * n, ctx + 864);
    s.ls32(ctx + 1184, n + 1);
    return true;
}

// op32 (0xB980 -> 0xEFA0 for mode 3: positions and normals): skinning, in place. The program of op23 (one code per
// four vertices, ctx[448]) picks how each group's matrices are built from the palette of op24 (ctx[464], 3x4 rows):
//   code 4   one bone for the group (halfword from ctx[452]), no weights
//   code 6   bones shared by the group (halfwords from ctx[452]), each with a quad of weights (ctx[460])
//   code 0   the previous group's matrices (0xF7A0)
//   code 2   a bone per vertex (four halfwords from ctx[456], as code 10), no weights (0xF52C)
//   code 10  a bone per vertex (four halfwords from ctx[456], vertices 0-3 = halfwords 1 0 3 2), each step with a quad
//            of weights
//   8 / 12 / 14: end. Bit 2 of a bone halfword: another bone follows (code 10: that of vertex 2's halfword).
// Blended rows: fm(w, row) for the first bone, fma(w, row, acc) for the next ones (0xF318 / 0xF380 / 0xF3F8 / 0xF488).
// Then (0xF598, four vertices as lanes, rows A B C D / E F G H / I J K L): positions fma(PY, B, fma(C, PZ, fma(A, PX,
// D))) and so on; normals by the cofactor rows (products rounded or fused as below), scaled by frsqest of their
// squared length. The w words of both arrays keep what the stores' shuffles leave there: nothing reads them (not
// reproduced; the check skips them). Then ctx[272..284] = per word ctx[672 + i] ? ctx[640 + i] : ctx[448 + i].
thread_local u32 t_skip_pos = 0, t_skip_nrm = 0, t_skip_n = 0;   // the check: w words not compared
inline __m128 sat4(__m128 x) { return _mm_max_ps(_mm_min_ps(x, _mm_set1_ps(FLT_MAX)), _mm_set1_ps(-FLT_MAX)); }
inline __m128 vfm(__m128 a, __m128 b) { return sat4(_mm_mul_ps(a, b)); }
inline __m128 vfma(__m128 a, __m128 b, __m128 c) { return sat4(_mm_fmadd_ps(a, b, c)); }
inline __m128 vfms(__m128 a, __m128 b, __m128 c) { return sat4(_mm_fmsub_ps(a, b, c)); }    // a b - c
inline __m128 vfnms(__m128 a, __m128 b, __m128 c) { return sat4(_mm_fnmadd_ps(a, b, c)); }  // c - a b
inline __m128 ldq(const u8* p) {   // big-endian quad
    const __m128i v = _mm_loadu_si128((const __m128i*)p);
    return _mm_castsi128_ps(_mm_shuffle_epi8(v, _mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12)));
}
inline void st3(u8* p, float x, float y, float z) {
    const u32 v[3] = {__builtin_bswap32(f2u_(x)), __builtin_bswap32(f2u_(y)), __builtin_bswap32(f2u_(z))};
    memcpy(p, v, 12);
}
std::atomic<u64> g_sk_decl[32];
bool sk_decline(u32 why) { g_sk_decl[why & 31].fetch_add(1, std::memory_order_relaxed); return false; }
bool op32(SPUThread& s) {
    const u32 ctx = s.pref(3) & 0x3FFF0;
    if (((s.gpr[4].w[3] >> 16) & 15) != 3) return sk_decline(0);   // mode 3 only
    const u32 pal = s.ls32(ctx + 464) & 0x3FFF0, codes = s.ls32(ctx + 448) & 0x3FFFF;
    const u32 pos = s.ls32(ctx + 800) & 0x3FFF0, nrm = s.ls32(ctx + 804) & 0x3FFF0;
    u32 p1 = s.ls32(ctx + 452) & 0x3FFFF, p2 = s.ls32(ctx + 456) & 0x3FFFF, pw = s.ls32(ctx + 460) & 0x3FFF0;
    // the groups' rows first (nothing is written until the whole program is known to be covered)
    // per vertex, rows 0-2 (4 columns each); single: the cofactors of 0xF900 (code 4); zfirst: the normal's z term
    // first (0xF900, 0xF7A0)
    struct Rows { __m128 r[4][3]; bool single, zfirst; };
    thread_local std::vector<Rows> rows;
    u32 ng = 0;
    for (;; ng++) {
        if (codes + ng >= 0x40000 || ng > 0x1000) return sk_decline(3);
        const u32 code = s.ls[codes + ng];
        if (code == 8 || code == 12 || code == 14) break;
        if (rows.size() <= ng) rows.resize(ng + 16);
        Rows& R = rows[ng];
        R.single = code == 4;
        R.zfirst = code == 4;
        if (code == 0) {   // the previous group's matrices again (0xF7A0)
            if (!ng) return sk_decline(10);
            R = rows[ng - 1];
            R.zfirst = true;
        } else if (code == 2) {   // a bone per vertex, no weights (0xF52C)
            if (p2 + 8 > 0x40000) return sk_decline(11);
            const u32 hw[4] = {be16(s.ls + p2 + 2), be16(s.ls + p2), be16(s.ls + p2 + 6), be16(s.ls + p2 + 4)};
            p2 += 8;
            for (int v = 0; v < 4; v++) {
                const u32 m = pal + (hw[v] & ~15u);
                if (m + 48 > 0x40000) return sk_decline(12);
                for (int r = 0; r < 3; r++) R.r[v][r] = ldq(s.ls + m + 16 * r);
            }
        } else if (code == 4) {
            if (p1 + 2 > 0x40000) return sk_decline(1);
            const u32 m = pal + (be16(s.ls + p1) & ~15u);
            p1 += 2;
            if (m + 48 > 0x40000) return false;
            for (int v = 0; v < 4; v++)
                for (int r = 0; r < 3; r++) R.r[v][r] = ldq(s.ls + m + 16 * r);
        } else if (code == 6) {
            for (u32 k = 0;; k++) {
                if (p1 + 2 > 0x40000 || pw + 16 > 0x40000 || k > 8) return sk_decline(4);
                const u32 h = be16(s.ls + p1), m = pal + (h & ~15u);
                p1 += 2;
                if ((h & 3) || m + 48 > 0x40000) return sk_decline(5);
                const __m128 w = ldq(s.ls + pw);
                pw += 16;
                for (int r = 0; r < 3; r++) {
                    const __m128 row = ldq(s.ls + m + 16 * r);
                    for (int v = 0; v < 4; v++) {
                        const __m128 wv = _mm_set1_ps(((const float*)&w)[v]);
                        R.r[v][r] = k ? vfma(wv, row, R.r[v][r]) : vfm(wv, row);
                    }
                }
                if (!(h & 4)) break;
            }
        } else if (code == 10) {
            for (u32 k = 0;; k++) {
                if (p2 + 8 > 0x40000 || pw + 16 > 0x40000 || k > 8) return sk_decline(6);
                const u32 hw[4] = {be16(s.ls + p2 + 2), be16(s.ls + p2), be16(s.ls + p2 + 6), be16(s.ls + p2 + 4)};   // per vertex
                p2 += 8;
                if (hw[2] & 3) return sk_decline(7);
                const __m128 w = ldq(s.ls + pw);
                pw += 16;
                for (int v = 0; v < 4; v++) {
                    const u32 m = pal + (hw[v] & ~15u);
                    if (m + 48 > 0x40000) return sk_decline(8);
                    const __m128 wv = _mm_set1_ps(((const float*)&w)[v]);
                    for (int r = 0; r < 3; r++) {
                        const __m128 row = ldq(s.ls + m + 16 * r);
                        R.r[v][r] = k ? vfma(wv, row, R.r[v][r]) : vfm(wv, row);
                    }
                }
                if (!(hw[2] & 4)) break;
            }
        } else {
            return sk_decline(code < 16 ? 16 + code : 2);
        }
    }
    if (pos + 64 * ng > 0x40000 || nrm + 64 * ng > 0x40000) return sk_decline(9);
    for (u32 g = 0; g < ng; g++) {
        Rows& R = rows[g];
        // rows to lanes: A = row 0 column 0 of vertices 0-3, ...
        __m128 c[3][4];
        for (int r = 0; r < 3; r++) {
            __m128 a = R.r[0][r], b = R.r[1][r], cc = R.r[2][r], d = R.r[3][r];
            _MM_TRANSPOSE4_PS(a, b, cc, d);
            c[r][0] = a; c[r][1] = b; c[r][2] = cc; c[r][3] = d;
        }
        const __m128 A = c[0][0], B = c[0][1], C = c[0][2], D = c[0][3], E = c[1][0], F = c[1][1], G = c[1][2], H = c[1][3];
        const __m128 I = c[2][0], J = c[2][1], K = c[2][2], L = c[2][3];
        __m128 px = ldq(s.ls + pos + 64 * g), py = ldq(s.ls + pos + 64 * g + 16), pz = ldq(s.ls + pos + 64 * g + 32), pw4 = ldq(s.ls + pos + 64 * g + 48);
        _MM_TRANSPOSE4_PS(px, py, pz, pw4);
        __m128 nx = ldq(s.ls + nrm + 64 * g), ny = ldq(s.ls + nrm + 64 * g + 16), nz = ldq(s.ls + nrm + 64 * g + 32), nw4 = ldq(s.ls + nrm + 64 * g + 48);
        _MM_TRANSPOSE4_PS(nx, ny, nz, nw4);
        const __m128 ox = vfma(py, B, vfma(C, pz, vfma(A, px, D)));
        const __m128 oy = vfma(F, py, vfma(G, pz, vfma(E, px, H)));
        const __m128 oz = vfma(J, py, vfma(K, pz, vfma(I, px, L)));
        __m128 c00, c01, c02, c10, c11, c12, c20, c21, c22;
        if (!R.single) {   // (0xF598)
            c00 = vfnms(J, G, vfm(K, F)); c01 = vfnms(K, E, vfm(G, I)); c02 = vfms(J, E, vfm(I, F));
            c10 = vfms(J, C, vfm(K, B)); c11 = vfms(K, A, vfm(I, C)); c12 = vfnms(A, J, vfm(B, I));
            c20 = vfms(G, B, vfm(F, C)); c21 = vfms(E, C, vfm(A, G)); c22 = vfnms(E, B, vfm(A, F));
        } else {   // one matrix for the group (0xF900): other roundings
            c00 = vfnms(J, G, vfm(F, K)); c01 = vfms(G, I, vfm(K, E)); c02 = vfnms(I, F, vfm(J, E));
            c10 = vfms(J, C, vfm(K, B)); c11 = vfnms(I, C, vfm(K, A)); c12 = vfnms(A, J, vfm(B, I));
            c20 = vfnms(F, C, vfm(B, G)); c21 = vfnms(G, A, vfm(C, E)); c22 = vfnms(E, B, vfm(A, F));
        }
        __m128 mx, my, mz;
        if (!R.zfirst) {
            mx = vfma(ny, c01, vfma(c02, nz, vfm(c00, nx)));
            my = vfma(ny, c11, vfma(c12, nz, vfm(c10, nx)));
            mz = vfma(ny, c21, vfma(c22, nz, vfm(c20, nx)));
        } else {
            mx = vfma(c01, ny, vfma(nx, c00, vfm(nz, c02)));
            my = vfma(c11, ny, vfma(nx, c10, vfm(nz, c12)));
            mz = vfma(c21, ny, vfma(nx, c20, vfm(nz, c22)));
        }
        const __m128 len = vfma(mz, mz, vfma(my, my, vfm(mx, mx)));
        const __m128 inv = sat4(_mm_div_ps(_mm_set1_ps(1.0f), _mm_sqrt_ps(_mm_and_ps(len, _mm_castsi128_ps(_mm_set1_epi32(0x7FFFFFFF))))));
        const __m128 qx = vfm(inv, mx), qy = vfm(inv, my), qz = vfm(inv, mz);
        alignas(16) float X[4], Y[4], Z[4], NX[4], NY[4], NZ[4];
        _mm_store_ps(X, ox); _mm_store_ps(Y, oy); _mm_store_ps(Z, oz);
        _mm_store_ps(NX, qx); _mm_store_ps(NY, qy); _mm_store_ps(NZ, qz);
        for (int v = 0; v < 4; v++) {
            st3(s.ls + pos + 64 * g + 16 * v, X[v], Y[v], Z[v]);
            st3(s.ls + nrm + 64 * g + 16 * v, NX[v], NY[v], NZ[v]);
        }
    }
    for (u32 i = 0; i < 16; i += 4) s.ls32(ctx + 272 + i, s.ls32(ctx + 672 + i) ? s.ls32(ctx + 640 + i) : s.ls32(ctx + 448 + i));
    t_skip_pos = pos; t_skip_nrm = nrm; t_skip_n = 4 * ng;
    return true;
}

// the stream bytes from the command on, as the loop hands them to the handler (r4, r5): halfword i
inline u32 arg(const SPUThread& s, int i) {
    const u32 w = s.gpr[4 + i / 8].w[3 - (i % 8) / 2];
    return i & 1 ? w & 0xFFFF : w >> 16;
}
// memcpy of 0xBC80: size / 16 quads, both addresses aligned down (sizes are multiples of 16)
inline void copyq(SPUThread& s, u32 dst, u32 src, u32 size) {
    dst &= 0x3FFF0; src &= 0x3FFF0;
    for (u32 o = 0; o < size; o += 16) memcpy(s.ls + ((dst + o) & 0x3FFF0), s.ls + ((src + o) & 0x3FFF0), 16);
}
// op9 (0xB300) with mask 3 and the format of geom_sk_shape: per attribute, highest mask bit first (normals, then
// positions), an array of nv4 entries of 16 bytes at the bump pointer: every lane i of ctx[272..284] grows by
// (((ctx[368 + 4i] + 3) & ~3) + 4) * 16; ctx[720 + 4n] = array, byte ctx[784 + n] = attribute id, n = ctx[704]
// (every lane of ctx[704..716] + 1), ctx[800 + 4 (id - 1)] = array. Decoders (table 0x12260): float (0xBE00) = the 16
// bytes at the vertex, half (0xBF80) = four halves to floats ((h & 0x7FFF) << 13 + 0x38000000 unless 0, the sign).
bool op9(SPUThread& s) {
    const u32 ctx = s.pref(3) & 0x3FFF0;
    if (arg(s, 0) != 0x0900 || arg(s, 2) != 3 || arg(s, 3)) return sk_decline(13);
    const u32 in = (s.ls32(ctx + 256) + arg(s, 1)) & 0x3FFFF, nv = s.ls32(ctx + 368), nv4 = (nv + 3) & ~3u;
    const u32 n0 = s.ls32(ctx + 704);
    if (n0 > 6 || in + 18 * nv4 + 16 > 0x40000 || nv4 > 0x2000) return sk_decline(14);
    u32 bump[4], grow[4];
    for (int i = 0; i < 4; i++) {
        bump[i] = s.ls32(ctx + 272 + 4 * i);
        grow[i] = (((s.ls32(ctx + 368 + 4 * i) + 3) & ~3u) + 4) << 4;
    }
    if (bump[0] + 2 * grow[0] > 0x40000) return sk_decline(15);
    for (u32 a = 0; a < 2; a++) {
        const u32 id = a ? 1 : 2, arr = bump[0] & 0x3FFFF, n = s.ls32(ctx + 704);
        for (int i = 0; i < 4; i++) bump[i] += grow[i];
        s.ls32(ctx + 720 + 4 * n, arr);
        for (int i = 0; i < 4; i++) s.ls32(ctx + 704 + 4 * i, s.ls32(ctx + 704 + 4 * i) + 1);
        s.ls[ctx + 784 + n] = (u8)id;
        s.ls32(ctx + 800 + 4 * (id - 1), arr);
        u8* o = s.ls + arr;
        if (id == 1) {
            for (u32 v = 0; v < nv4; v++) memcpy(o + 16 * v, s.ls + in + 18 * v, 16);
        } else {
            for (u32 v = 0; v < nv4; v++)
                for (int c = 0; c < 4; c++) {
                    const u32 h = be16(s.ls + in + 18 * v + 12 + 2 * c);
                    const u32 f = (h & 0x8000) << 16 | ((h & 0x7FFF) ? ((h & 0x7FFF) << 13) + 0x38000000 : 0);
                    const u32 b = __builtin_bswap32(f);
                    memcpy(o + 16 * v + 4 * c, &b, 4);
                }
        }
    }
    for (int i = 0; i < 4; i++) s.ls32(ctx + 272 + 4 * i, bump[i]);
    // the w words: positions get the normal's first two halves, normals a fourth half; at the end of the
    // decoders' blocks the SPU leaves other bytes there. Nothing reads them (op32 / op35 / op40 use x y z).
    t_skip_nrm = s.ls32(ctx + 800 + 4); t_skip_pos = s.ls32(ctx + 800); t_skip_n = nv4;
    return true;
}
// op23 (0xB700), low nibble of the command 0 and h8 = 0 only: four buffers at the bump pointer: h2 bytes from set 1 +
// h1 (the bone program, with an end byte 8 after it), h4 from + h3, h6 from + h5, (h9 + 15) & ~15 weights from bytes
// at + h7 (as floats: b / 255);
// ctx[448..460] = the four buffers, every lane of ctx[272..284] moved on as the quad arithmetic of the handler does.
bool op23(SPUThread& s) {
    const u32 ctx = s.pref(3) & 0x3FFF0;
    u32 h[16];
    for (int i = 0; i < 16; i++) h[i] = arg(s, i);
    if ((h[0] & 15) || h[8]) return sk_decline(22);   // (weights given as floats; a second weight set: 0xCD80)
    const u32 set1 = s.ls32(ctx + 256);
    u32 b[4];
    for (int i = 0; i < 4; i++) b[i] = s.ls32(ctx + 272 + 4 * i);
    const u32 r15[4] = {h[2], h[4], h[6], h[0]};
    const u32 A0 = ((h[0] + 15) & 0xFFFF) & ~15u, A2 = ((h[2] + 15) & 0xFFFF) & ~15u, A4 = ((h[4] + 15) & 0xFFFF) & ~15u, A6 = ((h[6] + 15) & 0xFFFF) & ~15u;
    const u32 ra[4] = {A4, A6, A0, A2}, rb[4] = {A6, A0, A2, A4}, low5[4] = {h[9], h[11], h[13], h[15]}, fl[4] = {h[0] & 15, h[2] & 15, h[4] & 15, h[6] & 15};
    u32 r10[4], r2[4], r11[4], r84[4], r83[4];
    for (int i = 0; i < 4; i++) {
        r10[i] = b[i] + ((r15[i] + 16) & ~15u);
        r2[i] = ra[i] + r10[i];
        r11[i] = rb[i] + r2[i];
        r83[i] = ((low5[i] >> (fl[i] ? 2 : 0)) + 15) & ~15u;
        r84[i] = r11[i] + (r83[i] << 2);
    }
    const u32 buf[4] = {b[0], r10[0], r2[0], r11[0]};
    if (r84[0] > 0x40000 || (buf[0] & 15)) return sk_decline(23);
    for (int i = 0; i < 4; i++) s.ls32(ctx + 272 + 4 * i, r84[i]);
    for (int i = 0; i < 4; i++) s.ls32(ctx + 448 + 4 * i, buf[i]);
    const u32 src0 = set1 + h[1], end = (src0 + h[2]) & 0x3FFFF;
    u8 q[16];
    memcpy(q, s.ls + (end & 0x3FFF0), 16);
    q[end & 15] = 8;
    copyq(s, buf[0], src0, ((h[2] + 16) & ~15u) - 16);
    memcpy(s.ls + ((buf[1] - 16) & 0x3FFF0), q, 16);
    copyq(s, buf[1], set1 + h[3], A4);
    copyq(s, buf[2], set1 + h[5], A6);
    for (u32 i = 0; i < r83[0]; i++) {   // the weights: bytes to floats (0xC180: csflt, fma(x, 1 / 255, 0))
        const float w = std::fmaf((float)s.ls[(set1 + h[7] + i) & 0x3FFFF], u2f_(0x3B808081), 0.0f);
        s.ls32(buf[3] + 4 * i, f2u_(w));
    }
    return true;
}
// op24 (0xB280), low nibble of the command 0 only (else plain 48-byte copies): (h2 + 1) & ~1 matrices from set 1 + h1
// to the bump pointer, each 4x4 at the source and its first three columns as rows here (0xC580, in pairs);
// ctx[464..476] = the old ctx[272..284], every lane i of ctx[272..284] + ((h_{2,4,6,0} + 1) & ~1) * 48 (16-bit sums).
bool op24(SPUThread& s) {
    const u32 ctx = s.pref(3) & 0x3FFF0;
    const u32 h0 = arg(s, 0), h1 = arg(s, 1);
    if (h0 & 15) return sk_decline(20);   // (0xBC80: matrices stored as rows already)
    const u32 hs[4] = {arg(s, 2), arg(s, 4), arg(s, 6), h0};
    u32 b[4];
    for (int i = 0; i < 4; i++) b[i] = s.ls32(ctx + 272 + 4 * i);
    auto size = [](u32 hv) { return (u32)((s32)(s16)(u16)(((hv + 1) & 0xFFFF) & ~1u) * 48); };
    const u32 src = (s.ls32(ctx + 256) + h1) & 0x3FFF0, nm = 2 * ((hs[0] + 1) >> 1);
    if (hs[0] > 0x400 || b[0] + 48 * nm > 0x40000 || (b[0] & 15) || src + 64 * nm > 0x40000) return sk_decline(21);
    for (int i = 0; i < 4; i++) s.ls32(ctx + 464 + 4 * i, b[i]);
    for (int i = 0; i < 4; i++) s.ls32(ctx + 272 + 4 * i, b[i] + size(hs[i]));
    for (u32 m = 0; m < nm; m++)
        for (u32 r = 0; r < 3; r++)
            for (u32 c = 0; c < 4; c++) memcpy(s.ls + b[0] + 48 * m + 16 * r + 4 * c, s.ls + src + 64 * m + 16 * c + 4 * r, 4);
    return true;
}

// op12 (0x7C50), u16 triangles (low nibble of the command 0): ntri = h2 at set 1 + h1 to (a b c a) entries of 8 bytes
// (0xBD80, eight a step: round_up8(ntri) entries, the last ones from the bytes after the triangles) in
// round_up8(ntri) * 8 bytes at the bump pointer (0x41C0: word 0 of ctx[272] only); ctx[400] = list, ctx[384] = 3 ntri,
// ctx[388] = 3 h3. Covered and skinned jobs.
}  // namespace
// hle_geom.cpp, covered jobs: the native op35 writes the kept entries; of the others only those op37 packs as padding
// after them are read (entries kn .. round_up8(kn) + 8): the list is filled there only
thread_local bool t_geom_list_dead = false;
thread_local u32 t_geom_list_kept = 0;
namespace {
bool op12(SPUThread& s) {
    const u32 ctx = s.pref(3) & 0x3FFF0;
    const u32 h0 = arg(s, 0), h2 = arg(s, 2), h3 = arg(s, 3);
    const u32 src = (s.ls32(ctx + 256) + arg(s, 1)) & 0x3FFFF, list = s.ls32(ctx + 272), nt = (h2 + 7) & ~7u;
    if ((h0 & 15) || (src & 15) || (list & 15) || src + 6 * nt > 0x40000 || list + 8 * nt > 0x40000) return sk_decline(25);
    s.ls32(ctx + 272, list + 8 * nt);
    s.ls32(ctx + 400, list);
    const u8* q = s.ls + src;
    u8* o = s.ls + list;
    u32 t0 = 0, t1 = nt;
    if (t_geom_list_dead && !g_check) { t0 = std::min(t_geom_list_kept, nt); t1 = std::min(((t_geom_list_kept + 7) & ~7u) + 8, nt); }
    q += 6 * t0; o += 8 * t0;
    for (u32 t = t0; t < t1; t++, q += 6, o += 8) { memcpy(o, q, 6); memcpy(o + 6, q, 2); }
    s.ls32(ctx + 384, 3 * h2);
    s.ls32(ctx + 388, 3 * h3);
    return true;
}

// op4 (0x7790): the job's parameters, 0x90 bytes from set 1 + h1 to the bump pointer (0x7580 -> 0x41C0 / 0xBA80),
// ctx[576] = them; when parameter word 0x28 is set, a 16-byte GET from parameter word 16 + 16 * SPU number to
// ctx + 240, tag ctx[264], between an interrupt disable and an enable.
bool op4(SPUThread& s) {
    const u32 ctx = s.pref(3) & 0x3FFF0;
    const u32 src = (s.ls32(ctx + 256) + arg(s, 1)) & 0x3FFFF, dst = s.ls32(ctx + 272);
    if ((dst & 15) || (src & 15) || dst + 0xD0 > 0x40000 || src + 0xD0 > 0x40000) return sk_decline(26);
    s.ls32(ctx + 272, dst + 0x90);
    // 0xBA80 with both addresses aligned: 64-byte blocks, then one more store at + 0xC0 of the quad that was at
    // + 0x80 before the copy (its tail merge picks the old bytes there entirely)
    u8 old80[16];
    memcpy(old80, s.ls + dst + 0x80, 16);
    memmove(s.ls + dst, s.ls + src, 0x90);
    memcpy(s.ls + dst + 0xC0, old80, 16);
    s.ls32(ctx + 576, dst);
    if (s.ls32(dst + 0x28)) {
        s.mfc_lsa = ctx + 240;
        s.mfc_eal = s.ls32(dst + 16) + (s.ls32(s.ls32(0x12C00)) << 4);
        s.mfc_size = 16;
        s.mfc_tag = s.ls32(ctx + 264);
        s.dma(0x40);
        s.interrupts = true;
    }
    return true;
}

bool native(SPUThread& s, u32 handler) {
    switch (handler) {
        case 0x07790: return op4(s);
        case 0x07C50: return op12(s);
        case 0x0B300: return op9(s);
        case 0x0B700: return op23(s);
        case 0x0B280: return op24(s);
        case 0x06A30: return op40(s);
        case 0x0B980: return op32(s);
        default: return false;
    }
}
u32 op_of(u32 handler) {
    static const u32 h[41] = {0x08C80, 0x05818, 0x05860, 0x058F0, 0x07790, 0x07858, 0x079B0, 0x07A28, 0x07888, 0x0B300,
                              0x08600, 0x07C08, 0x07C50, 0x07D78, 0x07E40, 0x07E70, 0x07EA0, 0x07ED0, 0x07FA8, 0x07FD8,
                              0x08008, 0x08120, 0x08150, 0x0B700, 0x0B280, 0x08B88, 0x056B8, 0x07958, 0x05148, 0x08C10,
                              0x09FD8, 0x0B020, 0x0B980, 0x0A098, 0x0A530, 0x0AFD0, 0x0AFF8, 0x05D30, 0x073D0, 0x05E28,
                              0x06A30};
    for (u32 i = 0; i < 41; i++)
        if (h[i] == handler) return i;
    return 0;
}
}  // namespace

// set1: the job's gathered input. True for the shape this file covers.
bool geom_sk_shape(const u8* set1, u32 set1_size) {
    static const u8 want[] = {4, 6, 8, 9, 12, 14, 23, 24, 30, 32, 35, 37, 40, 0};
    static const u8 lens[41] = {1, 0, 0, 0, 2, 2, 3, 3, 3, 4, 2, 3, 4, 7, 2, 2, 2, 3, 2, 2, 3, 2, 3, 10, 3, 4, 3, 2, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 1, 1};
    u32 i = 1;
    for (u8 op : want) {
        if (i >= 0x40 || set1[2 * i] != op) return false;
        auto arg = [&](u32 k) { return (u32)be16(set1 + 2 * (i + k)); };
        if (op == 9 && arg(2) != 3) return false;
        if (op == 40 && (set1[2 * i + 1] || arg(1) || arg(2) || arg(3))) return false;
        if (op == 8) {   // two attributes, stride 18: float3 (id 1) at 0, half3 (id 2) at 12
            const u32 f = arg(1);
            if (f + 16 > set1_size || be32(set1 + f + 4) != 0x00140000 || be32(set1 + f + 8) != 0x00020012 ||
                be32(set1 + f + 12) != 0x01030000 || be32(set1 + f + 16) != 0x0213000C)
                return false;
        }
        i += lens[op];
    }
    g_sk_jobs.fetch_add(1, std::memory_order_relaxed);
    return true;
}

// A command handler of a skinned job (hle_geom.cpp nj_handler): true when it ran natively (or, checking, the SPU
// handler ran after the native one was compared with it), false to leave it to the SPU code.
bool geom_sk_handler(SPUThread& s, u32 handler) {
    if (!g_check) {
        if (!native(s, handler)) return false;
        g_sk_native[op_of(handler)].fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    // check: the native handler, then the SPU one from the same state; the SPU's result is kept
    thread_local std::vector<u8> before, mine;
    before.assign(s.ls, s.ls + 0x40000);
    v128 gpr[128];
    memcpy(gpr, s.gpr, sizeof(gpr));
    const u32 sp = s.pref(1);
    t_skip_n = 0;
    if (!native(s, handler)) return false;
    g_sk_native[op_of(handler)].fetch_add(1, std::memory_order_relaxed);
    mine.assign(s.ls, s.ls + 0x40000);
    s.write_ls(0, before.data(), 0x40000);
    memcpy(s.gpr, gpr, sizeof(gpr));
    s.pc = handler;
    s.run_until(0x5C88);
    if (s.halted || s.pc != 0x5C88) fatal("geom sk: handler %05x did not return (pc %05x)", handler, s.pc);
    const u32 end = (sp - 0x2000) & 0x3FFF0;   // the stack below the caller's frame is the handlers' scratch
    std::string d;
    u32 ndiff = 0;
    auto skipped = [&](u32 a) {
        if (!t_skip_n || (a & 15) != 12) return false;
        return (a >= t_skip_pos && a < t_skip_pos + 16 * t_skip_n) || (a >= t_skip_nrm && a < t_skip_nrm + 16 * t_skip_n);
    };
    for (u32 a = 0; a < end; a += 4)
        if (memcmp(s.ls + a, mine.data() + a, 4) && !skipped(a)) {
            if (ndiff++ < 12) {
                char b[48];
                snprintf(b, sizeof(b), " %05x %08x/%08x", a, s.ls32(a), be32(mine.data() + a));
                d += b;
            }
        }
    (ndiff ? g_sk_differ : g_sk_same).fetch_add(1, std::memory_order_relaxed);
    if (ndiff) {
        static std::atomic<int> logged{0};
        const u32 c = gpr[3].w[3] & 0x3FFF0;
        auto bw = [&](u32 o) { return be32(before.data() + ((c + o) & 0x3FFFC)); };
        if (logged++ < 20)
            LOG_W("geom sk check: op%u (ctx %05x) %u words differ (SPU/native):%s | before: 320 %x 304 %x 308 %x 312 %x 336 %x 376 %x 800 %x "
                  "804 %x 1184 %x; SPU after: 864 %x", op_of(handler), c, ndiff, d.c_str(), bw(320), bw(304), bw(308), bw(312), bw(336),
                  bw(376), bw(800), bw(804), bw(1184), s.ls32(c + 864));
    }
    return true;
}

void geom_sk_report() {
    if (!g_sk_jobs.load()) return;
    std::string o;
    for (u32 i = 0; i < 41; i++)
        if (const u64 n = g_sk_native[i].load()) {
            char b[32];
            snprintf(b, sizeof(b), " op%u %llu", i, (unsigned long long)n);
            o += b;
        }
    std::string dc;
    for (u32 i = 0; i < 32; i++)
        if (const u64 n = g_sk_decl[i].load()) {
            char b[32];
            snprintf(b, sizeof(b), " %u:%llu", i, (unsigned long long)n);
            dc += b;
        }
    LOG_I("geom sk: %llu skinned jobs, native:%s; check %llu same, %llu differ; op32 declined (reason:count):%s", (unsigned long long)g_sk_jobs.load(),
          o.c_str(), (unsigned long long)g_sk_same.load(), (unsigned long long)g_sk_differ.load(), dc.c_str());
}
