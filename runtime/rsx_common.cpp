// Parts shared by the OpenGL and Vulkan renderers: RSX register file, microcode translation to GLSL, hashes, the
// block allocator and the program database. Only one renderer runs in a process (rsx.cpp picks it).
#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif
#include "rsx_common.h"
#include "kernel.h"

#include <algorithm>
#include <filesystem>
#include <immintrin.h>
#include <set>
#include <sstream>

namespace rsx {
// F1CE_TRACE_FRAME=<n> traces frame n; F1CE_TRACE_COUNT=<k> extends it to frames n .. n+k-1
bool trace_on(long long first) {
    static const long long count = getenv("F1CE_TRACE_COUNT") ? atoll(getenv("F1CE_TRACE_COUNT")) : 1;
    return first >= 0 && (long long)g_frame >= first && (long long)g_frame < first + count;
}

// ------------------------------------------------------------------ RSX registers
u32 R[0x10000 / 4];
u32 vp_words[512 * 4];          // vertex program memory
u32 vp_load_pos = 0;            // in words
float vp_consts[468 * 4];
// per constant: value of vp_ver when it was last written; a program re-uploads only the constants newer than its upload
u32 vp_slot_ver[468];
u32 vp_ver = 1;
u32 vp_const_pos = 0;           // in floats
u64 g_frame = 0;
Prof g_prof;
// debug: F1CE_WATCH_FP=<local offset> logs 2D-engine writes into [offset, offset + 0x800) and the constants of draws
// that use the fragment program at that offset
const u32 g_watch_fp = getenv("F1CE_WATCH_FP") ? (u32)strtoul(getenv("F1CE_WATCH_FP"), nullptr, 16) : ~0u;
u32 g_prim = 0;
std::vector<Range> g_array_ranges, g_index_ranges;
std::vector<u32> g_inline;
bool g_vtx_gpu = false;   // vertex pulling on (F1CE_VTX_CPU=1: CPU conversion into float4 attribute buffers)
bool g_shader_vk = false;   // the translators emit Vulkan GLSL (see the end of rsx_common.h)

// fingerprint of [ea, ea + bytes): 512 words sampled evenly
u64 mem_fingerprint(u32 ea, u32 bytes) {
    u64 h = 1469598103934665603ull;
    u32 step = std::max(4u, (bytes / 512) & ~3u);
    for (u32 o = 0; o + 4 <= bytes; o += step) { u32 v; memcpy(&v, gmem(ea + o), 4); h = (h ^ v) * 1099511628211ull; }
    return h;
}
u32 location_to_ea(u32 offset, bool main_memory) {
    return main_memory ? io_offset_to_ea(offset) : mem::RSX_LOCAL + offset;
}

// Resolution scaling (roadmap step 3, F1CE_SCALE, default 1): local render targets and depth buffers are
// allocated at scale x their guest size and drawn with a scaled viewport and scissor; the vertex programs map window
// coordinates through the guest size, so geometry needs no change. Main-memory targets (read back by the CPU) stay
// native. Fragment programs see WPOS in guest pixels (u_res_scale), occlusion counts are divided by scale^2 (the
// game picks levels of detail and flare intensity from them), cube maps and copies use the physical sizes.
// The fragment programs read their scale from uniforms (u_res_scale, u_ui_clamp), so one source serves every scale
// and the shader cache is shared: changing F1CE_SCALE no longer compiles every program again (the Driver garage stayed
// white for seconds the first time at a new scale). F1CE_NATIVE_GLSL=1: the scale-1 sources of before.
// The formats are in hle_aspect.cpp.
bool scaled_glsl() { static const bool v = !getenv("F1CE_NATIVE_GLSL"); return v; }

// 5x7 font of the progress label: the renderers have no text path of their own, so each lit run of a row is a rectangle
// they clear (no shaders are ready at this point)
std::vector<TextRect> progress_label(double frac, const char* what, s32 ww, s32 wh, s32 bar_top) {
    static const struct { char c; u8 rows[7]; } font[] = {
        {'L', {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}}, {'a', {0x00, 0x00, 0x0E, 0x01, 0x0F, 0x11, 0x0F}},
        {'d', {0x01, 0x01, 0x0D, 0x13, 0x11, 0x11, 0x0F}}, {'e', {0x00, 0x00, 0x0E, 0x11, 0x1F, 0x10, 0x0E}},
        {'g', {0x00, 0x00, 0x0F, 0x11, 0x0F, 0x01, 0x0E}}, {'h', {0x10, 0x10, 0x16, 0x19, 0x11, 0x11, 0x11}},
        {'i', {0x04, 0x00, 0x0C, 0x04, 0x04, 0x04, 0x0E}}, {'n', {0x00, 0x00, 0x16, 0x19, 0x11, 0x11, 0x11}},
        {'o', {0x00, 0x00, 0x0E, 0x11, 0x11, 0x11, 0x0E}}, {'r', {0x00, 0x00, 0x16, 0x19, 0x10, 0x10, 0x10}},
        {'s', {0x00, 0x00, 0x0F, 0x10, 0x0E, 0x01, 0x1E}}, {'.', {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C}},
        {'%', {0x18, 0x19, 0x02, 0x04, 0x08, 0x13, 0x03}}, {'0', {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}},
        {'1', {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}}, {'2', {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}},
        {'3', {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E}}, {'4', {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}},
        {'5', {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E}}, {'6', {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}},
        {'7', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}}, {'8', {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}},
        {'9', {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}},
    };
    char text[64];
    snprintf(text, sizeof(text), "Loading %s... %d%%", what, (int)(std::clamp(frac, 0.0, 1.0) * 100));
    const s32 cell = std::max(2, wh / 200), len = (s32)strlen(text);
    const s32 x0 = (ww - (len * 6 - 1) * cell) / 2, y0 = bar_top - 12 * cell;
    std::vector<TextRect> out;
    for (s32 i = 0; i < len; i++)
        for (const auto& g : font) {
            if (g.c != text[i]) continue;
            for (s32 row = 0; row < 7; row++)
                for (s32 col = 0; col < 5; col++) {
                    if (!((g.rows[row] >> (4 - col)) & 1)) continue;
                    s32 run = 1;
                    while (col + run < 5 && ((g.rows[row] >> (4 - col - run)) & 1)) run++;
                    out.push_back({x0 + (i * 6 + col) * cell, y0 + row * cell, run * cell, cell});
                    col += run - 1;
                }
        }
    return out;
}

// ------------------------------------------------------------------ shaders
// Vulkan dialect: set 0 holds the vertex constants (0), the draw's parameters (1), the fragment constants (2) and the
// vertex pulling buffers (3, 4), set 1 the textures as descriptor arrays; u_slot gives each unit's index in them.
static const char* vk_header() {
    return "#version 450 core\n"
           "#extension GL_EXT_scalar_block_layout : require\n"
           "#extension GL_EXT_nonuniform_qualifier : require\n"
           "layout(scalar, set = 0, binding = 1) uniform DRAW {\n"
           " vec4 u_vp_scale; vec4 u_vp_offset; vec4 u_surface; vec4 u_alpha_test; vec4 u_fog;\n"
           " vec2 u_res_scale; float u_ui_clamp; float u_pad;\n"
           " ivec4 u_va[16];\n"
           " float u_tex_flip[16]; vec2 u_tex_scale[16]; float u_tex_depth[16]; float u_remap[16];\n"
           " int u_slot[16]; int u_vslot[4];\n"
           "};\n"
           "layout(set = 1, binding = 0) uniform sampler2D g_tex[];\n"
           "layout(set = 1, binding = 1) uniform samplerCube g_cube[];\n";
}
const char* SWZ = "xyzw";

std::string swizzle_str(u32 sx, u32 sy, u32 sz, u32 sw) {
    std::string s = ".";
    s += SWZ[sx]; s += SWZ[sy]; s += SWZ[sz]; s += SWZ[sw];
    return s;
}

std::string mask_str(bool x, bool y, bool z, bool w) {
    std::string s;
    if (x) s += 'x';
    if (y) s += 'y';
    if (z) s += 'z';
    if (w) s += 'w';
    return s;
}

// assigns a vec4 expression to dst through a write mask
std::string masked_assign(const std::string& dst, const std::string& mask, const std::string& expr, bool sat) {
    std::string e = sat ? "clamp(" + expr + ", 0.0, 1.0)" : expr;
    if (mask.size() == 4) return dst + " = " + e + ";";
    return dst + "." + mask + " = (" + e + ")." + mask + ";";
}

// ---------------- vertex program

std::string vp_src(u32 src, u32 d1, bool abs_flag, u32 d3, u32 d0) {
    u32 type = src & 3, tmp = (src >> 2) & 0x3F;
    u32 sw = (src >> 8) & 3, sz = (src >> 10) & 3, sy = (src >> 12) & 3, sx = (src >> 14) & 3;
    bool neg = (src >> 16) & 1;
    std::string r;
    switch (type) {
        case 1: r = "R[" + std::to_string(tmp) + "]"; break;
        case 2: r = "in_attr" + std::to_string((d1 >> 8) & 0xF); break;
        case 3: {
            u32 ci = (d1 >> 12) & 0x3FF;
            if (d3 & 2) r = "C[clamp(" + std::to_string(ci) + " + A[" + std::to_string((d0 >> 24) & 1) + "]." + SWZ[d0 & 3] + ", 0, 467)]";
            else r = "C[" + std::to_string(ci) + "]";
            break;
        }
        default: r = "vec4(0.0)";
    }
    r = "(" + r + ")" + swizzle_str(sx, sy, sz, sw);
    if (abs_flag) r = "abs(" + r + ")";
    if (neg) r = "-" + r;
    return r;
}

// highest constant index used with array `name` ("R" matches R[n] but not FC[n]) + 1; at least `min_n`
int array_size(const std::string& src, const char* name, int min_n) {
    int n = min_n;
    size_t len = strlen(name);
    for (size_t i = src.find(name); i != std::string::npos; i = src.find(name, i + 1)) {
        if (i > 0 && (isalnum((unsigned char)src[i - 1]) || src[i - 1] == '_')) continue;
        if (src.compare(i + len, 1, "[") != 0) continue;
        n = std::max(n, atoi(src.c_str() + i + len + 1) + 1);
    }
    return n;
}
// Vertex pulling (roadmap step 2): the vertex program reads the raw big-endian guest records itself from two buffer
// textures (the stream ring and the vertex heap) and decodes the RSX formats, as fetch_attr() does on the CPU.
// u_va[i]: x = byte offset of record 0 in its buffer, y = stride, z = type | size << 4 | modulo << 8 | ring << 9
// (type 0: attribute disabled, reads (0, 0, 0, 1)), w = frequency. gl_VertexID is the guest vertex index.
bool vp_uses_input(const std::string& body, int i) {   // "in_attr<i>" not followed by another digit
    std::string n = "in_attr" + std::to_string(i);
    for (size_t q = body.find(n); q != std::string::npos; q = body.find(n, q + 1))
        if (q + n.size() >= body.size() || !isdigit((unsigned char)body[q + n.size()])) return true;
    return false;
}

