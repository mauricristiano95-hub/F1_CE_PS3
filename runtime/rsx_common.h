// What the OpenGL and Vulkan renderers share: the RSX register file, the microcode translators, hashes, the block
// heap and the program database (rsx_common.cpp). Index lists and vertex regions: rsx_draw_shared.inl.
#pragma once
#include "rsx.h"
#include <iterator>
#include <set>
#include "ppu_runtime.h"

#include <cstddef>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace rsx {
u32 io_offset_to_ea(u32 io);   // hle_gcm.cpp

// ------------------------------------------------------------------ RSX registers
extern u32 R[0x10000 / 4];
FORCE_INLINE u32 reg(u32 m) { return R[m >> 2]; }
FORCE_INLINE float regf(u32 m) { float f; memcpy(&f, &R[m >> 2], 4); return f; }

extern u32 vp_words[512 * 4];
extern u32 vp_load_pos;
extern float vp_consts[468 * 4];
extern u32 vp_slot_ver[468];
extern u32 vp_ver;
extern u32 vp_const_pos;

constexpr u32 CTX_LOCAL = 0xFEED0000, CTX_MAIN = 0xFEED0001;

u32 location_to_ea(u32 offset, bool main_memory);
u64 mem_fingerprint(u32 ea, u32 bytes);
constexpr u32 MAIN_SURFACE_KEY = 0x80000000;   // g_surfaces key bit for main-memory colour buffers
bool scaled_glsl();
// physical pixels of a guest length; every size and edge goes through this, so they agree at a fractional scale
FORCE_INLINE u32 scaled(u32 v, float k) { return (u32)(v * k + 0.5f); }
FORCE_INLINE s32 scaled_s(s32 v, float k) { return (s32)floorf(v * k + 0.5f); }
// label of the start-up progress bar as window rectangles (pixels, top-left origin), above the line at y = bar_top
struct TextRect { s32 x, y, w, h; };
std::vector<TextRect> progress_label(double frac, const char* what, s32 ww, s32 wh, s32 bar_top);

extern u64 g_frame;
bool trace_on(long long first);
// profiling: microseconds per frame section (reported every 120 frames by flip())
struct Prof { u64 program = 0, textures = 0, vertices = 0, submit = 0, other = 0, draws = 0, verts = 0, frame_start = 0, conv_verts = 0, conv_inst = 0, hashed = 0, tex_hashed = 0, t_index = 0, t_hash = 0, t_conv = 0, conv_new = 0, conv_changed = 0, idx_built = 0, ir_grow = 0, ir_changed = 0, idx_old = 0, t_pre = 0, t_apply = 0, t_upload = 0, t_bind = 0, binds = 0, t_draw = 0, t_ringwait = 0, ring_segs = 0, t_texdec = 0, tex_decoded = 0, memo_hits = 0, memo_miss = 0, t_miss = 0, tex_lin = 0, tex_small = 0, tex_big = 0, t_texhash = 0; };
extern Prof g_prof;
extern const u32 g_watch_fp;

extern u32 g_prim;
struct Range { u32 first, count; };
extern std::vector<Range> g_array_ranges, g_index_ranges;
extern std::vector<u32> g_inline;
extern bool g_vtx_gpu;

// ------------------------------------------------------------------ microcode translators
struct VPInfo { std::string glsl; u32 outputs_used = 0; };
struct FPInfo {
    std::string glsl;
    std::vector<u32> const_offsets;   // byte offsets (from program start) of inline constants
    u32 size = 0;                     // bytes
    u32 tex_used = 0;
};
VPInfo translate_vp(u32 start);
FPInfo translate_fp(u32 ea, u32 shader_control, u32 cube_mask);

// F1CE_CUBE_FACES=<n>: the reflection cube map renders n of its 6 faces per frame, in turn (default 6: all of them, as on
// the RSX). A race sends 26% of its draws there. A skipped face keeps its last image; the game never reads them back.
u32 cube_faces_per_frame();
inline bool cube_face_skipped(const std::set<u32>& faces, u32 offset, u64 frame) {
    static const u32 n = cube_faces_per_frame();
    if (n >= 6) return false;
    auto it = faces.find(offset);
    if (it == faces.end()) return false;
    u32 f = (u32)std::distance(faces.begin(), it) % 6;
    return (f + frame * n) % 6 >= n;
}

// fragment programs are read from guest memory, or from a saved copy while the program database is replayed
extern const u8* g_fp_override;   // big-endian program bytes standing for guest memory at g_fp_override_ea
extern u32 g_fp_override_ea, g_fp_override_size;
FORCE_INLINE u32 fp_word(u32 ea) {
    u32 v;
    if (__builtin_expect(g_fp_override != nullptr, 0)) {
        u32 o = ea - g_fp_override_ea;
        if (o + 4 > g_fp_override_size) return 0;
        memcpy(&v, g_fp_override + o, 4);
        v = __builtin_bswap32(v);
    } else v = ld32(ea);
    return (v << 16) | (v >> 16);
}
extern bool g_fp_stop_here;   // F1CE_FP_STOP applies to the program being translated (F1CE_FP_STOP_HASH)
extern u64 g_fp_hash_cur;     // hash of the program being translated (debug options by hash)