std::string vp_fetch_glsl(const std::string& body) {
    std::string s = g_shader_vk
        ? "layout(std430, set = 0, binding = 3) readonly buffer VaRing { uint d[]; } va_ring;\n"
          "layout(std430, set = 0, binding = 4) readonly buffer VaHeap { uint d[]; } va_heap;\n"
          "uint va_w(int a, bool r) { return r ? va_ring.d[a >> 2] : va_heap.d[a >> 2]; }\n"
        : "uniform usamplerBuffer u_va_ring;\nuniform usamplerBuffer u_va_heap;\nuniform ivec4 u_va[16];\n"
          "uint va_w(int a, bool r) { return r ? texelFetch(u_va_ring, a >> 2).r : texelFetch(u_va_heap, a >> 2).r; }\n";
    s +=
        "uint va_u8(int a, bool r) { return (va_w(a, r) >> uint((a & 3) * 8)) & 255u; }\n"
        "uint va_u16(int a, bool r) {\n"
        " if ((a & 1) == 0) { uint x = (va_w(a, r) >> uint((a & 2) * 8)) & 0xFFFFu; return ((x & 255u) << 8) | (x >> 8); }\n"
        " return (va_u8(a, r) << 8) | va_u8(a + 1, r);\n"
        "}\n"
        "uint va_u32(int a, bool r) {\n"
        " if ((a & 3) == 0) { uint w = va_w(a, r); return (w << 24) | ((w & 0xFF00u) << 8) | ((w >> 8) & 0xFF00u) | (w >> 24); }\n"
        " return (va_u16(a, r) << 16) | va_u16(a + 2, r);\n"
        "}\n"
        "float va_s16(uint x) { return float(int(x << 16) >> 16); }\n"
        "vec4 va_fetch(int i) {\n"
        " ivec4 d = u_va[i];\n"
        " int type = d.z & 15, size = min((d.z >> 4) & 15, 4);\n"
        " vec4 o = vec4(0.0, 0.0, 0.0, 1.0);\n"
        " if (type == 0) return o;\n"
        " bool r = (d.z & 512) != 0;\n"
        " int v = VERTEX_INDEX;\n"
        " if (d.w > 1) v = (d.z & 256) != 0 ? v % d.w : v / d.w;\n"
        " int a = d.x + v * d.y;\n"
        " if (type == 6) {   // CMP 11:11:10\n"
        "  uint p = va_u32(a, r);\n"
        "  if (size > 0) o.xyz = vec3(float(int(p << 21) >> 21) / 1023.0, float(int(p << 10) >> 21) / 1023.0, float(int(p) >> 22) / 511.0);\n"
        "  return o;\n"
        " }\n"
        " for (int k = 0; k < 4; k++) {\n"
        "  if (k >= size) break;\n"
        "  if (type == 1) o[k] = va_s16(va_u16(a + k * 2, r)) / 32767.0;\n"
        "  else if (type == 2) o[k] = uintBitsToFloat(va_u32(a + k * 4, r));\n"
        "  else if (type == 3) o[k] = unpackHalf2x16(va_u16(a + k * 2, r)).x;\n"
        "  else if (type == 4) o[k] = float(va_u8(a + k, r)) / 255.0;\n"
        "  else if (type == 5) o[k] = va_s16(va_u16(a + k * 2, r));\n"
        "  else if (type == 7) o[k] = float(va_u8(a + k, r));\n"
        " }\n"
        " return o;\n"
        "}\n";
    for (size_t q; (q = s.find("VERTEX_INDEX")) != std::string::npos;) s.replace(q, 12, g_shader_vk ? "gl_VertexIndex" : "gl_VertexID");
    for (int i = 0; i < 16; i++)
        if (vp_uses_input(body, i)) s += "vec4 in_attr" + std::to_string(i) + ";\n";
    return s;
}

VPInfo translate_vp(u32 start) {
    VPInfo info;
    std::ostringstream body;
    for (u32 slot = start; slot < 512; slot++) {
        const u32* w = &vp_words[slot * 4];
        u32 d0 = w[0], d1 = w[1], d2 = w[2], d3 = w[3];
        u32 vec_op = (d1 >> 22) & 0x1F, sca_op = (d1 >> 27) & 0x1F;
        u32 src0 = ((d1 & 0xFF) << 9) | (d2 >> 23);
        u32 src1 = (d2 >> 6) & 0x1FFFF;
        u32 src2 = ((d2 & 0x3F) << 11) | (d3 >> 21);
        bool sat = (d0 >> 26) & 1, vec_result = (d0 >> 30) & 1;
        u32 vec_tmp = (d0 >> 15) & 0x3F, dst = (d3 >> 2) & 0x1F, sca_tmp = (d3 >> 7) & 0x3F;
        std::string vmask = mask_str((d3 >> 16) & 1, (d3 >> 15) & 1, (d3 >> 14) & 1, (d3 >> 13) & 1);
        std::string smask = mask_str((d3 >> 20) & 1, (d3 >> 19) & 1, (d3 >> 18) & 1, (d3 >> 17) & 1);
        std::string a = vp_src(src0, d1, (d0 >> 21) & 1, d3, d0);
        std::string b = vp_src(src1, d1, (d0 >> 22) & 1, d3, d0);
        std::string c = vp_src(src2, d1, (d0 >> 23) & 1, d3, d0);

        std::string ve;
        switch (vec_op) {
            case 0: break;
            case 1: ve = a; break;
            case 2: ve = a + " * " + b; break;
            case 3: ve = a + " + " + c; break;   // ADD uses src0 + src2
            case 4: ve = a + " * " + b + " + " + c; break;
            case 5: ve = "vec4(dot((" + a + ").xyz, (" + b + ").xyz))"; break;
            case 6: ve = "vec4(dot(vec4((" + a + ").xyz, 1.0), " + b + "))"; break;
            case 7: ve = "vec4(dot(" + a + ", " + b + "))"; break;
            case 8: ve = "vec4(1.0, (" + a + ").y * (" + b + ").y, (" + a + ").z, (" + b + ").w)"; break;
            case 9: ve = "min(" + a + ", " + b + ")"; break;
            case 10: ve = "max(" + a + ", " + b + ")"; break;
            case 11: ve = "vec4(lessThan(" + a + ", " + b + "))"; break;
            case 12: ve = "vec4(greaterThanEqual(" + a + ", " + b + "))"; break;
            case 13: body << "A[" << ((d0 >> 24) & 1) << "] = ivec4(floor(" << a << "));\n"; break;
            case 14: ve = "fract(" + a + ")"; break;
            case 15: ve = "floor(" + a + ")"; break;
            case 16: ve = "vec4(equal(" + a + ", " + b + "))"; break;
            case 17: ve = "vec4(0.0)"; break;
            case 18: ve = "vec4(greaterThan(" + a + ", " + b + "))"; break;
            case 19: ve = "vec4(lessThanEqual(" + a + ", " + b + "))"; break;
            case 20: ve = "vec4(notEqual(" + a + ", " + b + "))"; break;
            case 21: ve = "vec4(1.0)"; break;
            case 22: ve = "sign(" + a + ")"; break;
            case 25: ve = "textureLod(vtex" + std::to_string((d2 >> 8) & 3) + ", (" + a + ").xy, 0.0)"; break;   // TXL (vertex texture)
            default: LOG_W("rsx vp: unsupported vector op %u", vec_op);
        }
        std::string se;
        std::string cx = "(" + c + ").x";
        switch (sca_op) {
            case 0: break;
            case 1: se = "vec4(" + cx + ")"; break;
            case 2: se = "vec4(1.0 / " + cx + ")"; break;
            case 3: se = "vec4(clamp(1.0 / " + cx + ", -1.884467e19, 1.884467e19))"; break;
            case 4: se = "vec4(inversesqrt(abs(" + cx + ")))"; break;
            case 5: se = "vec4(exp2(floor(" + cx + ")), fract(" + cx + "), exp2(" + cx + "), 1.0)"; break;
            case 6: se = "vec4(floor(log2(abs(" + cx + "))), 1.0, log2(abs(" + cx + ")), 1.0)"; break;
            case 7: se = "vec4(1.0, max((" + c + ").x, 0.0), ((" + c + ").x > 0.0) ? pow(max((" + c + ").y, 0.0), clamp((" + c + ").w, -127.9961, 127.9961)) : 0.0, 1.0)"; break;
            case 13: se = "vec4(log2(" + cx + "))"; break;
            case 14: se = "vec4(exp2(" + cx + "))"; break;
            case 15: se = "vec4(sin(" + cx + "))"; break;
            case 16: se = "vec4(cos(" + cx + "))"; break;
            default: LOG_W("rsx vp: unsupported scalar op %u", sca_op);
        }
        // condition codes: D0 bits 10-12 condition (1 lt, 2 eq, 4 gt), 13 test enable, 14 + 29 update enable,
        // 25 condition register select, 2-9 condition swizzle (w, z, y, x)
        u32 cond = (d0 >> 10) & 7;
        bool cond_test = ((d0 >> 13) & 1) && cond != 7;
        bool cond_update = ((d0 >> 14) & 1) && ((d0 >> 29) & 1);
        std::string ccreg = "CC[" + std::to_string((d0 >> 25) & 1) + "]";
        std::string ccv = ccreg + swizzle_str((d0 >> 8) & 3, (d0 >> 6) & 3, (d0 >> 4) & 3, (d0 >> 2) & 3);
        std::string cmask = "step(vec4(0.5), vec4(0.0)";
        if (cond & 1) cmask += " + vec4(lessThan(" + ccv + ", vec4(0.0)))";
        if (cond & 2) cmask += " + vec4(equal(" + ccv + ", vec4(0.0)))";
        if (cond & 4) cmask += " + vec4(greaterThan(" + ccv + ", vec4(0.0)))";
        cmask += ")";
        auto put = [&](const std::string& target, const std::string& mask, const std::string& val) {
            if (cond_test) return masked_assign(target, mask, "mix(" + target + ", " + val + ", m)", false) + "\n";
            return masked_assign(target, mask, val, false) + "\n";
        };
        bool skip = ((d0 >> 13) & 1) && cond == 0;   // condition "never"
        if (!ve.empty() && !skip) {
            body << "{ vec4 v = " << (sat ? "clamp(" + ve + ", 0.0, 1.0)" : ve) << ";\n";
            if (cond_test) body << " vec4 m = " << cmask << ";\n";
            if (vec_tmp != 0x3F && !vmask.empty()) body << put("R[" + std::to_string(vec_tmp) + "]", vmask, "v");
            if ((vec_result || sca_op == 0) && dst != 0x1F && !vmask.empty()) {   // no scalar op: the vector unit owns the output
                body << put("O[" + std::to_string(dst) + "]", vmask, "v");
                info.outputs_used |= 1u << dst;
            }
            if (cond_update && !vmask.empty()) body << put(ccreg, vmask, "v");
            body << "}\n";
        }
        if (!se.empty() && !skip) {
            body << "{ vec4 s = " << (sat ? "clamp(" + se + ", 0.0, 1.0)" : se) << ";\n";
            if (cond_test) body << " vec4 m = " << cmask << ";\n";
            if (sca_tmp != 0x3F && !smask.empty()) body << put("R[" + std::to_string(sca_tmp) + "]", smask, "s");
            if (!vec_result && sca_op != 0 && dst != 0x1F && !smask.empty()) {
                body << put("O[" + std::to_string(dst) + "]", smask, "s");
                info.outputs_used |= 1u << dst;
            }
            if (cond_update && ve.empty() && !smask.empty()) body << put(ccreg, smask, "s");
            body << "}\n";
        }
        if (d3 & 1) break;   // last instruction
    }
    std::ostringstream src;
    src << (g_shader_vk ? vk_header() : "#version 420 core\n");
    if (g_vtx_gpu) src << vp_fetch_glsl(body.str());
    else for (int i = 0; i < 16; i++) src << "layout(location = " << i << ") in vec4 in_attr" << i << ";\n";
    if (g_shader_vk)
        src << "layout(std140, set = 0, binding = 0) uniform VC { vec4 C[468]; };\n"
               "#define vtex0 g_tex[u_vslot[0]]\n#define vtex1 g_tex[u_vslot[1]]\n#define vtex2 g_tex[u_vslot[2]]\n#define vtex3 g_tex[u_vslot[3]]\n"
               "layout(location = 0) out vec4 v_col0; layout(location = 1) out vec4 v_col1; layout(location = 2) out vec4 v_fog;\n"
               "layout(location = 3) out vec4 v_tex[10];\n";
    else
        src << "uniform vec4 C[468];\nuniform sampler2D vtex0;\nuniform sampler2D vtex1;\nuniform sampler2D vtex2;\nuniform sampler2D vtex3;\n"
               "uniform vec4 u_vp_scale;\nuniform vec4 u_vp_offset;\nuniform vec4 u_surface;\n"
               "out vec4 v_col0; out vec4 v_col1; out vec4 v_fog; out vec4 v_tex[10];\n";
    src << "void main() {\n";
    {   // only the temporaries the program uses (zero-initialised arrays of 48/64 registers compiled very slowly)
        int nr = array_size(body.str(), "R", 1);
        src << " vec4 R[" << nr << "]; for (int i = 0; i < " << nr << "; i++) R[i] = vec4(0.0);\n";
    }
    if (g_vtx_gpu)
        for (int i = 0; i < 16; i++)
            if (vp_uses_input(body.str(), i))
                src << " in_attr" << i << " = va_fetch(" << i << ");\n";
    src << " vec4 O[32]; for (int i = 0; i < 32; i++) O[i] = vec4(0.0, 0.0, 0.0, 1.0);\n ivec4 A[2]; A[0] = ivec4(0); A[1] = ivec4(0);\n"
           " vec4 CC[2]; CC[0] = vec4(0.0); CC[1] = vec4(0.0);\n";
    src << body.str();
    // RSX: clip-space position -> (divide) -> viewport scale/offset -> window coords (top-left origin).
    // Convert back to GL clip space for the current surface size, keeping w for perspective interpolation.
    // (Vulkan: same x and y, so an image row holds what the same row holds under OpenGL; depth range 0..1)
    src << " vec4 p = O[0];\n"
           " vec3 win = (p.xyz / p.w) * u_vp_scale.xyz + u_vp_offset.xyz;\n"
           " float y_top = u_surface.z > 0.5 ? -win.y : win.y;   // SHADER_WINDOW origin bottom: y measured upward\n"
        << (g_shader_vk ? " vec3 ndc = vec3(win.x / u_surface.x * 2.0 - 1.0, 1.0 - y_top / u_surface.y * 2.0, win.z);\n gl_PointSize = 1.0;\n"
                        : " vec3 ndc = vec3(win.x / u_surface.x * 2.0 - 1.0, 1.0 - y_top / u_surface.y * 2.0, win.z * 2.0 - 1.0);\n")
        << " gl_Position = vec4(ndc * p.w, p.w);\n"
           " v_col0 = O[1]; v_col1 = O[2]; v_fog = O[5];\n"
           " for (int i = 0; i < 10; i++) v_tex[i] = O[7 + i];\n}\n";
    info.glsl = src.str();
    return info;
}

// ---------------- fragment program
// fragment programs are read from guest memory, or from a saved copy while the program database is replayed
const u8* g_fp_override = nullptr;   // big-endian program bytes standing for guest memory at g_fp_override_ea
u32 g_fp_override_ea = 0, g_fp_override_size = 0;
// The RSX half registers share the file of the full ones: H[2k] is stored in R[k].xy and H[2k+1] in R[k].zw, two
// halves per 32-bit word. The shader compiler packs both into the same storage when their lifetimes allow (the crowd
// shader writes H17.w right after its last read of R8.w) and moves packed halves with full-precision MOVs: with
// separate arrays the crowd's fallback lookup used stale coordinates and dressed every figure in the skin colour of
// its atlas cell. The file is kept as raw bits (uvec4 R[]): a packed pair whose high half is zero is a denormal
// float, which the GPU flushes to zero when it is copied as a float.
bool fp_h_alias() { static const bool v = getenv("F1CE_NO_H_ALIAS") == nullptr; return v; }   // debug: separate H/R

std::string fp_r_read(u32 n) {
    return fp_h_alias() ? "uintBitsToFloat(R[" + std::to_string(n) + "])" : "R[" + std::to_string(n) + "]";
}

std::string fp_h_read(u32 n) {
    if (!fp_h_alias()) return "H[" + std::to_string(n) + "]";
    return std::string(n & 1 ? "h_hi(R[" : "h_lo(R[") + std::to_string(n >> 1) + "])";
}

std::string fp_h_write(u32 n, const std::string& mask, const std::string& e) {
    if (!fp_h_alias()) return masked_assign("H[" + std::to_string(n) + "]", mask, e, false);
    return "{ vec4 hv = " + fp_h_read(n) + "; " + masked_assign("hv", mask, e, false) + " R[" + std::to_string(n >> 1) + "]." +
           (n & 1 ? "zw" : "xy") + " = h_pack(hv); }";
}

std::string fp_src(u32 s, u32 attr, bool abs_flag, int const_idx, bool& uses_const) {
    u32 type = s & 3, idx = (s >> 2) & 0x3F;
    bool half = (s >> 8) & 1;
    u32 sx = (s >> 9) & 3, sy = (s >> 11) & 3, sz = (s >> 13) & 3, sw = (s >> 15) & 3;
    bool neg = (s >> 17) & 1;
    std::string r;
    static const char* inputs[] = {"wpos", "v_col0", "v_col1", "fogc", "v_tex[0]", "v_tex[1]", "v_tex[2]",
                                   "v_tex[3]", "v_tex[4]", "v_tex[5]", "v_tex[6]", "v_tex[7]", "v_tex[8]", "v_tex[9]",
                                   "vec4(gl_FrontFacing ? 1.0 : -1.0)", "vec4(0.0)"};
    switch (type) {
        case 0: r = half ? fp_h_read(idx) : fp_r_read(idx); break;
        case 1: r = inputs[attr & 15]; break;
        case 2: r = "FC[" + std::to_string(const_idx) + "]"; uses_const = true; break;
        default: r = "vec4(0.0)";
    }
    r = "(" + r + ")" + swizzle_str(sx, sy, sz, sw);
    if (abs_flag) r = "abs(" + r + ")";
    if (neg) r = "-" + r;
    return r;
}

bool g_fp_stop_here = false;   // F1CE_FP_STOP applies to the program being translated (F1CE_FP_STOP_HASH)