u64 hash_words(const u32* p, size_t n, u64 h = 1469598103934665603ull);
u64 hash_fp(u32 ea, u32& size_out);
std::string shader_cache_dir();

// ------------------------------------------------------------------ program database (see rsx_common.cpp)
struct ProgRecord { u32 magic, vp_start, vp_len, fp_ea, fp_size, shader_control, cube_mask, pad; u64 vh, fh; };
constexpr u32 PROG_MAGIC = 0x46314442;   // "F1DB"
struct ProgEntry { ProgRecord r; std::vector<u32> vp; std::vector<u8> fp; };
std::vector<ProgEntry> load_program_db();   // (recording starts here)
void db_record(u32 vp_start, u32 vp_len, u32 fp_ea, u32 fp_size, u32 shader_control, u32 cube_mask, u64 vh, u64 fh);

// ------------------------------------------------------------------ hashes and small helpers
u64 hash_range(const u8* p, u32 bytes, u32 step);
// guest index buffer -> host list: byte swap, the restart index (rv; 0xFFFF for 16-bit lists when rv >= 0xFFFF) becomes
// 0xFFFFFFFF; lo/hi take the min/max of the other indices (unchanged when there are none)
void convert_indices(const u8* src, u32 n, bool u16idx, bool restart, u32 rv, u32* out, u32& lo, u32& hi);
void index_minmax(const u32* p, u32 n, u32& lo, u32& hi);   // as above, over a converted list
FORCE_INLINE u64 mix64(u64 x) {   // splitmix64 finalizer
    x ^= x >> 30; x *= 0xBF58476D1CE4E5B9ull;
    x ^= x >> 27; x *= 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}
u32 morton(u32 x, u32 y, u32 log_w, u32 log_h);
u32 ilog2(u32 v);
// Texture resolution (F1CE_TEX_RES=<n>): the n largest mip levels of a mipmapped texture are left out (1 = half,
// 2 = quarter size). Textures without mips, linear, main-memory or texel-addressed ones (format bits 0x20/0x40) keep
// their size, and the remaining level 0 keeps at least 32 texels on its short side.
u32 tex_levels_dropped(u32 w, u32 h, u32 levels, u32 fmt, bool main_mem);
u32 tex_level_bytes(u32 w, u32 h, u32 level, u32 base_fmt, u32 bpp);   // guest size of a swizzled or DXT mip level
u32 zpass_value(u32 ea, u64 samples);   // the occlusion count written to report ea for that many counted samples
float half_to_float(u16 h);

// Block heaps: raw vertex data (vertex pulling) and converted index lists that stay for several frames. Blocks are
// never rewritten: changed data gets a new block and the old one is reused only after the GPU finished the frame
// that freed it.
struct BlockHeap {
    static constexpr u32 ALIGN = 256;
    u32 size = 0;
    u32 buf = 0, tex = 0, ring_tex = 0;       // (OpenGL objects)
    u8* ptr = nullptr;                        // persistently mapped (index arena)
    std::map<u32, u32> free_off;              // offset -> size
    std::set<std::pair<u32, u32>> free_size;  // size, offset
    struct Pending { u64 frame; u32 off, size; };
    std::deque<Pending> pending;              // freed blocks waiting for their frame's fence
    std::deque<std::pair<u64, void*>> fences;   // end-of-frame fences (OpenGL)
    u64 done_frame = 0;                       // the GPU finished every frame up to this one
    u32 used = 0, failed = 0;
};
void heap_insert_free(BlockHeap& h, u32 off, u32 size);
u32 heap_alloc(BlockHeap& h, u32 bytes);
void heap_free(BlockHeap& h, u32 off, u32 bytes);

// ------------------------------------------------------------------ 2D engine, the parts on guest memory
bool method_2d_inline(u32 sub, u32 mm, u32 arg);     // NV308A; true when handled
struct Nv0039 { u32 src, dst, len, lines; bool src_main, dst_main; };
bool nv0039_transfer(u32 sub, u32 mm, Nv0039& x);    // NV0039 BUFFER_NOTIFY: true after the memory copy

const char* tex_decode_glsl();
const char* present_glsl();

// Vulkan dialect of the translators: the same programs with their uniforms in buffers, sampled through descriptor
// arrays. Set before translating; off, the output is what the OpenGL renderer has always compiled.
extern bool g_shader_vk;
struct VkDrawParams {   // layout(scalar) block DRAW in the translated programs
    float vp_scale[4], vp_offset[4], surface[4], alpha_test[4], fog[4];
    float res_scale[2], ui_clamp, pad;
    int va[64];
    float tex_flip[16], tex_scale[32], tex_depth[16], remap[16];
    int slot[16], vslot[4];   // descriptor array index of each texture unit / vertex texture unit
};
}  // namespace rsx