u64 g_fp_hash_cur = 0;   // hash of the program being translated (debug options by hash)
FPInfo translate_fp(u32 ea, u32 shader_control, u32 cube_mask) {
    FPInfo info;
    std::ostringstream body;
    u32 off = 0;
    int nconst = 0;
    // IFE blocks: byte offsets (from the program start) where an else branch / the end of an if begins
    std::vector<u32> else_offs, end_offs;
    for (int guard = 0; guard < 2048; guard++) {
        for (auto it = std::find(end_offs.begin(), end_offs.end(), off); it != end_offs.end();
             it = std::find(end_offs.begin(), end_offs.end(), off)) {
            end_offs.erase(it);
            body << "}\n";
        }
        for (auto it = std::find(else_offs.begin(), else_offs.end(), off); it != else_offs.end();
             it = std::find(else_offs.begin(), else_offs.end(), off)) {
            else_offs.erase(it);
            body << "} else {\n";
        }
        u32 w0 = fp_word(ea + off), w1 = fp_word(ea + off + 4), w2 = fp_word(ea + off + 8), w3 = fp_word(ea + off + 12);
        bool end = w0 & 1;
        u32 dst = (w0 >> 1) & 0x3F;
        bool dst_half = (w0 >> 7) & 1;
        std::string mask = mask_str((w0 >> 9) & 1, (w0 >> 10) & 1, (w0 >> 11) & 1, (w0 >> 12) & 1);
        u32 attr = (w0 >> 13) & 0xF, tex = (w0 >> 17) & 0xF;
        u32 op = ((w0 >> 24) & 0x3F) | (((w2 >> 31) & 1) << 6);
        bool no_dest = (w0 >> 30) & 1, sat = (w0 >> 31) & 1;
        u32 scale = (w2 >> 28) & 7;
        bool uses_const = false, flow = op >= 0x40 && op <= 0x45;   // BRK CAL IFE LOOP REP RET: no sources
        std::string a, b, c;
        if (!flow) {
            a = fp_src(w1, attr, (w1 >> 29) & 1, nconst, uses_const);
            b = fp_src(w2, attr, (w2 >> 18) & 1, nconst, uses_const);
            c = fp_src(w3, attr, (w3 >> 18) & 1, nconst, uses_const);
        }
        off += 16;
        if (uses_const) {
            info.const_offsets.push_back(off);
            nconst++;
            off += 16;
        }
        // condition codes: writes execute per component where CC[cond_reg].swizzle compares (<, ==, >) 0 as enabled;
        // set_cond also stores the result into CC[cond_mod_reg]
        u32 cond = (w1 >> 18) & 7;
        bool cond_all = cond == 7, set_cond = (w0 >> 8) & 1;
        static const bool cc_swap = getenv("F1CE_FP_CCSWAP") != nullptr;   // debug: swap the tested / written CC bits
        std::string ccv = "CC[" + std::to_string((w1 >> (cc_swap ? 30 : 31)) & 1) + "]" +
                          swizzle_str((w1 >> 21) & 3, (w1 >> 23) & 3, (w1 >> 25) & 3, (w1 >> 27) & 3);
        std::string cond_mask = "(";
        if (cond & 1) cond_mask += "vec4(lessThan(" + ccv + ", vec4(0.0))) + ";
        if (cond & 2) cond_mask += "vec4(equal(" + ccv + ", vec4(0.0))) + ";
        if (cond & 4) cond_mask += "vec4(greaterThan(" + ccv + ", vec4(0.0))) + ";
        cond_mask += "vec4(0.0))";
        std::string e;
        // all lookups go through sample_unit(): v flip for surfaces, depth compare for zeta buffers
        auto tex2d = [&](const std::string& coord4, bool projective) {
            info.tex_used |= 1u << tex;
            std::string t = std::to_string(tex);
            if (cube_mask & (1u << tex)) return "remap(texture(tex" + t + ", (" + coord4 + ").xyz), " + t + ")";
            return "sample_unit(tex" + t + ", " + coord4 + ", " + t + ", " + (projective ? "true" : "false") + ")";
        };
        switch (op) {
            case 0x00: break;
            case 0x01: e = a; break;
            case 0x02: e = a + " * " + b; break;
            case 0x03: e = a + " + " + b; break;
            case 0x04: e = a + " * " + b + " + " + c; break;
            case 0x05: e = "vec4(dot((" + a + ").xyz, (" + b + ").xyz))"; break;
            case 0x06: e = "vec4(dot(" + a + ", " + b + "))"; break;
            case 0x07: e = "vec4(1.0, (" + a + ").y * (" + b + ").y, (" + a + ").z, (" + b + ").w)"; break;
            case 0x08: e = "min(" + a + ", " + b + ")"; break;
            case 0x09: e = "max(" + a + ", " + b + ")"; break;
            case 0x0A: e = "vec4(lessThan(" + a + ", " + b + "))"; break;
            case 0x0B: e = "vec4(greaterThanEqual(" + a + ", " + b + "))"; break;
            case 0x0C: e = "vec4(lessThanEqual(" + a + ", " + b + "))"; break;
            case 0x0D: e = "vec4(greaterThan(" + a + ", " + b + "))"; break;
            case 0x0E: e = "vec4(notEqual(" + a + ", " + b + "))"; break;
            case 0x0F: e = "vec4(equal(" + a + ", " + b + "))"; break;
            case 0x10: e = "fract(" + a + ")"; break;
            case 0x11: e = "floor(" + a + ")"; break;
            case 0x12:   // KIL: discard where the condition holds
                body << "if (" << (cond_all ? "true" : "any(greaterThan(" + cond_mask + ", vec4(0.5)))") << ") discard;\n";
                break;
            case 0x15: e = "dFdx(" + a + ")"; break;
            case 0x16: e = "dFdy(" + a + ")"; break;
            case 0x17: e = tex2d(a, false); break;
            case 0x18: e = tex2d(a, true); break;
            case 0x19: e = tex2d(a, false); break;
            case 0x1A: e = "vec4(1.0 / (" + a + ").x)"; break;
            case 0x1B: e = "vec4(inversesqrt(abs((" + a + ").x)))"; break;
            case 0x1C: e = "vec4(exp2((" + a + ").x))"; break;
            case 0x1D: e = "vec4(log2((" + a + ").x))"; break;
            case 0x1E: e = "vec4(1.0, max((" + a + ").x, 0.0), ((" + a + ").x > 0.0) ? pow(max((" + a + ").y, 0.0), (" + a + ").w) : 0.0, 1.0)"; break;
            case 0x1F: e = "mix(" + c + ", " + b + ", " + a + ")"; break;
            case 0x20: e = "vec4(1.0)"; break;
            case 0x21: e = "vec4(0.0)"; break;
            case 0x22: e = "vec4(cos((" + a + ").x))"; break;
            case 0x23: e = "vec4(sin((" + a + ").x))"; break;
            case 0x26: e = "vec4(pow((" + a + ").x, (" + b + ").x))"; break;
            case 0x2F: e = tex2d(a, false); break;
            case 0x31: e = tex2d(a, false); break;
            case 0x38: e = "vec4(dot((" + a + ").xy, (" + b + ").xy))"; break;
            case 0x39: e = "vec4(normalize((" + a + ").xyz), 1.0)"; break;
            case 0x3A: e = a + " / (" + b + ").x"; break;
            case 0x3B: e = a + " * inversesqrt(abs((" + b + ").x))"; break;
            case 0x3D: case 0x3E: break;   // FENCT / FENCB: ordering fences, no effect here
            case 0x42: {   // IFE: if (condition) { .. } else { .. }; word 2 = else, word 3 = endif (in 32-bit words)
                // the crowd shader picks each figure's clothes in such a branch: running both sides painted every
                // figure in the skin colour of its atlas cell
                u32 else_off = (w2 & 0x7FFFFFFF) << 2, end_off = (w3 & 0x7FFFFFFF) << 2;
                body << "if (" << (cond_all ? "true" : "any(greaterThan(" + cond_mask + ", vec4(0.5)))") << ") {\n";
                if (else_off != end_off) else_offs.push_back(else_off);
                end_offs.push_back(end_off);
                break;
            }
            default: LOG_W("rsx fp: unsupported opcode 0x%x", op);
        }
        {   // raw words (w0 w1 w2 w3, halves swapped) for decoding checks
            char raw[48];
            snprintf(raw, sizeof(raw), " [%08x %08x %08x %08x]", w0, w1, w2, w3);
            body << "// i" << guard << " op " << std::hex << op << std::dec << " cond " << cond << (set_cond ? " setcc" : "") << raw << "\n";
        }
        if (!e.empty() && !mask.empty() && cond != 0 && (!no_dest || set_cond)) {
            static const char* scales[] = {"", " * 2.0", " * 4.0", " * 8.0", "", " / 2.0", " / 4.0", " / 8.0"};
            std::string ex = "(" + e + ")" + scales[scale];
            if (sat) ex = "clamp(" + ex + ", 0.0, 1.0)";
            std::string reg = dst_half ? fp_h_read(dst) : fp_r_read(dst);
            std::string rbits = "R[" + std::to_string(dst) + "]";
            // a plain MOV between full registers copies bits (packed halves included)
            bool bit_move = fp_h_alias() && op == 0x01 && !dst_half && (w1 & 3) == 0 && !((w1 >> 8) & 1) && !sat &&
                            scale == 0 && !((w1 >> 17) & 1) && !((w1 >> 29) & 1);
            std::string move_src = "R[" + std::to_string((w1 >> 2) & 0x3F) + "]" +
                                   swizzle_str((w1 >> 9) & 3, (w1 >> 11) & 3, (w1 >> 13) & 3, (w1 >> 15) & 3);
            auto write = [&](const std::string& v) {
                if (dst_half) return fp_h_write(dst, mask, v);
                if (!fp_h_alias()) return masked_assign(reg, mask, v, false);
                return masked_assign(rbits, mask, "floatBitsToUint(" + v + ")", false);
            };
            std::string cc = "CC[" + std::to_string((w1 >> (cc_swap ? 31 : 30)) & 1) + "]";
            if (cond_all) {
                if (!no_dest) body << (bit_move ? masked_assign(rbits, mask, move_src, false) : write(ex)) << "\n";
                static const bool no_setcc = getenv("F1CE_FP_NOSETCC") != nullptr;   // debug experiment
                if (set_cond && !no_setcc) body << masked_assign(cc, mask, ex, false) << "\n";
            } else {
                body << "{ vec4 t = " << ex << "; vec4 m = step(vec4(0.5), " << cond_mask << ");\n";
                if (!no_dest && bit_move) body << masked_assign(rbits, mask, "r_sel(" + rbits + ", " + move_src + ", m)", false) << "\n";
                else if (!no_dest && !dst_half && fp_h_alias())
                    body << masked_assign(rbits, mask, "r_sel(" + rbits + ", floatBitsToUint(t), m)", false) << "\n";
                else if (!no_dest) body << write("mix(" + reg + ", t, m)") << "\n";
                if (set_cond) body << masked_assign(cc, mask, "mix(" + cc + ", t, m)", false) << "\n";
                body << "}\n";
            }
        }
        // debug: F1CE_FP_STOP=<n>:<R|H><m> outputs register m right after instruction n (every program that long)
        static const char* fp_stop = getenv("F1CE_FP_STOP");
        if (fp_stop && g_fp_stop_here && guard == atoi(fp_stop)) {
            const char* r = strchr(fp_stop, ':');
            if (r && r[1] == 'v')   // interpolated input v_tex[m]
                body << "{ frag_color = vec4(abs(v_tex[" << atoi(r + 2) << "].xyz), 1.0); return; }\n";
            else if (r && r[1] == 'f')   // fog: red = factor (fogc.y), green = fog input / 1000
                body << "{ frag_color = vec4(clamp(fogc.y, 0.0, 1.0), clamp(abs(fogc.x) / 1000.0, 0.0, 1.0), 0.0, 1.0); return; }\n";
            else if (r && r[1] == 'c')
                body << "{ frag_color = vec4(abs(v_col0.xyz), 1.0); return; }\n";
            else if (r && r[1] == 'C')   // condition register: > 0 / == 0 / < 0 per component y,z,w -> 1.0 / 0.5 / 0.0
                body << "{ vec4 x = CC[" << atoi(r + 2) << "]; frag_color = vec4(step(0.0, x.yzw) * 0.5 + step(0.0, -x.yzw) * -0.5 + 0.5, 1.0); return; }\n";
            else if (r && (r[1] == 'R' || r[1] == 'H') && getenv("F1CE_FP_STOP_NAN"))   // red = NaN, green = Inf
                body << "{ vec3 x = " << (r[1] == 'H' ? fp_h_read(atoi(r + 2)) : fp_r_read(atoi(r + 2))) << ".xyz; frag_color = vec4(any(isnan(x)) ? 1.0 : 0.0, "
                     << "any(isinf(x)) ? 1.0 : 0.0, 0.0, 1.0); return; }\n";
            else if (r && (r[1] == 'R' || r[1] == 'H'))
                body << "{ frag_color = vec4(abs(" << (r[1] == 'H' ? fp_h_read(atoi(r + 2)) : fp_r_read(atoi(r + 2))) << "."
                     << (getenv("F1CE_FP_STOP_SWZ") ? getenv("F1CE_FP_STOP_SWZ") : "xyz") << ") * "   // e.g. wwx
                     << (getenv("F1CE_FP_STOP_SCALE") ? getenv("F1CE_FP_STOP_SCALE") : "1.0") << ", 1.0); return; }\n";
        }
        if (end) break;
    }
    for (size_t i = 0; i < end_offs.size(); i++) body << "}\n";   // blocks that end with the program
    info.size = off;
    std::ostringstream src;
    src << (g_shader_vk ? vk_header() : "#version 420 core\n");
    // debug: F1CE_SHADER_SALT=<text> makes every program new to the disk cache (cold compile tests)
    if (const char* salt = getenv("F1CE_SHADER_SALT")) src << "// " << salt << "\n";
    if (g_shader_vk) {
        src << "layout(location = 0) in vec4 v_col0; layout(location = 1) in vec4 v_col1; layout(location = 2) in vec4 v_fog;\n"
               "layout(location = 3) in vec4 v_tex[10];\n";
        for (int i = 0; i < 16; i++) src << "#define tex" << i << ((cube_mask >> i) & 1 ? " g_cube[u_slot[" : " g_tex[u_slot[") << i << "]]\n";
    } else {
        src << "in vec4 v_col0; in vec4 v_col1; in vec4 v_fog; in vec4 v_tex[10];\n";
        for (int i = 0; i < 16; i++) src << ((cube_mask >> i) & 1 ? "uniform samplerCube tex" : "uniform sampler2D tex") << i << ";\n";
    }
    // u_tex_scale: 1/size for textures with unnormalized (texel) coordinates (format bit 0x40), else 1
    if (scaled_glsl() && !g_shader_vk) src << "uniform float u_ui_clamp;\n";
    if (!g_shader_vk) src << "uniform float u_tex_flip[16];\nuniform vec2 u_tex_scale[16];\n";
    src << "vec2 tex_coord(vec2 c, int u) { c *= u_tex_scale[u]; return u_tex_flip[u] > 0.5 ? vec2(c.x, 1.0 - c.y) : c; }\n";
    if (!g_shader_vk) src << "uniform float u_tex_depth[16];\n";
    src
           // TEXTURE_CONTROL1 remap: bits 2i..2i+1 = source (0 A, 1 R, 2 G, 3 B) of output i (0 A, 1 R, 2 G, 3 B),
           // bits 8+2i.. = 0 zero, 1 one, 2 the source; 0xaae4 is the identity
           // bits 16-19: TEXTURE_ADDRESS gamma (sRGB decode of R, G, B, A), bit 20: unsigned remap BIASED (x * 2 - 1),
           // applied to the fetched texel before the remap
           << (g_shader_vk ? "" : "uniform float u_remap[16];\n") <<
           "vec4 remap(vec4 v, int u) {\n"
           " int r = int(u_remap[u]), c = r >> 16; r &= 0xFFFF;\n"
           " if (c != 0) {\n"
           "  vec4 m = vec4(ivec4(c, c >> 1, c >> 2, c >> 3) & 1);\n"
           "  v = mix(v, v * (v * (v * 0.305306011 + 0.682171111) + 0.012522878), m);\n"
           "  if ((c & 16) != 0) v = v * 2.0 - 1.0;\n"
           " }\n"
           " if (r == 0xaae4) return v;\n"
           " vec4 s = vec4(v.a, v.r, v.g, v.b); vec4 o;\n"
           " for (int i = 0; i < 4; i++) { int op = (r >> (8 + 2 * i)) & 3; o[i] = op == 0 ? 0.0 : op == 1 ? 1.0 : s[(r >> (2 * i)) & 3]; }\n"
           " return vec4(o.y, o.z, o.w, o.x);\n"
           "}\n"
        // (Vulkan: the sampler comes from the descriptor array inside, an array element is no function argument)
        << (g_shader_vk ? "#define t g_tex[u_slot[u]]\nvec4 sample_unit_(vec4 c, int u, bool proj) {\n"
                        : "vec4 sample_unit(sampler2D t, vec4 c, int u, bool proj) {\n")
        << " vec3 p = proj ? c.xyz / c.w : c.xyz;\n";
    // resolution scaling: draws without depth test (2D interface, full-screen passes) keep coordinates inside [0, 1]
    // half a texel from the edges. Their quads map textures 1:1 onto pixels: at native size every sample hits a
    // texel centre, at a higher scale the edge samples blended in the opposite edge of repeating textures (a
    // coloured line down the Quick Race menu at the portrait's edge). Coordinates outside [0, 1] repeat as before.
    if (scaled_glsl())
        src << " vec2 tc = tex_coord(p.xy, u);\n"
               " if (u_ui_clamp > 0.5 && all(greaterThanEqual(tc, vec2(0.0))) && all(lessThanEqual(tc, vec2(1.0)))) {\n"
               "  vec2 hh = 0.5 / vec2(textureSize(t, 0)); tc = clamp(tc, hh, 1.0 - hh);\n"
               " }\n"
               " vec4 v = texture(t, tc);\n";
    else
        src << " vec4 v = texture(t, tex_coord(p.xy, u));\n";
    src << " int f = int(u_tex_depth[u]);\n"
           " if (f == 0) return remap(v, u);\n"
           " if (f == 9) {   // D24S8 zeta read with a colour format: A = d[23:16], R = d[15:8], G = d[7:0], B = stencil\n"
           "  uint z = uint(v.r * 16777215.0 + 0.5);\n"
           "  return remap(vec4(float((z >> 8) & 255u), float(z & 255u), 0.0, float(z >> 16)) / 255.0, u);\n"
           " }\n"
           " if (f == 12) return remap(vec4(v.b), u);   // B8 render target (stores blue) read as a B8 texture\n"
           " float d = v.r, r = clamp(p.z, 0.0, 1.0); bool pass;\n"
           " if (f == 10) return vec4(d); if (f == 11) return vec4(p.z);   // debug (F1CE_ZFUNC): stored depth / reference\n"
           // linear filtering of a depth compare (f + 16) is percentage-closer filtering on the RSX: the four texels
           // are compared, then the results are weighted bilinearly. Comparing the filtered depth spread wrong
           // shadows along depth edges (dark smears around the wheels and suspension).
           " if (f >= 16) {\n"
           "  f -= 16;\n"
           "  vec2 sz = vec2(textureSize(t, 0)), q = tex_coord(p.xy, u) * sz - 0.5, w = fract(q);\n"
           "  vec4 g = textureGather(t, (floor(q) + 1.0) / sz, 0);   // (x0,y1) (x1,y1) (x1,y0) (x0,y0)\n"
           "  vec4 c;\n"
           "  for (int i = 0; i < 4; i++) {\n"
           "   float e = g[i]; bool ps;\n"
           "   if (f == 1) ps = false; else if (f == 2) ps = e < r; else if (f == 3) ps = e == r;\n"
           "   else if (f == 4) ps = e <= r; else if (f == 5) ps = e > r; else if (f == 6) ps = e != r;\n"
           "   else if (f == 7) ps = e >= r; else ps = true;\n"
           "   c[i] = ps ? 1.0 : 0.0;\n"
           "  }\n"
           "  return vec4(mix(mix(c.w, c.z, w.x), mix(c.x, c.y, w.x), w.y));\n"
           " }\n"
           // RSX texture depth compare is "stored FUNC reference" (the reverse of GL's ref-to-texture order); the GL
           // order put the player car in its own shadow and darkened the whole race scene
           " if (f == 1) pass = false; else if (f == 2) pass = d < r; else if (f == 3) pass = d == r;\n"
           " else if (f == 4) pass = d <= r; else if (f == 5) pass = d > r; else if (f == 6) pass = d != r;\n"
           " else if (f == 7) pass = d >= r; else pass = true;\n"
           " return vec4(pass ? 1.0 : 0.0);\n"
           "}\n";
    if (g_shader_vk) src << "#undef t\n#define sample_unit(T, c, u, proj) sample_unit_(c, u, proj)\n";
    // Rain drops on the screen ("PostEffects: Rain Drops"): this program moves the image by 0.04 x the slope of a
    // 512x288 B8 water map that the SpuRainDroplet task writes as trails one texel wide. Read as they are, the steep
    // trail edges move the image by tens of pixels in hard-edged patches (posterised look). F1CE_RAIN_SMOOTH (default
    // on, =0 off) reads the map through a 3x3 tent filter (four bilinear taps half a texel apart), which turns the
    // trails into smooth streaks. Not checked against the console: there is no rain reference.
    static const bool rain_smooth = !getenv("F1CE_RAIN_SMOOTH") || atoi(getenv("F1CE_RAIN_SMOOTH")) != 0;
    const bool smooth_map = rain_smooth && g_fp_hash_cur == 0x8213ee750eb96053ull;
    if (smooth_map)
        src << "vec4 sample_smooth0(vec4 c, int u, bool proj) {\n"
               " vec2 h = 0.5 / vec2(textureSize(tex0, 0));\n"
               " return 0.25 * (sample_unit(tex0, c + vec4(h.x, h.y, 0.0, 0.0), u, proj) + sample_unit(tex0, c + vec4(-h.x, h.y, 0.0, 0.0), u, proj) +\n"
               "                sample_unit(tex0, c + vec4(h.x, -h.y, 0.0, 0.0), u, proj) + sample_unit(tex0, c + vec4(-h.x, -h.y, 0.0, 0.0), u, proj));\n"
               "}\n";
    // FOGC input: x = the VP fog output, y = fog factor from FOG_MODE / FOG_PARAMS (u_fog = mode, p0, p1)
    src << "vec4 h_lo(uvec4 r) { return vec4(unpackHalf2x16(r.x), unpackHalf2x16(r.y)); }\n"
           "vec4 h_hi(uvec4 r) { return vec4(unpackHalf2x16(r.z), unpackHalf2x16(r.w)); }\n"
           "uvec2 h_pack(vec4 v) { return uvec2(packHalf2x16(v.xy), packHalf2x16(v.zw)); }\n"
           "uvec4 r_sel(uvec4 a, uvec4 b, vec4 m) {\n"
           " return uvec4(m.x > 0.5 ? b.x : a.x, m.y > 0.5 ? b.y : a.y, m.z > 0.5 ? b.z : a.z, m.w > 0.5 ? b.w : a.w);\n"
           "}\n";
    if (!g_shader_vk) src << "uniform vec4 u_fog;\n";
    src << "vec4 fog_value() {\n"
           " float x = v_fog.x, p0 = u_fog.y, p1 = u_fog.z, y; int m = int(u_fog.x);\n"
           " if (m >= 3) x = abs(x);   // *_ABS modes\n"
           " if (m == 0 || m == 3) y = p1 * x + (p0 - 1.0);\n"
           " else if (m == 1 || m == 4) y = exp(11.084 * (p1 * x + p0 - 1.5));\n"
           " else y = exp(-pow(4.709 * (p1 * x + p0 - 1.5), 2.0));\n"
           " return vec4(v_fog.x, clamp(y, 0.0, 1.0), 0.0, 0.0);\n"
           "}\n";
    if (g_shader_vk)
        src << "layout(std140, set = 0, binding = 2) uniform FCB { vec4 FC[" << std::max(nconst, 1) << "]; };\n"
               "const vec2 u_prog_id = vec2(0.0);\nlayout(location = 0) out vec4 frag_color;\n";
    else {
        if (scaled_glsl()) src << "uniform vec2 u_res_scale;\n";
        src << "uniform vec4 FC[" << std::max(nconst, 1) << "];\nuniform vec4 u_alpha_test;\nuniform vec2 u_prog_id;\nout vec4 frag_color;\n";
    }
    src << "void main() {\n vec4 CC[2]; CC[0] = vec4(0.0); CC[1] = vec4(0.0);\n vec4 fogc = fog_value();\n";
    // WPOS.w is the clip-space w on the RSX; gl_FragCoord.w is 1/w. The crowd impostors multiply a texture coordinate
    // by it to recover texel coordinates in their figure table: with 1/w they read texels near the origin (empty)
    static const bool wpos_raw_all = getenv("F1CE_WPOS_CLIP_W") == nullptr;   // debug experiment: WPOS.w = clip w
    // F1CE_WPOS_CLIP_W_FP=<fp hash>,...: the experiment for those programs only (bisecting)
    static const std::string wpos_fps = getenv("F1CE_WPOS_CLIP_W_FP") ? "," + std::string(getenv("F1CE_WPOS_CLIP_W_FP")) + "," : "";
    char fhk[24];
    snprintf(fhk, sizeof(fhk), ",%016llx,", (unsigned long long)g_fp_hash_cur);
    bool listed = wpos_fps.find(fhk) != std::string::npos, except = wpos_fps.size() > 1 && wpos_fps[1] == '-';   // "-<hash>,..": all but
    bool wpos_raw = wpos_raw_all && (wpos_fps.empty() || (except ? listed : !listed));
    if (scaled_glsl())
        src << " vec4 wpos = vec4(gl_FragCoord.xy / u_res_scale, gl_FragCoord.z, " << (wpos_raw ? "gl_FragCoord.w" : "1.0 / gl_FragCoord.w") << ");\n";
    else
        src << (wpos_raw ? " vec4 wpos = gl_FragCoord;\n" : " vec4 wpos = vec4(gl_FragCoord.xyz, 1.0 / gl_FragCoord.w);\n");
    {   // only the registers the program uses (R1 for depth export, H0/R0 for the output)
        int nr = array_size(body.str(), "R", 2), nh = array_size(body.str(), "H", 1);
        src << (fp_h_alias() ? " uvec4 R[" : " vec4 R[") << nr << "]; for (int i = 0; i < " << nr << "; i++) R[i] = "
            << (fp_h_alias() ? "uvec4(0u)" : "vec4(0.0)") << ";\n"
            << " vec4 H[" << nh << "]; for (int i = 0; i < " << nh << "; i++) H[i] = vec4(0.0);\n";
    }
    if (smooth_map) {   // the water map's lookups (unit 0) go through the tent filter above
        std::string b = body.str();
        for (size_t at; (at = b.find("sample_unit(tex0, ")) != std::string::npos;) b.replace(at, 18, "sample_smooth0(");
        body.str(b);
        body.seekp(0, std::ios::end);
    }
    src << body.str();
    static const bool force_r0 = getenv("F1CE_FP_FORCE_R0") != nullptr;   // debug experiment
    src << " frag_color = " << ((shader_control & 0x40) || force_r0 ? fp_r_read(0) : fp_h_read(0)) << ";\n";
    if (shader_control & 0xE) src << " gl_FragDepth = " << fp_r_read(1) << ".z;\n";   // depth export: fragment depth from R1.z
    // debug: F1CE_FP_DEBUG=1 shows the raw unit-0 texture for shaders that sample it (bypasses lighting)
    //        F1CE_FP_DEBUG=<n> (n = 0..9) shows the interpolated input v_tex[n].xyz instead
    static const char* fp_debug = getenv("F1CE_FP_DEBUG");
    if (fp_debug && (info.tex_used & 1)) {
        int n = atoi(fp_debug);
        if (*fp_debug == 't') src << " frag_color = vec4(texture(tex0, tex_coord(v_tex[0].xy, 0)).rgb, frag_color.a);\n";
        else if (*fp_debug == 'R' || *fp_debug == 'H')   // F1CE_FP_DEBUG=R3 / H1: show a register's final value
            src << " frag_color = vec4(abs(" << (*fp_debug == 'H' ? fp_h_read(std::clamp(atoi(fp_debug + 1), 0, 63)) : fp_r_read(std::clamp(atoi(fp_debug + 1), 0, 63))) << ".xyz), frag_color.a);\n";
        else src << " frag_color = vec4(abs(v_tex[" << std::clamp(n, 0, 9) << "].xyz), frag_color.a);\n";
    }
    // debug: F1CE_PROG_ID=1 paints every program in a colour encoding its id (R = id & 255, G = id >> 8)
    if (getenv("F1CE_PROG_ID")) src << " frag_color = vec4(u_prog_id.xy, 0.5, 1.0);\n";
    // alpha test: x = enabled, y = func (GL enum - 0x200), z = ref
    src << " if (u_alpha_test.x > 0.5) {\n"
           "  int f = int(u_alpha_test.y); float a = frag_color.a, r = u_alpha_test.z; bool pass = true;\n"
           "  if (f == 0) pass = false; else if (f == 1) pass = a < r; else if (f == 2) pass = a == r;\n"
           "  else if (f == 3) pass = a <= r; else if (f == 4) pass = a > r; else if (f == 5) pass = a != r;\n"
           "  else if (f == 6) pass = a >= r;\n"
           "  if (!pass) discard;\n }\n"
           // SHADER_PACKER (0x1FD8) bit 0: sRGB output (the game lights in linear space: textures are decoded on fetch
           // by their TEXTURE_ADDRESS gamma bits and the result is encoded when written)
           " if (u_alpha_test.w > 0.5) {\n"
           "  vec3 c = clamp(frag_color.rgb, 0.0, 1.0);\n"
           "  frag_color.rgb = mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(0.0031308, c));\n"
           " }\n}\n";
    info.glsl = src.str();
    return info;
}

// Linked programs are cached on disk (shader_cache/ next to the executable, keyed by the GLSL sources and the GL
// driver): compiling the few hundred programs of a race scene took seconds on the first frames, during which no
// frame was presented (white screen after loading).
std::string shader_cache_dir() {
    static std::string dir = [] {
        std::string d = "shader_cache";
#ifdef _WIN32
        char path[MAX_PATH];
        DWORD n = GetModuleFileNameA(nullptr, path, MAX_PATH);
        if (n && n < MAX_PATH) {
            std::string s(path, n);
            size_t slash = s.find_last_of("\\/");
            if (slash != std::string::npos) d = s.substr(0, slash + 1) + "shader_cache";
        }
#else
        std::error_code ec;
        auto executable = std::filesystem::read_symlink("/proc/self/exe", ec);
        if (!ec) d = (executable.parent_path() / "shader_cache").string();
#endif
        std::error_code create_error;
        std::filesystem::create_directories(d, create_error);
        if (create_error) LOG_E("shader cache directory '%s': %s", d.c_str(), create_error.message().c_str());
        return d;
    }();
    return dir;
}

u64 hash_words(const u32* p, size_t n, u64 h) {
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

// hashes the FP instructions only (inline constants excluded so constant changes reuse the shader)
u64 hash_fp(u32 ea, u32& size_out) {
    u64 h = 1469598103934665603ull;
    u32 off = 0;
    for (int guard = 0; guard < 2048; guard++) {
        u32 w[4];
        for (int i = 0; i < 4; i++) w[i] = fp_word(ea + off + i * 4);
        h = hash_words(w, 4, h);
        off += 16;
        bool uses_const = (w[1] & 3) == 2 || (w[2] & 3) == 2 || (w[3] & 3) == 2;
        if (uses_const) off += 16;
        if (w[0] & 1) break;
    }
    size_out = off;
    return h;
}

// ------------------------------------------------------------------ program database
// Program database (shader_cache/programs.bin): the guest microcode of every program pair the game has used (vertex
// program slots, fragment program bytes, shader control, cube mask). At start-up, before the renderer runs its first
// command, every entry is translated again with the current translator and its GL program is loaded from the binary
// cache or compiled, with a progress bar in the window: programs are never compiled or loaded during play any more
// (stutter, white screens while a new screen's programs compiled), also after a translator change or a new scale.
// Programs the game has never used before are still compiled once when they first appear, and recorded.
std::set<u64> g_db_keys;   // program pairs in the database (vh * 31 + fh)
bool g_db_loaded = false;

void db_record(u32 vp_start, u32 vp_len, u32 fp_ea, u32 fp_size, u32 shader_control, u32 cube_mask, u64 vh, u64 fh) {
    if (!g_db_loaded || !g_db_keys.insert(vh * 31 + fh).second || fp_size == 0 || fp_size > 65536) return;
    FILE* f = fopen((shader_cache_dir() + "/programs.bin").c_str(), "ab");
    if (!f) return;
    ProgRecord r{PROG_MAGIC, vp_start, vp_len, fp_ea, fp_size, shader_control, cube_mask, 0, vh, fh};
    fwrite(&r, sizeof(r), 1, f);
    fwrite(&vp_words[vp_start * 4], 4, (size_t)vp_len * 4, f);
    std::vector<u8> fp(fp_size);
    for (u32 o = 0; o < fp_size; o += 4) { u32 v = __builtin_bswap32(ld32(fp_ea + o)); memcpy(&fp[o], &v, 4); }   // as in memory
    fwrite(fp.data(), 1, fp_size, f);
    fclose(f);
}

std::vector<ProgEntry> load_program_db() {
    g_db_loaded = true;
    std::vector<ProgEntry> entries;
    FILE* f = fopen((shader_cache_dir() + "/programs.bin").c_str(), "rb");
    if (!f) { LOG_I("rsx: no program database yet (shader_cache/programs.bin)"); return entries; }
    for (;;) {
        ProgEntry e;
        if (fread(&e.r, sizeof(e.r), 1, f) != 1 || e.r.magic != PROG_MAGIC || e.r.vp_start + e.r.vp_len > 512 || e.r.fp_size > 65536) break;
        e.vp.resize((size_t)e.r.vp_len * 4);
        e.fp.resize(e.r.fp_size);
        if (fread(e.vp.data(), 4, e.vp.size(), f) != e.vp.size() || fread(e.fp.data(), 1, e.fp.size(), f) != e.fp.size()) break;
        if (g_db_keys.insert(e.r.vh * 31 + e.r.fh).second) entries.push_back(std::move(e));
    }
    fclose(f);
    return entries;
}

// ------------------------------------------------------------------ hashes and small helpers
// change-detection hash of guest memory: 64-byte stripes in two AVX2 accumulators (data times data, as xxh3, with a key
// that moves on per stripe so that swapped stripes differ), then four independent multiply chains for what is left;
// step > 8 samples one 8-byte word per step
u64 hash_range(const u8* p, u32 bytes, u32 step) {
    u64 h0 = 1469598103934665603ull, h1 = 0x9E3779B97F4A7C15ull, h2 = 0xC2B2AE3D27D4EB4Full, h3 = 0x165667B19E3779F9ull;
    constexpr u64 M = 1099511628211ull;
    u32 k = 0;
    if (step == 8) {
        if (bytes >= 64) {
            __m256i a0 = _mm256_set_epi64x(h0, h1, h2, h3), a1 = _mm256_set_epi64x(h3, h0, h1, h2);
            __m256i k0 = _mm256_set_epi64x(0x243F6A8885A308D3ll, 0x13198A2E03707344ll, (long long)0xA4093822299F31D0ull, 0x082EFA98EC4E6C89ll);
            __m256i k1 = _mm256_set_epi64x(0x452821E638D01377ll, (long long)0xBE5466CF34E90C6Cll, (long long)0xC0AC29B7C97C50DDull, 0x3F84D5B5B5470917ll);
            const __m256i inc = _mm256_set1_epi64x((long long)0x9E3779B97F4A7C15ull);
            for (; k + 64 <= bytes; k += 64) {
                const __m256i d0 = _mm256_loadu_si256((const __m256i*)(p + k)), d1 = _mm256_loadu_si256((const __m256i*)(p + k + 32));
                const __m256i x0 = _mm256_xor_si256(d0, k0), x1 = _mm256_xor_si256(d1, k1);
                a0 = _mm256_add_epi64(a0, _mm256_add_epi64(_mm256_mul_epu32(x0, _mm256_srli_epi64(x0, 32)), _mm256_shuffle_epi32(d0, 0x4E)));
                a1 = _mm256_add_epi64(a1, _mm256_add_epi64(_mm256_mul_epu32(x1, _mm256_srli_epi64(x1, 32)), _mm256_shuffle_epi32(d1, 0x4E)));
                k0 = _mm256_add_epi64(k0, inc);
                k1 = _mm256_add_epi64(k1, inc);
            }
            alignas(32) u64 w[8];
            _mm256_store_si256((__m256i*)w, a0);
            _mm256_store_si256((__m256i*)(w + 4), a1);
            h0 = (h0 ^ w[0]) * M; h1 = (h1 ^ w[1]) * M; h2 = (h2 ^ w[2]) * M; h3 = (h3 ^ w[3]) * M;
            h0 = (h0 ^ w[4]) * M; h1 = (h1 ^ w[5]) * M; h2 = (h2 ^ w[6]) * M; h3 = (h3 ^ w[7]) * M;
        }
        for (; k + 32 <= bytes; k += 32) {
            u64 x0, x1, x2, x3;
            memcpy(&x0, p + k, 8); memcpy(&x1, p + k + 8, 8); memcpy(&x2, p + k + 16, 8); memcpy(&x3, p + k + 24, 8);
            h0 = (h0 ^ x0) * M; h1 = (h1 ^ x1) * M; h2 = (h2 ^ x2) * M; h3 = (h3 ^ x3) * M;
        }
        for (; k + 8 <= bytes; k += 8) { u64 x; memcpy(&x, p + k, 8); h0 = (h0 ^ x) * M; }
        for (; k < bytes; k++) h1 = (h1 ^ p[k]) * M;
    } else {
        for (; k + 8 <= bytes; k += step) { u64 x; memcpy(&x, p + k, 8); h0 = (h0 ^ x) * M; }
    }
    return h0 ^ (h1 * 3) ^ (h2 * 5) ^ (h3 * 7) ^ bytes;
}

// Eight indices per step (the scalar loop was ~6 ns per index, ~3 ms per frame while driving). A restart index turns into
// all ones, which never lowers the min; for the max it is masked to 0.
static inline void minmax_reduce(__m256i vmin, __m256i vmax, u32& lo, u32& hi) {
    alignas(32) u32 a[8], b[8];
    _mm256_store_si256((__m256i*)a, vmin);
    _mm256_store_si256((__m256i*)b, vmax);
    for (int i = 0; i < 8; i++) {
        if (a[i] != ~0u) lo = std::min(lo, a[i]);
        if (a[i] != ~0u || b[i]) hi = std::max(hi, b[i]);
    }
}
void convert_indices(const u8* src, u32 n, bool u16idx, bool restart, u32 rv, u32* out, u32& lo, u32& hi) {
    const u32 cmp = u16idx && rv >= 0xFFFF ? 0xFFFF : rv;
    const __m256i vc = _mm256_set1_epi32(restart ? (int)cmp : -1), ones = _mm256_set1_epi32(-1);
    // (without restart, a raw 0xFFFFFFFF index compares equal to the all-ones sentinel and stays all ones: same value)
    __m256i vmin = ones, vmax = _mm256_setzero_si256();
    u32 k = 0;
    if (u16idx) {
        const __m128i sw = _mm_setr_epi8(1, 0, 3, 2, 5, 4, 7, 6, 9, 8, 11, 10, 13, 12, 15, 14);
        for (; k + 8 <= n; k += 8) {
            __m256i v = _mm256_cvtepu16_epi32(_mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)(src + k * 2)), sw));
            __m256i m = _mm256_cmpeq_epi32(v, vc);
            v = _mm256_or_si256(v, m);
            _mm256_storeu_si256((__m256i*)(out + k), v);
            vmin = _mm256_min_epu32(vmin, v);
            vmax = _mm256_max_epu32(vmax, _mm256_andnot_si256(m, v));
        }
    } else {
        const __m256i sw = _mm256_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12,
                                             3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12);
        for (; k + 8 <= n; k += 8) {
            __m256i v = _mm256_shuffle_epi8(_mm256_loadu_si256((const __m256i*)(src + k * 4)), sw);
            __m256i m = _mm256_cmpeq_epi32(v, vc);
            v = _mm256_or_si256(v, m);
            _mm256_storeu_si256((__m256i*)(out + k), v);
            vmin = _mm256_min_epu32(vmin, v);
            vmax = _mm256_max_epu32(vmax, _mm256_andnot_si256(m, v));
        }
    }
    minmax_reduce(vmin, vmax, lo, hi);
    for (; k < n; k++) {
        u32 v;
        if (u16idx) { u16 x; memcpy(&x, src + k * 2, 2); v = __builtin_bswap16(x); }
        else { u32 x; memcpy(&x, src + k * 4, 4); v = __builtin_bswap32(x); }
        if (restart && v == cmp) { out[k] = ~0u; continue; }
        out[k] = v;
        lo = std::min(lo, v);
        hi = std::max(hi, v);
    }
}
void index_minmax(const u32* p, u32 n, u32& lo, u32& hi) {
    const __m256i ones = _mm256_set1_epi32(-1);
    __m256i vmin = ones, vmax = _mm256_setzero_si256();
    u32 k = 0;
    for (; k + 8 <= n; k += 8) {
        __m256i v = _mm256_loadu_si256((const __m256i*)(p + k));
        vmin = _mm256_min_epu32(vmin, v);
        vmax = _mm256_max_epu32(vmax, _mm256_andnot_si256(_mm256_cmpeq_epi32(v, ones), v));
    }
    minmax_reduce(vmin, vmax, lo, hi);
    for (; k < n; k++)
        if (p[k] != ~0u) { lo = std::min(lo, p[k]); hi = std::max(hi, p[k]); }
}

// Block heaps: raw vertex data (vertex pulling) and converted index lists that stay for several frames. Blocks are
// never rewritten: changed data gets a new block and the old one is reused only after the GPU finished the frame
// that freed it.
void heap_insert_free(BlockHeap& h, u32 off, u32 size) {   // with coalescing of neighbours
    auto next = h.free_off.lower_bound(off);
    if (next != h.free_off.end() && next->first == off + size) {
        size += next->second;
        h.free_size.erase({next->second, next->first});
        next = h.free_off.erase(next);
    }
    if (next != h.free_off.begin()) {
        auto prev = std::prev(next);
        if (prev->first + prev->second == off) {
            h.free_size.erase({prev->second, prev->first});
            off = prev->first;
            size += prev->second;
            h.free_off.erase(prev);
        }
    }
    h.free_off[off] = size;
    h.free_size.emplace(size, off);
}

// a block of `bytes` in the heap (offset), ~0u when full
u32 heap_alloc(BlockHeap& h, u32 bytes) {
    bytes = (bytes + BlockHeap::ALIGN - 1) & ~(BlockHeap::ALIGN - 1);
    auto it = h.free_size.lower_bound({bytes, 0u});
    if (it == h.free_size.end()) { h.failed++; return ~0u; }
    u32 off = it->second, size = it->first;
    h.free_size.erase(it);
    h.free_off.erase(off);
    if (size > bytes) heap_insert_free(h, off + bytes, size - bytes);
    h.used += bytes;
    return off;
}

void heap_free(BlockHeap& h, u32 off, u32 bytes) {
    if (off == ~0u) return;
    bytes = (bytes + BlockHeap::ALIGN - 1) & ~(BlockHeap::ALIGN - 1);
    h.pending.push_back({g_frame, off, bytes});
    h.used -= bytes;
}

u32 morton(u32 x, u32 y, u32 log_w, u32 log_h) {
    // RSX swizzle: interleave x/y bits while both have bits left, then the remaining bits of the larger one
    u32 off = 0, shift = 0;
    u32 lw = log_w, lh = log_h;
    while (lw || lh) {
        if (lw) { off |= (x & 1) << shift++; x >>= 1; lw--; }
        if (lh) { off |= (y & 1) << shift++; y >>= 1; lh--; }
    }
    return off;
}

u32 ilog2(u32 v) { u32 r = 0; while ((1u << r) < v) r++; return r; }

float half_to_float(u16 h) {
    u32 s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023;
    u32 f;
    if (e == 0) f = m ? 0 : (s << 31);
    else if (e == 31) f = (s << 31) | 0x7F800000 | (m << 13);
    else f = (s << 31) | ((e + 112) << 23) | (m << 13);
    if (e == 0 && m) { float v = std::ldexp((float)m, -24); return s ? -v : v; }
    float r;
    memcpy(&r, &f, 4);
    return r;
}

// ------------------------------------------------------------------ auxiliary shaders
// texture decode compute shader: the part after the declarations and rd8()
const char* tex_decode_glsl() {
    return
        "uint rd16(int a) { return (rd8(a) << 8) | rd8(a + 1); }\n"
        "uint rd32(int a) { return (rd16(a) << 16) | rd16(a + 2); }\n"
        "uint morton(uint x, uint y, int lw, int lh) {   // as morton() on the CPU\n"
        " uint off = 0u; int shift = 0;\n"
        " while (lw > 0 || lh > 0) {\n"
        "  if (lw > 0) { off |= (x & 1u) << shift; shift++; x >>= 1; lw--; }\n"
        "  if (lh > 0) { off |= (y & 1u) << shift; shift++; y >>= 1; lh--; }\n"
        " }\n"
        " return off;\n"
        "}\n"
        "void main() {\n"
        " ivec2 p = ivec2(gl_GlobalInvocationID.xy);\n"
        " if (p.x >= u_lvl.y || p.y >= u_lvl.z) return;\n"
        " int bpp = u_lay.z;\n"
        " int texel = u_lay.x != 0 ? p.y * u_lay.y + p.x * bpp : int(morton(uint(p.x), uint(p.y), u_lay.w & 255, u_lay.w >> 8)) * bpp;\n"
        " int a = u_lvl.x + texel, f = u_lvl.w;\n"
        " uvec4 c;   // r g b a\n"
        " if (f == 0x81) { uint v = rd8(a); c = uvec4(v); }\n"
        " else if (f == 0x83) { uint v = rd16(a); c = uvec4((v >> 8) & 15u, (v >> 4) & 15u, v & 15u, (v >> 12) & 15u) * 17u; }\n"
        " else if (f == 0x82) { uint v = rd16(a); c = uvec4(((v >> 10) & 31u) * 255u / 31u, ((v >> 5) & 31u) * 255u / 31u, (v & 31u) * 255u / 31u, (v >> 15) != 0u ? 255u : 0u); }\n"
        " else if (f == 0x84) { uint v = rd16(a); c = uvec4(((v >> 11) & 31u) * 255u / 31u, ((v >> 5) & 63u) * 255u / 63u, (v & 31u) * 255u / 31u, 255u); }\n"
        " else if (f == 0x8B) { uint v = rd16(a); c = uvec4(v >> 8, v >> 8, v & 255u, v & 255u); }\n"
        " else { uint v = rd32(a); c = uvec4((v >> 16) & 255u, (v >> 8) & 255u, v & 255u, v >> 24); }   // A8R8G8B8 and the rest\n"
        " imageStore(u_dst, p, vec4(c) / 255.0);\n"
        "}\n";
}
// presentation fragment shader: the part after the declarations (u_src, u_p, u_win, o). A negative u_win.w maps a window
// whose y runs against the image's (Vulkan).
const char* present_glsl() {
    return
            "vec3 tap(vec2 uv) { return textureLod(u_src, uv, u_p.x).rgb; }\n"
            "float luma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }\n"
            "void main() {\n"
            " vec2 uv = (gl_FragCoord.xy - u_win.xy) / u_win.zw, r = u_p.zw;\n"
            " vec3 m = tap(uv);\n"
            " if (u_p.y < 0.5) { o = vec4(m, 1.0); return; }\n"
            " vec3 nw = tap(uv + vec2(-1.0, -1.0) * r), ne = tap(uv + vec2(1.0, -1.0) * r);\n"
            " vec3 sw = tap(uv + vec2(-1.0, 1.0) * r), se = tap(uv + vec2(1.0, 1.0) * r);\n"
            " float lnw = luma(nw), lne = luma(ne), lsw = luma(sw), lse = luma(se), lm = luma(m);\n"
            " float lmin = min(lm, min(min(lnw, lne), min(lsw, lse))), lmax = max(lm, max(max(lnw, lne), max(lsw, lse)));\n"
            " vec2 dir = vec2(-((lnw + lne) - (lsw + lse)), (lnw + lsw) - (lne + lse));\n"
            " float reduce = max((lnw + lne + lsw + lse) * (0.25 / 8.0), 1.0 / 128.0);\n"
            " dir = clamp(dir / (min(abs(dir.x), abs(dir.y)) + reduce), vec2(-8.0), vec2(8.0)) * r;\n"
            " vec3 a = 0.5 * (tap(uv + dir * (1.0 / 3.0 - 0.5)) + tap(uv + dir * (2.0 / 3.0 - 0.5)));\n"
            " vec3 b = a * 0.5 + 0.25 * (tap(uv - dir * 0.5) + tap(uv + dir * 0.5));\n"
            " float lb = luma(b);\n"
            " o = vec4(lb < lmin || lb > lmax ? a : b, 1.0);\n"
            "}\n";
}

// ------------------------------------------------------------------ 2D engine, the parts on guest memory
static u32 sreg(u32 sub, u32 mm) { return R[((sub << 13) | mm) >> 2]; }

bool method_2d_inline(u32 sub, u32 mm, u32 arg) {
    if (sub == 5 && mm >= 0x400 && mm < 0x2000) {          // NV308A COLOR[i]
        u32 fmt = sreg(3, 0x300), pitch_out = sreg(3, 0x304) >> 16;
        u32 bpp = fmt == 0xB || fmt == 0xA ? 4 : 2;           // Y32 / A8R8G8B8 : R5G6B5
        u32 point = sreg(5, 0x304), x = point & 0xFFFF, y = point >> 16;   // POINT (0x300 = colour format)
        u32 idx = (mm - 0x400) >> 2;
        u32 dst = sreg(3, 0x30C) + y * pitch_out + (x + idx) * bpp;
        u32 ea = location_to_ea(dst, sreg(3, 0x188) == CTX_MAIN);
        if (bpp == 4) st32(ea, arg); else st16(ea, (u16)arg);
        static const long long trace_frame = getenv("F1CE_TRACE_FRAME") ? atoll(getenv("F1CE_TRACE_FRAME")) : -1;
        if (trace_on(trace_frame) && idx == 0) LOG_I("trace NV308A -> %08x", dst);
        if (dst - g_watch_fp < 0x800) {
            static int wl = 0;
            if (wl++ < 300) LOG_I("watch fp: NV308A +%03x = %08x (%g)", dst - g_watch_fp, arg, *(float*)&arg);
        }
        static int logged = 0;
        if (logged++ < 64)
            LOG_I("rsx: NV308A fmt %x pitch %u point %u,%u [%u] -> %08x%s = %08x", fmt, pitch_out, x, y, idx, dst,
                  sreg(3, 0x188) == CTX_MAIN ? "(main)" : "", arg);
        return true;
    }
    return false;
}

bool nv0039_transfer(u32 sub, u32 mm, Nv0039& x) {
    if (sub != 1 || mm != 0x328) return false;              // NV0039 BUFFER_NOTIFY: run the transfer
    u32 src = sreg(1, 0x30C), dst = sreg(1, 0x310);
    s32 pitch_in = (s32)sreg(1, 0x314), pitch_out = (s32)sreg(1, 0x318);
    u32 len = sreg(1, 0x31C), lines = sreg(1, 0x320);
    bool src_main = sreg(1, 0x184) == CTX_MAIN, dst_main = sreg(1, 0x188) == CTX_MAIN;
    static int logged = 0;
    if (logged++ < 40)
        LOG_I("rsx: NV0039 copy %08x%s -> %08x%s len %u x %u pitch %d/%d", src, src_main ? "(main)" : "", dst,
              dst_main ? "(main)" : "", len, lines, pitch_in, pitch_out);
    if (!dst_main && dst < g_watch_fp + 0x800 && dst + len * std::max(1u, lines) > g_watch_fp)
        LOG_I("watch fp: NV0039 copy %08x%s -> %08x len %u x %u", src, src_main ? "(main)" : "", dst, len, lines);
    if (!pitch_in) pitch_in = (s32)len;
    if (!pitch_out) pitch_out = (s32)len;
    u32 sea = location_to_ea(src, src_main), dea = location_to_ea(dst, dst_main);
    for (u32 l = 0; l < lines; l++)
        memmove(gmem(dea + l * pitch_out), gmem(sea + l * pitch_in), len);
    x = {src, dst, len, lines, src_main, dst_main};
    return true;
}
}  // namespace rsx

u32 rsx::cube_faces_per_frame() {
    const char* e = getenv("F1CE_CUBE_FACES");
    return e ? (u32)std::clamp(atoi(e), 1, 6) : 6;
}
