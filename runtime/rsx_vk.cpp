// Vulkan renderer for the RSX command stream (F1CE_RENDERER=vulkan), next to the OpenGL one in rsx_gl.cpp.
//
// Runs on the RSX front-end thread. It follows rsx_gl.cpp decision by decision (what counts as a surface, when a target
// is stale, which draws are sRGB-encoded): the reasons are in the comments there, this file only says what differs.
//  - An image row holds what the same row holds under OpenGL (bottom-up surfaces): the programs keep their y and
//    gl_FragCoord, textures keep their flip, only the front face and the presentation change.
//  - One pipeline per program pair. Everything else (blend, masks, samples, culling, depth) is dynamic state, so the
//    program database compiles all pipelines before the game runs and no draw ever waits for one.
//  - Colour surfaces are plain RGBA8 and stay in the GENERAL layout, with one barrier after each render pass.
//  - Textures come from descriptor arrays: a draw passes indices in its parameter block and binds no descriptors.
//  - Copies into the vertex heap and texture uploads go to a command buffer that runs before the frame's draws:
//    heap blocks are never rewritten, and a changed texture gets a new image (draws recorded earlier see the old one).
//  - Occlusion counts and main-memory targets come back when the GPU finishes, without blocking at the flip: the
//    game sees them a frame or two later.
#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#endif
#include "host.h"
#include "kernel.h"
#include "rsx.h"
#include "rsx_common.h"

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>
#include <vector>

extern u64 g_rsx_idle_us;   // hle_gcm.cpp: time the FIFO reader waited for commands (profiling)
namespace rsx {
namespace vk {
bool glsl_to_spirv(int stage, const std::string& src, std::vector<u32>& spirv, std::string& log);   // rsx_vk_spirv.cpp

namespace {
// ------------------------------------------------------------------ loader
#define VK_INSTANCE_FUNCS(X)                                                                                         \
    X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) X(vkGetPhysicalDeviceProperties2)                 \
    X(vkGetPhysicalDeviceFeatures2) X(vkGetPhysicalDeviceMemoryProperties) X(vkGetPhysicalDeviceQueueFamilyProperties) \
    X(vkGetPhysicalDeviceFormatProperties) X(vkGetPhysicalDeviceSurfaceSupportKHR)                                   \
    X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) X(vkGetPhysicalDeviceSurfaceFormatsKHR)                             \
    X(vkGetPhysicalDeviceSurfacePresentModesKHR) X(vkEnumerateDeviceExtensionProperties) X(vkCreateDevice)           \
    X(vkGetDeviceProcAddr)
#define VK_DEVICE_FUNCS(X)                                                                                           \
    X(vkGetDeviceQueue) X(vkCreateSwapchainKHR) X(vkDestroySwapchainKHR) X(vkGetSwapchainImagesKHR)                  \
    X(vkAcquireNextImageKHR) X(vkQueuePresentKHR) X(vkQueueSubmit2) X(vkQueueWaitIdle) X(vkDeviceWaitIdle)           \
    X(vkCreateSemaphore) X(vkWaitSemaphores) X(vkGetSemaphoreCounterValue) X(vkCreateCommandPool)                    \
    X(vkResetCommandPool) X(vkAllocateCommandBuffers) X(vkBeginCommandBuffer) X(vkEndCommandBuffer)                  \
    X(vkAllocateMemory) X(vkMapMemory) X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements)         \
    X(vkBindBufferMemory) X(vkFreeMemory) X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements)         \
    X(vkBindImageMemory) X(vkCreateImageView) X(vkDestroyImageView) X(vkCreateSampler) X(vkCreateDescriptorSetLayout) \
    X(vkCreateDescriptorPool) X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) X(vkCreatePipelineLayout)        \
    X(vkCreateShaderModule) X(vkCreateGraphicsPipelines) X(vkCreateComputePipelines) X(vkCreatePipelineCache)        \
    X(vkGetPipelineCacheData) X(vkCreateQueryPool) X(vkGetQueryPoolResults) X(vkCmdResetQueryPool)                   \
    X(vkCmdBeginQuery) X(vkCmdEndQuery) X(vkCmdBeginRendering) X(vkCmdEndRendering) X(vkCmdPipelineBarrier2)         \
    X(vkCmdBindPipeline) X(vkCmdBindDescriptorSets) X(vkCmdBindIndexBuffer) X(vkCmdDrawIndexed) X(vkCmdDraw)         \
    X(vkCmdDispatch) X(vkCmdPushConstants) X(vkCmdClearAttachments) X(vkCmdClearColorImage)                          \
    X(vkCmdClearDepthStencilImage) X(vkCmdCopyBuffer) X(vkCmdCopyBufferToImage) X(vkCmdCopyImageToBuffer)            \
    X(vkCmdBlitImage) X(vkCmdSetViewportWithCount) X(vkCmdSetScissorWithCount) X(vkCmdSetCullMode)                   \
    X(vkCmdSetFrontFace) X(vkCmdSetPrimitiveTopology) X(vkCmdSetPrimitiveRestartEnable) X(vkCmdSetDepthTestEnable)   \
    X(vkCmdSetDepthWriteEnable) X(vkCmdSetDepthCompareOp) X(vkCmdSetBlendConstants) X(vkCmdSetColorBlendEnableEXT)   \
    X(vkCmdSetColorBlendEquationEXT) X(vkCmdSetColorWriteMaskEXT) X(vkCmdSetAlphaToCoverageEnableEXT)                \
    X(vkCmdSetRasterizationSamplesEXT) X(vkCmdPushDescriptorSetKHR)

#define X(name) PFN_##name name;
VK_INSTANCE_FUNCS(X)
VK_DEVICE_FUNCS(X)
#undef X
PFN_vkCreateInstance vkCreateInstance;
PFN_vkEnumerateInstanceVersion vkEnumerateInstanceVersion;

bool g_vk_ok = false;
VkInstance g_inst;
VkPhysicalDevice g_phys;
VkDevice g_dev;
VkQueue g_queue;
u32 g_qfam = 0;
VkSurfaceKHR g_surface;
VkPhysicalDeviceProperties g_props;
VkPhysicalDeviceMemoryProperties g_memprops;
VkFormat g_depth_fmt = VK_FORMAT_D24_UNORM_S8_UINT;
bool g_list_restart = false;   // primitive restart on list topologies

[[noreturn]] void fatal(const char* what) {
    LOG_E("rsx: Vulkan: %s (unset F1CE_RENDERER to use OpenGL)", what);
    fflush(stdout);
    fflush(stderr);
    _exit(1);
}
void check(VkResult r, const char* what) {
    if (r == VK_SUCCESS) return;
    char b[160];
    snprintf(b, sizeof(b), "%s failed: %d", what, (int)r);
    fatal(b);
}

// ------------------------------------------------------------------ submissions
// Every command buffer pair has a serial; the timeline semaphore counts the finished ones.
constexpr u32 SLOTS = 8, QUERIES = 4096;
struct Slot {
    VkCommandPool pool = 0;
    VkCommandBuffer cb = 0, ucb = 0;   // draws; uploads, which run first
    VkQueryPool qpool = 0;
    u32 qused = 0;
    u64 qfirst = 0, serial = 0;        // id of its first query, serial it was submitted with
    bool qread = true;
};
Slot g_slots[SLOTS];
u32 g_slot = 0;
VkSemaphore g_timeline;
u64 g_serial = 1;      // of the command buffers being recorded
u64 g_completed = 0;
VkCommandBuffer g_cb, g_ucb;

u64 completed() {
    check(vkGetSemaphoreCounterValue(g_dev, g_timeline, &g_completed), "vkGetSemaphoreCounterValue");
    return g_completed;
}
void submit(VkSemaphore wait = 0, VkSemaphore signal = 0);
void wait_serial(u64 s) {
    if (g_completed >= s) return;
    if (s >= g_serial) submit();
    VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
    wi.semaphoreCount = 1;
    wi.pSemaphores = &g_timeline;
    wi.pValues = &s;
    check(vkWaitSemaphores(g_dev, &wi, 5000000000ull), "waiting for the GPU");
    completed();
}

// resources released once the GPU has finished what was recorded before
std::deque<std::pair<u64, std::function<void()>>> g_dead;
void later(std::function<void()> f) { g_dead.push_back({g_serial, std::move(f)}); }
void run_dead() {
    while (!g_dead.empty() && g_dead.front().first <= g_completed) {
        g_dead.front().second();
        g_dead.pop_front();
    }
}

// ------------------------------------------------------------------ memory
u32 mem_type(u32 bits, VkMemoryPropertyFlags want, VkDeviceSize min_heap = 0) {
    for (u32 i = 0; i < g_memprops.memoryTypeCount; i++)
        if ((bits >> i & 1) && (g_memprops.memoryTypes[i].propertyFlags & want) == want &&
            g_memprops.memoryHeaps[g_memprops.memoryTypes[i].heapIndex].size >= min_heap)
            return i;
    return ~0u;
}
struct Buffer { VkBuffer buf = 0; VkDeviceMemory mem = 0; u8* ptr = nullptr; VkDeviceSize size = 0; };
// host = mapped; video = also in video memory when the whole of it is mapped (resizable BAR)
Buffer make_buffer(VkDeviceSize size, VkBufferUsageFlags usage, bool host, bool video) {
    Buffer b;
    b.size = size;
    VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    ci.size = size;
    ci.usage = usage;
    check(vkCreateBuffer(g_dev, &ci, nullptr, &b.buf), "vkCreateBuffer");
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(g_dev, b.buf, &mr);
    constexpr VkMemoryPropertyFlags HV = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    u32 type = ~0u;
    if (host && video) type = mem_type(mr.memoryTypeBits, HV | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 1024ull << 20);
    if (host && type == ~0u) type = mem_type(mr.memoryTypeBits, HV);
    if (!host) type = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type == ~0u) fatal("no memory type for a buffer");
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = type;
    check(vkAllocateMemory(g_dev, &ai, nullptr, &b.mem), "vkAllocateMemory");
    check(vkBindBufferMemory(g_dev, b.buf, b.mem, 0), "vkBindBufferMemory");
    if (host) check(vkMapMemory(g_dev, b.mem, 0, VK_WHOLE_SIZE, 0, (void**)&b.ptr), "vkMapMemory");
    return b;
}

// Segmented ring as in rsx_gl.cpp, a segment being reusable when the serial that used it has finished.
struct Ring {
    Buffer b;
    u32 base = 0, seg = 0, nseg = 0, pos = 0;   // base: start of the ring in the buffer
    u64 serial[16] = {};
};
u8* ring_take(Ring& r, u32 bytes, u32 align, ptrdiff_t* off) {
    if (bytes > r.seg) return nullptr;
    u32 pos = (r.pos + align - 1) & ~(align - 1);
    u32 s = pos / r.seg;
    if (s >= r.nseg || (pos + bytes - 1) / r.seg != s) {
        u32 cur = std::min(s, r.nseg - 1), next = (cur + 1) % r.nseg;
        r.serial[cur] = g_serial;
        if (r.serial[next]) {
            u64 tw = time_usec();
            wait_serial(r.serial[next]);
            g_prof.t_ringwait += time_usec() - tw;
            g_prof.ring_segs++;
        }
        r.serial[next] = 0;
        pos = next * r.seg;
    }
    r.pos = pos + bytes;
    *off = (ptrdiff_t)r.base + pos;
    return r.b.ptr + r.base + pos;
}

// Image memory: chunks of device memory divided with the block heap (a process may hold only ~4000 allocations)
struct Chunk { VkDeviceMemory mem; BlockHeap heap; u32 type; };
std::vector<std::unique_ptr<Chunk>> g_chunks;
struct ImgMem { Chunk* chunk = nullptr; u32 block = 0, bytes = 0; VkDeviceSize off = 0; };
ImgMem img_alloc(const VkMemoryRequirements& mr) {
    constexpr u32 CHUNK = 256u << 20;
    if (mr.size + mr.alignment > CHUNK) fatal("image too large");
    u32 type = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type == ~0u) fatal("no memory type for an image");
    u32 bytes = (u32)(mr.size + mr.alignment), block = ~0u;
    Chunk* c = nullptr;
    for (auto& ch : g_chunks) {
        if (ch->type != type) continue;
        block = heap_alloc(ch->heap, bytes);
        if (block != ~0u) { c = ch.get(); break; }
    }
    if (!c) {
        auto ch = std::make_unique<Chunk>();
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = CHUNK;
        ai.memoryTypeIndex = type;
        check(vkAllocateMemory(g_dev, &ai, nullptr, &ch->mem), "vkAllocateMemory (images)");
        ch->type = type;
        ch->heap.size = CHUNK;
        heap_insert_free(ch->heap, 0, CHUNK);
        block = heap_alloc(ch->heap, bytes);
        c = ch.get();
        g_chunks.push_back(std::move(ch));
        LOG_I("rsx: image memory chunk %zu", g_chunks.size());
    }
    ImgMem m;
    m.chunk = c;
    m.block = block;
    m.bytes = (bytes + BlockHeap::ALIGN - 1) & ~(BlockHeap::ALIGN - 1);
    m.off = (block + mr.alignment - 1) & ~(mr.alignment - 1);
    return m;
}

// ------------------------------------------------------------------ images and descriptors
struct Img {
    VkImage img = 0;
    VkImageView view = 0;        // sampled (depth formats: depth aspect)
    VkImageView att = 0;         // attachment
    VkImageView levels[13] = {}; // storage views (decoded textures)
    ImgMem mem;
    u32 w = 0, h = 0, nlevels = 1, id = 0;
    VkFormat fmt = VK_FORMAT_UNDEFINED;
    bool cube = false;
    std::vector<std::pair<u64, u32>> slots;   // descriptor array entries using it
};
constexpr u32 MAX_SLOTS = 32768;
VkDescriptorSet g_set0, g_set1;
VkDescriptorSetLayout g_layout0, g_layout1, g_layout_cs;
VkPipelineLayout g_pl, g_pl_cs;
std::unordered_map<u64, u32> g_slot_map;   // image id << 24 | sampler id -> array entry
std::vector<u32> g_slot_free;
u32 g_slot_next = 1;                       // entry 0 of both arrays: black
u32 g_img_id = 1;
std::unordered_map<u64, u32> g_sampler_ids;
std::vector<VkSampler> g_samplers;
Img* g_black = nullptr;
Img* g_black_cube = nullptr;

void write_slot(u32 slot, bool cube, VkImageView view, VkSampler s) {
    VkDescriptorImageInfo ii{s, view, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = g_set1;
    w.dstBinding = cube ? 1 : 0;
    w.dstArrayElement = slot;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = &ii;
    vkUpdateDescriptorSets(g_dev, 1, &w, 0, nullptr);
}
u32 tex_slot(Img* im, u32 sampler_id) {
    u64 key = ((u64)im->id << 24) | sampler_id;
    auto it = g_slot_map.find(key);
    if (it != g_slot_map.end()) return it->second;
    u32 slot;
    if (!g_slot_free.empty()) { slot = g_slot_free.back(); g_slot_free.pop_back(); }
    else if (g_slot_next < MAX_SLOTS) slot = g_slot_next++;
    else { LOG_W("rsx: out of texture descriptors"); return 0; }
    write_slot(slot, im->cube, im->view, g_samplers[sampler_id]);
    g_slot_map[key] = slot;
    im->slots.push_back({key, slot});
    return slot;
}

VkImageView make_view(VkImage img, VkFormat fmt, VkImageViewType type, VkImageAspectFlags aspect, u32 level, u32 nlevels, u32 layers) {
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = img;
    vi.viewType = type;
    vi.format = fmt;
    vi.subresourceRange = {aspect, level, nlevels, 0, layers};
    VkImageView v;
    check(vkCreateImageView(g_dev, &vi, nullptr, &v), "vkCreateImageView");
    return v;
}
void image_barrier(VkCommandBuffer cb, VkImage img, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = {aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    d.imageMemoryBarrierCount = 1;
    d.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cb, &d);
}
void memory_barrier(VkCommandBuffer cb) {
    VkMemoryBarrier2 b{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    d.memoryBarrierCount = 1;
    d.pMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cb, &d);
}

constexpr VkFormat COLOR_FMT = VK_FORMAT_R8G8B8A8_UNORM;
bool is_depth(VkFormat f) { return f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D32_SFLOAT_S8_UINT; }

// a new image in the GENERAL layout (the transition runs first, in the upload command buffer)
Img* make_image(u32 w, u32 h, u32 levels, VkFormat fmt, VkImageUsageFlags usage, VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT, bool cube = false) {
    Img* im = new Img;
    im->w = w; im->h = h; im->nlevels = levels; im->fmt = fmt; im->cube = cube; im->id = g_img_id++;
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.flags = cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = fmt;
    ci.extent = {w, h, 1};
    ci.mipLevels = levels;
    ci.arrayLayers = cube ? 6 : 1;
    ci.samples = samples;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    check(vkCreateImage(g_dev, &ci, nullptr, &im->img), "vkCreateImage");
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(g_dev, im->img, &mr);
    im->mem = img_alloc(mr);
    check(vkBindImageMemory(g_dev, im->img, im->mem.chunk->mem, im->mem.off), "vkBindImageMemory");
    const bool depth = is_depth(fmt);
    const VkImageViewType vt = cube ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D;
    const u32 layers = cube ? 6 : 1;
    if (usage & VK_IMAGE_USAGE_SAMPLED_BIT)
        im->view = make_view(im->img, fmt, vt, depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, layers);
    if (usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT))
        im->att = make_view(im->img, fmt, vt, depth ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, layers);
    if (usage & VK_IMAGE_USAGE_STORAGE_BIT)
        for (u32 l = 0; l < levels; l++) im->levels[l] = make_view(im->img, fmt, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT, l, 1, 1);
    image_barrier(g_ucb, im->img, depth ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_COLOR_BIT,
                  VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    return im;
}
void destroy_image(Img* im) {
    if (!im) return;
    later([im] {
        for (auto& [key, slot] : im->slots) {   // no entry keeps a dead view: a disabled unit may still index it
            g_slot_map.erase(key);
            if (im->cube) write_slot(slot, true, g_black_cube->view, g_samplers[0]);
            else write_slot(slot, false, g_black->view, g_samplers[0]);
            g_slot_free.push_back(slot);
        }
        for (VkImageView v : im->levels) if (v) vkDestroyImageView(g_dev, v, nullptr);
        if (im->view) vkDestroyImageView(g_dev, im->view, nullptr);
        if (im->att) vkDestroyImageView(g_dev, im->att, nullptr);
        vkDestroyImage(g_dev, im->img, nullptr);
        im->mem.chunk->heap.used -= im->mem.bytes;
        heap_insert_free(im->mem.chunk->heap, im->mem.block, im->mem.bytes);
        delete im;
    });
}

// sampler of a filter / wrap combination; id 0 = linear, clamped
struct SamplerKey {
    VkFilter mag, min; VkSamplerMipmapMode mip; VkSamplerAddressMode u, v;
    float bias, min_lod, max_lod, aniso;
};
u32 get_sampler(const SamplerKey& k) {
    u64 h = 1469598103934665603ull;
    const u32 words[9] = {(u32)k.mag, (u32)k.min, (u32)k.mip, (u32)k.u, (u32)k.v, (u32)(k.bias * 256.f + 32768.f), (u32)(k.min_lod * 256.f), (u32)(k.max_lod * 256.f), (u32)k.aniso};
    h = hash_words(words, 9, h);
    auto it = g_sampler_ids.find(h);
    if (it != g_sampler_ids.end()) return it->second;
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = k.mag; si.minFilter = k.min; si.mipmapMode = k.mip;
    si.addressModeU = k.u; si.addressModeV = k.v; si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.mipLodBias = k.bias;
    si.minLod = k.min_lod;
    si.maxLod = std::max(k.min_lod, k.max_lod);
    si.anisotropyEnable = k.aniso > 1.0f;
    si.maxAnisotropy = std::min(k.aniso, g_props.limits.maxSamplerAnisotropy);
    si.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    VkSampler s;
    check(vkCreateSampler(g_dev, &si, nullptr, &s), "vkCreateSampler");
    g_samplers.push_back(s);
    u32 id = (u32)g_samplers.size() - 1;
    if (id >= 1u << 24) fatal("too many samplers");
    return g_sampler_ids[h] = id;
}

// ------------------------------------------------------------------ surfaces
struct Surface {
    u32 offset = 0, width = 0, height = 0, format = 0;
    Img* tex = nullptr;
    Img* attached_depth = nullptr;
    u64 last_use = 0;
    u64 mem_sig = 0, sig_checked = ~0ull;   // (see rsx_gl.cpp)
    u32 sig_bytes = 0;
    bool stale = false;
    float scale_x = 1, scale_y = 1;
    bool local_wb = false;
    u64 write_seq = 0;
    Img* ms_color = nullptr;   // antialiased target: drawn here, resolved into tex / the zeta texture before reads
    Img* ms_depth = nullptr;
    bool ms_active = false, ms_dirty = false;
    u32 main_ea = 0, pitch = 0;
    bool wb_dirty = false, wb_pending = false;
    Buffer wb_buf;
    u64 wb_serial = 0;
};
struct DepthSurface {
    u32 offset = 0, width = 0, height = 0;
    float scale_x = 1, scale_y = 1;   // (those of the colour targets it is used with)
    Img* tex = nullptr;
    u64 write_seq = 0;
};
u64 g_write_seq = 0, g_square_write_seq = 0;
DepthSurface* g_cur_depth = nullptr;
std::unordered_map<u32, Surface> g_surfaces;
std::unordered_map<u32, DepthSurface> g_depths;
std::unordered_map<u64, Surface> g_surface_spares;
std::vector<u32> g_ms_surfaces;
u32 g_surf_epoch = 0;
std::map<u32, u32> g_frame_targets;

// the render pass being recorded
struct Pass { bool on = false; VkImageView color = 0, depth = 0; Img* color_img = nullptr; Img* depth_img = nullptr; u32 w = 0, h = 0; VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT; } g_pass;
struct Queries { bool enabled = false, open = false; u64 id = 0; } g_q;
void q_begin();
void q_end();

void end_pass() {
    if (!g_pass.on) return;
    q_end();
    vkCmdEndRendering(g_cb);
    g_pass.on = false;
    memory_barrier(g_cb);
}

constexpr VkImageUsageFlags SURF_USAGE = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
constexpr VkImageUsageFlags ZETA_USAGE = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

void clear_new(Img* im) {   // (surfaces start black, not with what the memory held)
    if (is_depth(im->fmt)) {
        VkClearDepthStencilValue v{1.0f, 0};
        VkImageSubresourceRange r{VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1};
        vkCmdClearDepthStencilImage(g_ucb, im->img, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &r);
    } else {
        VkClearColorValue v{};
        VkImageSubresourceRange r{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, im->cube ? 6u : 1u};
        vkCmdClearColorImage(g_ucb, im->img, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &r);
    }
}

Surface* get_surface(u32 offset, u32 w, u32 h, u32 fmt) {
    Surface& s = g_surfaces[offset];
    if (s.tex && (s.width != w || s.height != h)) {
        auto size_key = [&](u32 sw, u32 sh) { return ((u64)offset << 32) | ((u64)sw << 16) | sh; };
        Surface old = s;
        auto it = g_surface_spares.find(size_key(w, h));
        if (it != g_surface_spares.end()) { s = it->second; g_surface_spares.erase(it); } else s = Surface{};
        g_surface_spares[size_key(old.width, old.height)] = old;
        g_surf_epoch++;
    }
    if (!s.tex) {
        g_surf_epoch++;
        s.offset = offset; s.width = w; s.height = h; s.format = fmt;
        s.scale_x = (offset & MAIN_SURFACE_KEY) ? 1 : target_scale_x(w, h);
        s.scale_y = (offset & MAIN_SURFACE_KEY) ? 1 : target_scale_y(w, h);
        s.tex = make_image(scaled(w, s.scale_x), scaled(h, s.scale_y), 1, COLOR_FMT, SURF_USAGE);
        clear_new(s.tex);
        LOG_I("rsx: surface %08x %ux%u fmt %x", offset, w, h, fmt);
    }
    s.last_use = g_frame;
    return &s;
}

VkSampleCountFlagBits msaa_samples() {
    static const VkSampleCountFlagBits v = [] {
        const char* e = getenv("F1CE_MSAA");
        int x = e ? atoi(e) : 4;
        if (x != 2 && x != 4 && x != 8 && x != 16) x = 4;
        VkSampleCountFlags have = g_props.limits.framebufferColorSampleCounts & g_props.limits.framebufferDepthSampleCounts;
        while (x > 1 && !(have & (VkSampleCountFlags)x)) x >>= 1;
        return (VkSampleCountFlagBits)x;
    }();
    return v;
}

void ensure_ms(Surface* s) {
    if (s->ms_color) return;
    constexpr VkImageUsageFlags CU = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, DU = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    s->ms_color = make_image(scaled(s->width, s->scale_x), scaled(s->height, s->scale_y), 1, COLOR_FMT, CU, msaa_samples());
    s->ms_depth = make_image(scaled(s->width, s->scale_x), scaled(s->height, s->scale_y), 1, g_depth_fmt, DU, msaa_samples());
    g_ms_surfaces.push_back(s->offset);
    LOG_I("rsx: surface %08x %ux%u is antialiased: %ux multisampled", s->offset, s->width, s->height, (u32)msaa_samples());
}

// a pass with no draws: its end resolves the multisampled attachments
bool resolve_ms(Surface& s) {
    if (!s.ms_color || !s.ms_dirty) return false;
    end_pass();
    VkRenderingAttachmentInfo c{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    c.imageView = s.ms_color->att;
    c.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    c.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
    c.resolveImageView = s.tex->att;
    c.resolveImageLayout = VK_IMAGE_LAYOUT_GENERAL;
    c.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    c.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingAttachmentInfo d{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    d.imageView = s.ms_depth->att;
    d.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    d.resolveMode = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
    d.resolveImageView = s.attached_depth ? s.attached_depth->att : 0;
    d.resolveImageLayout = VK_IMAGE_LAYOUT_GENERAL;
    d.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    d.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {scaled(s.width, s.scale_x), scaled(s.height, s.scale_y)}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &c;
    if (s.attached_depth) { ri.pDepthAttachment = &d; ri.pStencilAttachment = &d; }
    vkCmdBeginRendering(g_cb, &ri);
    vkCmdEndRendering(g_cb);
    memory_barrier(g_cb);
    s.ms_dirty = false;
    return true;
}
void resolve_for(Img* tex) {
    for (u32 off : g_ms_surfaces) {
        auto it = g_surfaces.find(off);
        if (it != g_surfaces.end() && (it->second.tex == tex || it->second.attached_depth == tex)) resolve_ms(it->second);
    }
}
void resolve_all_ms() {
    for (u32 off : g_ms_surfaces) {
        auto it = g_surfaces.find(off);
        if (it != g_surfaces.end()) resolve_ms(it->second);
    }
}
// before an image is read as a texture: what the open pass wrote must be finished
void sampled(Img* im) {
    if (g_pass.on && (g_pass.color_img == im || g_pass.depth_img == im)) end_pass();
}

DepthSurface* get_depth(u32 offset, u32 w, u32 h) {
    DepthSurface& d = g_depths[offset];
    if (d.tex && (d.width < w || d.height < h || d.scale_x != target_scale_x(w, h) || d.scale_y != target_scale_y(w, h))) {
        sampled(d.tex);
        destroy_image(d.tex);
        d.tex = nullptr;
    }
    if (!d.tex) {
        g_surf_epoch++;
        d.offset = offset; d.width = w; d.height = h;
        d.scale_x = target_scale_x(w, h); d.scale_y = target_scale_y(w, h);
        d.tex = make_image(scaled(w, d.scale_x), scaled(h, d.scale_y), 1, g_depth_fmt, ZETA_USAGE);
        clear_new(d.tex);
        LOG_I("rsx: depth surface %08x %ux%u", offset, w, h);
    }
    return &d;
}

// colour pixels of an image into host memory; waits for the GPU (debug dumps only)
void read_image(Img* im, u32 w, u32 h, u32* out) {
    end_pass();
    Buffer b = make_buffer((VkDeviceSize)w * h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, false);
    VkBufferImageCopy c{};
    c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    c.imageExtent = {w, h, 1};
    vkCmdCopyImageToBuffer(g_cb, im->img, VK_IMAGE_LAYOUT_GENERAL, b.buf, 1, &c);
    memory_barrier(g_cb);
    u64 s = g_serial;
    submit();
    wait_serial(s);
    const u8* p = b.ptr;
    for (size_t i = 0; i < (size_t)w * h; i++, p += 4)   // RGBA bytes -> host ARGB
        out[i] = ((u32)p[3] << 24) | ((u32)p[0] << 16) | ((u32)p[1] << 8) | p[2];
    vkDestroyBuffer(g_dev, b.buf, nullptr);
    vkFreeMemory(g_dev, b.mem, nullptr);
}

// debug: writes surface `off` to frame_s<off><tag>_<frame>_WxH.argb (top row first, BGRA bytes)
void dump_surface(u32 off, const char* tag) {
    auto it = g_surfaces.find(off);
    if (it == g_surfaces.end() || !it->second.tex) return;
    Surface& sf = it->second;
    resolve_ms(sf);
    const u32 pw = scaled(sf.width, sf.scale_x), ph = scaled(sf.height, sf.scale_y);
    std::vector<u32> px((size_t)pw * ph), fl(px.size());
    read_image(sf.tex, pw, ph, px.data());
    for (u32 y = 0; y < ph; y++) memcpy(&fl[(size_t)y * pw], &px[(size_t)(ph - 1 - y) * pw], pw * 4);
    char name[96];
    snprintf(name, sizeof(name), "frame_s%08x%s_%05llu_%ux%u.argb", off, tag, (unsigned long long)g_frame, pw, ph);
    if (FILE* f = fopen(name, "wb")) { fwrite(fl.data(), 4, fl.size(), f); fclose(f); }
}

// main-memory colour buffers: copied to a mapped buffer once per frame, and to guest memory when the GPU has finished
void write_back_pending(Surface* s) {
    if (!s->wb_pending || g_completed < s->wb_serial) return;
    const u8* p = s->wb_buf.ptr;
    u32 pitch = s->pitch ? s->pitch : s->width * 4;
    bool b8 = (s->format & 0x1F) == 9;
    for (u32 y = 0; y < s->height; y++)
        for (u32 x = 0; x < s->width; x++) {
            const u8* q = p + ((size_t)(s->height - 1 - y) * s->width + x) * 4;
            u32 v = ((u32)q[3] << 24) | ((u32)q[0] << 16) | ((u32)q[1] << 8) | q[2];
            if (b8) st8(s->main_ea + y * pitch + x, (u8)v);
            else st32(s->main_ea + y * pitch + x * 4, v);
        }
    if (s->local_wb && s->sig_bytes) s->mem_sig = mem_fingerprint(s->main_ea, s->sig_bytes);
    if (s->height == 1 && g_frame % 300 == 0) {
        std::string t;
        for (u32 x = 0; x < std::min(s->width, 24u); x++) { char b[12]; snprintf(b, sizeof(b), " %08x", ld32(s->main_ea + x * 4)); t += b; }
        LOG_I("rsx: lighting table %08x:%s", s->main_ea, t.c_str());
    }
    s->wb_pending = false;
}
void write_back_start(Surface* s) {
    if (!s->wb_dirty || s->wb_pending) return;
    end_pass();
    if (!s->wb_buf.buf) s->wb_buf = make_buffer((VkDeviceSize)s->width * s->height * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, false);
    VkBufferImageCopy c{};
    c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    c.imageExtent = {s->width, s->height, 1};
    vkCmdCopyImageToBuffer(g_cb, s->tex->img, VK_IMAGE_LAYOUT_GENERAL, s->wb_buf.buf, 1, &c);
    s->wb_dirty = false;
    s->wb_pending = true;
    s->wb_serial = g_serial;
}

// the current colour surface A (+ zeta buffer); returns it (or null if unsupported). The pass starts in begin_pass.
Surface* bind_target() {
    u32 fmt = reg(0x0208);
    u32 w = reg(0x0200) >> 16, h = reg(0x0204) >> 16;
    if (!w || !h) return nullptr;
    bool main = reg(0x0194) == CTX_MAIN;
    if (!main && reg(0x0194) != CTX_LOCAL) return nullptr;
    Surface* s = get_surface(main ? (reg(0x0210) | MAIN_SURFACE_KEY) : reg(0x0210), w, h, fmt);
    g_frame_targets[s->offset]++;
    if (!main && (s->stale || s->sig_checked != g_frame)) {
        s->sig_bytes = std::max(1u, reg(0x020C) & 0xFFFF) * h;
        s->mem_sig = mem_fingerprint(location_to_ea(reg(0x0210), false), s->sig_bytes);
        s->sig_checked = g_frame;
        if (s->stale) g_surf_epoch++;
        s->stale = false;
    }
    if (main) {
        if (!s->main_ea) LOG_I("rsx: main-memory render target io %08x %ux%u fmt %x (written back after draws)", reg(0x0210), w, h, fmt);
        s->main_ea = io_offset_to_ea(reg(0x0210));
        s->pitch = reg(0x020C) & 0xFFFF;
    } else {
        static const bool wb_local = getenv("F1CE_WB_LOCAL") != nullptr;
        if (wb_local && w * h <= 640 * 360) {
            s->main_ea = location_to_ea(reg(0x0210), false);
            s->pitch = reg(0x020C) & 0xFFFF;
            s->local_wb = true;
        }
    }
    g_cur_depth = nullptr;
    Img* depth = nullptr;
    if (!main && ((fmt >> 5) & 7) && reg(0x0198) == CTX_LOCAL) {
        g_cur_depth = get_depth(reg(0x0214), w, h);
        depth = g_cur_depth->tex;
    }
    s->attached_depth = depth;
    static const bool no_msaa = getenv("F1CE_NO_MSAA") != nullptr;
    s->ms_active = !main && ((fmt >> 12) & 0xF) && !no_msaa;
    if (s->ms_active) ensure_ms(s);
    return s;
}

// The pipelines always expect a depth/stencil attachment: targets without one get an unused image of at least their size
Img* g_null_depth = nullptr;
Img* null_depth(u32 w, u32 h) {
    if (!g_null_depth || g_null_depth->w < w || g_null_depth->h < h) {
        if (g_null_depth) destroy_image(g_null_depth);
        u32 nw = std::max(w, g_null_depth ? g_null_depth->w : 0u), nh = std::max(h, g_null_depth ? g_null_depth->h : 0u);
        g_null_depth = make_image(nw, nh, 1, g_depth_fmt, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
    }
    return g_null_depth;
}

// dynamic state of the command buffer being recorded, so unchanged state records nothing
struct DynState {
    bool valid = false;
    float vp_x, vp_y, vp_w, vp_h; VkRect2D scissor;
    VkCullModeFlags cull; VkFrontFace front; VkPrimitiveTopology topo; VkBool32 restart;
    VkBool32 dt, dw; VkCompareOp dop; float bc[4]; VkBool32 blend;
    VkColorBlendEquationEXT eq; VkColorComponentFlags mask; VkBool32 a2c; VkSampleCountFlagBits samples;
    VkPipeline pipe;
    u32 set0[3];   // dynamic offsets of the bound set 0 (~0u: none in this command buffer)
} g_dyn;

void begin_pass(Surface* s) {
    Img* c = s->ms_active ? s->ms_color : s->tex;
    Img* d = s->ms_active ? s->ms_depth : s->attached_depth;
    const u32 pw = scaled(s->width, s->scale_x), ph = scaled(s->height, s->scale_y);
    if (!d) d = null_depth(pw, ph);
    if (g_pass.on && g_pass.color == c->att && g_pass.depth == d->att && g_pass.w == pw && g_pass.h == ph) return;
    end_pass();
    VkRenderingAttachmentInfo ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    ca.imageView = c->att;
    ca.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingAttachmentInfo da = ca;
    da.imageView = d->att;
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {pw, ph}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &ca;
    ri.pDepthAttachment = &da;
    ri.pStencilAttachment = &da;
    vkCmdBeginRendering(g_cb, &ri);
    g_pass.on = true;
    g_pass.color = c->att; g_pass.depth = d->att; g_pass.w = pw; g_pass.h = ph;
    g_pass.color_img = s->tex;
    g_pass.depth_img = s->attached_depth;
    q_begin();
}

// viewport and scissor of the target (physical pixels; y as in OpenGL, see the file comment)
VkRect2D target_scissor(const Surface* s, bool hud = false) {
    float kx = s->scale_x, ky = s->scale_y, ox = 0, oy = 0;
    const u32 w = s->width, h = s->height;
    u32 sx = reg(0x08C0) & 0xFFFF, sw = reg(0x08C0) >> 16, sy = reg(0x08C4) & 0xFFFF, sh = reg(0x08C4) >> 16;
    const s32 pw = (s32)scaled(w, kx), ph = (s32)scaled(h, ky);
    VkRect2D r{{0, 0}, {(u32)pw, (u32)ph}};
    if (hud) {
        float vw, vh;
        hud_rect(pw, ph, &ox, &oy, &vw, &vh);
        kx = vw / w; ky = vh / h;
    }
    if (sw && sh && sw < 4096 && sh < 4096) {
        s32 x0 = scaled_s(sx, kx) + (s32)ox, y0 = scaled_s((s32)h - (s32)(sy + sh), ky) + (s32)oy;
        s32 x1 = scaled_s(sx + sw, kx) + (s32)ox, y1 = scaled_s((s32)h - (s32)sy, ky) + (s32)oy;
        x0 = std::clamp(x0, 0, pw); x1 = std::clamp(x1, x0, pw);
        y0 = std::clamp(y0, 0, ph); y1 = std::clamp(y1, y0, ph);
        r = {{x0, y0}, {(u32)(x1 - x0), (u32)(y1 - y0)}};
    }
    return r;
}
void set_viewport(float x, float y, float w, float h) {
    if (g_dyn.valid && g_dyn.vp_x == x && g_dyn.vp_y == y && g_dyn.vp_w == w && g_dyn.vp_h == h) return;
    VkViewport vp{x, y, w, h, 0, 1};
    vkCmdSetViewportWithCount(g_cb, 1, &vp);
    g_dyn.vp_x = x; g_dyn.vp_y = y; g_dyn.vp_w = w; g_dyn.vp_h = h;
}
void set_scissor(const VkRect2D& r) {
    if (g_dyn.valid && !memcmp(&g_dyn.scissor, &r, sizeof(r))) return;
    vkCmdSetScissorWithCount(g_cb, 1, &r);
    g_dyn.scissor = r;
}

// ------------------------------------------------------------------ programs and pipelines
struct Program {
    VkPipeline pipe[3] = {};                // by topology class: triangles, lines, points
    std::atomic<int> state[3] = {};         // 0 not asked for, 1 being built, 2 ready, -1 failed
    u32 tex_used = 0;
    u64 fh = 0;
    bool uses_vtex = false;
    std::string* vs = nullptr;
    std::string* fs = nullptr;
    u32 vs_id = 0;
};
std::unordered_map<u64, Program> g_programs;
std::unordered_map<u64, FPInfo> g_fp_cache;
std::unordered_map<u64, std::string> g_vp_cache;
VkPipelineCache g_pcache;
std::atomic<u64> g_compiled{0}, g_compile_us{0};
std::atomic<bool> g_pcache_dirty{false};

std::string vk_cache_dir() {
    static const std::string d = [] {
        std::string p = shader_cache_dir() + "/vk";
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::u8path(p), ec);
        return p;
    }();
    return d;
}

// SPIR-V of a GLSL source, cached on disk by the text
std::mutex g_mod_m;
std::unordered_map<u64, VkShaderModule> g_modules;
VkShaderModule shader_module(int stage, const std::string& src) {
    u64 key = 1469598103934665603ull ^ (u64)stage;
    for (unsigned char ch : src) { key ^= ch; key *= 1099511628211ull; }
    {
        std::lock_guard lk(g_mod_m);
        auto it = g_modules.find(key);
        if (it != g_modules.end()) return it->second;
    }
    char path[512];
    snprintf(path, sizeof(path), "%s/%016llx.spv", vk_cache_dir().c_str(), (unsigned long long)key);
    std::vector<u32> spv;
    if (FILE* f = fopen(path, "rb")) {
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (n > 0 && !(n & 3)) { spv.resize((size_t)n / 4); if (fread(spv.data(), 4, spv.size(), f) != spv.size()) spv.clear(); }
        fclose(f);
    }
    if (spv.empty()) {
        std::string log;
        if (!glsl_to_spirv(stage, src, spv, log)) {
            LOG_E("rsx: GLSL to SPIR-V failed:\n%s\n--- source ---\n%s", log.c_str(), src.c_str());
            return 0;
        }
        if (FILE* f = fopen(path, "wb")) { fwrite(spv.data(), 4, spv.size(), f); fclose(f); }
    }
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = spv.size() * 4;
    ci.pCode = spv.data();
    VkShaderModule m;
    check(vkCreateShaderModule(g_dev, &ci, nullptr, &m), "vkCreateShaderModule");
    std::lock_guard lk(g_mod_m);
    auto [it, fresh] = g_modules.emplace(key, m);
    return it->second;   // (two threads may have built it: one module stays unused)
}

const VkDynamicState DYNAMIC_STATES[] = {
    VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT, VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT, VK_DYNAMIC_STATE_CULL_MODE, VK_DYNAMIC_STATE_FRONT_FACE,
    VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY, VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE, VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE,
    VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
    VK_DYNAMIC_STATE_COLOR_BLEND_ENABLE_EXT, VK_DYNAMIC_STATE_COLOR_BLEND_EQUATION_EXT, VK_DYNAMIC_STATE_COLOR_WRITE_MASK_EXT,
    VK_DYNAMIC_STATE_ALPHA_TO_COVERAGE_ENABLE_EXT, VK_DYNAMIC_STATE_RASTERIZATION_SAMPLES_EXT};

VkPipeline make_pipeline(VkShaderModule vs, VkShaderModule fs, VkPrimitiveTopology topo, VkFormat color, VkFormat depth) {
    if (!vs || !fs) return 0;
    VkPipelineShaderStageCreateInfo st[2]{{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}, {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = vs; st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = fs; st[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = topo;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState att{};
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &att;
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = (u32)std::size(DYNAMIC_STATES);
    dy.pDynamicStates = DYNAMIC_STATES;
    VkPipelineRenderingCreateInfo rc{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rc.colorAttachmentCount = 1;
    rc.pColorAttachmentFormats = &color;
    rc.depthAttachmentFormat = depth;
    rc.stencilAttachmentFormat = depth;
    VkGraphicsPipelineCreateInfo pi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pi.pNext = &rc;
    pi.stageCount = 2;
    pi.pStages = st;
    pi.pVertexInputState = &vi;
    pi.pInputAssemblyState = &ia;
    pi.pViewportState = &vp;
    pi.pRasterizationState = &rs;
    pi.pMultisampleState = &ms;
    pi.pDepthStencilState = &ds;
    pi.pColorBlendState = &cb;
    pi.pDynamicState = &dy;
    pi.layout = g_pl;
    VkPipeline p = 0;
    VkResult r = vkCreateGraphicsPipelines(g_dev, g_pcache, 1, &pi, nullptr, &p);
    if (r != VK_SUCCESS) { LOG_E("rsx: vkCreateGraphicsPipelines failed: %d", (int)r); return 0; }
    return p;
}

// Pipeline workers: unlike the OpenGL driver, building pipelines in parallel works
std::mutex g_job_m;
std::condition_variable g_job_cv;
std::deque<std::function<void()>> g_jobs;
std::atomic<int> g_jobs_left{0};
void start_workers() {
    static bool started = false;
    if (started) return;
    started = true;
    int n = std::clamp((int)std::thread::hardware_concurrency() - 2, 1, 16);
    if (const char* e = getenv("F1CE_COMPILE_THREADS")) n = std::clamp(atoi(e), 1, 32);
    LOG_I("rsx: %d pipeline threads", n);
    for (int i = 0; i < n; i++) std::thread([] {
        for (;;) {
            std::function<void()> job;
            {
                std::unique_lock lk(g_job_m);
                g_job_cv.wait(lk, [] { return !g_jobs.empty(); });
                job = std::move(g_jobs.front());
                g_jobs.pop_front();
            }
            job();
            g_jobs_left--;
        }
    }).detach();
}
void queue_job(std::function<void()> f) {
    g_jobs_left++;
    { std::lock_guard lk(g_job_m); g_jobs.push_back(std::move(f)); }
    g_job_cv.notify_one();
}

void build_pipeline(Program* p, int cls) {
    int expect = 0;
    if (!p->state[cls].compare_exchange_strong(expect, 1)) return;
    static const VkPrimitiveTopology topo[3] = {VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_PRIMITIVE_TOPOLOGY_LINE_LIST, VK_PRIMITIVE_TOPOLOGY_POINT_LIST};
    queue_job([p, cls] {
        u64 t0 = time_usec();
        p->pipe[cls] = make_pipeline(shader_module(0, *p->vs), shader_module(1, *p->fs), topo[cls], COLOR_FMT, g_depth_fmt);
        g_compile_us += time_usec() - t0;
        g_compiled++;
        g_pcache_dirty = true;
        p->state[cls] = p->pipe[cls] ? 2 : -1;
    });
}

void save_pipeline_cache() {
    size_t n = 0;
    if (vkGetPipelineCacheData(g_dev, g_pcache, &n, nullptr) != VK_SUCCESS || !n) return;
    std::vector<u8> d(n);
    if (vkGetPipelineCacheData(g_dev, g_pcache, &n, d.data()) != VK_SUCCESS) return;
    std::string tmp = vk_cache_dir() + "/pipelines.tmp", path = vk_cache_dir() + "/pipelines.bin";
    if (FILE* f = fopen(tmp.c_str(), "wb")) {
        fwrite(d.data(), 1, n, f);
        fclose(f);
        remove(path.c_str());
        rename(tmp.c_str(), path.c_str());
    }
    g_pcache_dirty = false;
}

// (swapchain code below)
void progress_bar(double frac, const char* what);

Program& program_of(u64 key, u64 vh, u64 fh, u32 tex_used) {
    auto [it, fresh] = g_programs.try_emplace(key);
    Program& p = it->second;
    if (fresh) {
        p.fh = fh;
        p.tex_used = tex_used;
        p.vs = &g_vp_cache[vh];
        p.fs = &g_fp_cache[fh].glsl;
        p.uses_vtex = p.vs->find("textureLod(vtex") != std::string::npos;
    }
    return p;
}

// Program database (see rsx_common.cpp): the pipelines of all known programs are built before the game runs
void precompile_programs() {
    std::vector<ProgEntry> entries = load_program_db();
    if (entries.empty() || getenv("F1CE_NO_PRECOMPILE")) return;
    u64 t0 = time_usec(), last_bar = 0;
    std::vector<u32> saved_vp(vp_words, vp_words + 512 * 4);
    std::vector<Program*> progs;
    for (const ProgEntry& e : entries) {
        if (!g_vp_cache.count(e.r.vh)) {
            memcpy(&vp_words[e.r.vp_start * 4], e.vp.data(), e.vp.size() * 4);
            g_vp_cache[e.r.vh] = translate_vp(e.r.vp_start).glsl;
        }
        if (!g_fp_cache.count(e.r.fh)) {
            g_fp_override = e.fp.data(); g_fp_override_ea = e.r.fp_ea; g_fp_override_size = e.r.fp_size;
            g_fp_hash_cur = e.r.fh;
            g_fp_stop_here = false;
            g_fp_cache[e.r.fh] = translate_fp(e.r.fp_ea, e.r.shader_control, e.r.cube_mask);
            g_fp_override = nullptr;
        }
        progs.push_back(&program_of(e.r.vh * 31 + e.r.fh, e.r.vh, e.r.fh, g_fp_cache[e.r.fh].tex_used));
    }
    memcpy(vp_words, saved_vp.data(), saved_vp.size() * 4);
    start_workers();
    for (Program* p : progs) build_pipeline(p, 0);
    while (g_jobs_left > 0) {
        if (time_usec() - last_bar > 50000) {
            progress_bar(1.0 - (double)g_jobs_left.load() / progs.size(), "shaders");
            last_bar = time_usec();
        }
        sleep_usec(2000);
    }
    progress_bar(1.0, "shaders");
    u32 failed = 0;
    for (Program* p : progs) failed += p->state[0] != 2;
    save_pipeline_cache();
    LOG_I("rsx: program database: %zu pipelines ready in %.1f s (%u failed)", progs.size(), (time_usec() - t0) / 1e6, failed);
}

u32 g_vp_gen = 1;   // bumped by every write to vp_words: the vertex program hash of the last draw stays valid until then
Program* get_program(std::vector<float>& fp_consts) {
    u32 vp_start = reg(0x1EA0) & 511;
    static u32 memo_gen = 0, memo_start = 0, vp_len = 0;
    static u64 vh = 0;
    if (memo_gen != g_vp_gen || memo_start != vp_start) {
        vp_len = 0;
        while (vp_start + vp_len < 512 && !(vp_words[(vp_start + vp_len) * 4 + 3] & 1)) vp_len++;
        vp_len++;
        vh = hash_words(&vp_words[vp_start * 4], vp_len * 4);
        memo_gen = g_vp_gen;
        memo_start = vp_start;
    }
    u32 fp_ctl = reg(0x08E4);
    u32 fp_ea = location_to_ea(fp_ctl & ~3u, (fp_ctl & 3) == 2);
    u32 shader_control = reg(0x1D60);
    u32 fp_size;
    u64 fh = hash_fp(fp_ea, fp_size) ^ ((u64)(shader_control & 0x4E) << 56);
    u32 cube_mask = 0;
    for (u32 u = 0; u < 16; u++)
        if ((reg(0x1A0C + u * 0x20) & 0x80000000) && (reg(0x1A04 + u * 0x20) & 4)) cube_mask |= 1u << u;
    if (cube_mask) fh ^= (u64)cube_mask * 0x9E3779B97F4A7C15ull;

    auto fit = g_fp_cache.find(fh);
    if (fit == g_fp_cache.end()) {
        g_fp_stop_here = false;
        g_fp_hash_cur = fh;
        fit = g_fp_cache.emplace(fh, translate_fp(fp_ea, shader_control, cube_mask)).first;
    }
    const FPInfo& fpi = fit->second;
    fp_consts.resize(std::max<size_t>(fpi.const_offsets.size(), 1) * 4);
    for (size_t i = 0; i < fpi.const_offsets.size(); i++)
        for (int k = 0; k < 4; k++) {
            u32 v = fp_word(fp_ea + fpi.const_offsets[i] + k * 4);
            memcpy(&fp_consts[i * 4 + k], &v, 4);
        }
    if (!g_vp_cache.count(vh)) g_vp_cache.emplace(vh, translate_vp(vp_start).glsl);
    u64 key = vh * 31 + fh;
    auto pit = g_programs.find(key);
    if (pit != g_programs.end()) return &pit->second;
    db_record(vp_start, vp_len, fp_ea, fp_size, shader_control, cube_mask, vh, fh);
    LOG_I("rsx: new program (vp %u instr @%u, fp %u bytes, %zu consts) vp_%016llx fp_%016llx: compiling in the background",
          vp_len, vp_start, fp_size, fpi.const_offsets.size(), (unsigned long long)vh, (unsigned long long)fh);
    return &program_of(key, vh, fh, fpi.tex_used);
}

// ------------------------------------------------------------------ rings and heaps
Ring g_ring, g_uring, g_sring, g_iring;   // stream vertices, uniforms, texture staging, index lists
Buffer g_heap_buf, g_ibuf;
BlockHeap g_heap, g_iheap;
constexpr u32 IDX_SIZE = 96u << 20, IDX_RING = 16u << 20;
std::deque<std::pair<u64, u64>> g_frame_serials;   // frame -> serial of its last commands

// blocks freed by frames the GPU has finished
void heaps_end_frame() {
    g_frame_serials.push_back({g_frame, g_serial});
    while (!g_frame_serials.empty() && g_frame_serials.front().second <= g_completed) {
        g_heap.done_frame = g_iheap.done_frame = g_frame_serials.front().first;
        g_frame_serials.pop_front();
    }
    for (BlockHeap* h : {&g_heap, &g_iheap})
        while (!h->pending.empty() && h->pending.front().frame <= h->done_frame) {
            heap_insert_free(*h, h->pending.front().off, h->pending.front().size);
            h->pending.pop_front();
        }
}

u8* ring_alloc(u32 bytes, ptrdiff_t* off) { return ring_take(g_ring, bytes, 16, off); }
ptrdiff_t idx_stream(const u32* data, u32 n) {
    ptrdiff_t off;
    if (u8* p = ring_take(g_iring, n * 4, 16, &off)) { memcpy(p, data, (size_t)n * 4); return off; }
    u32 block = heap_alloc(g_iheap, n * 4);   // larger than a ring segment: a block for this frame
    if (block == ~0u) { LOG_W("rsx: index list of %u entries does not fit", n); return 0; }
    memcpy(g_iheap.ptr + block, data, (size_t)n * 4);
    heap_free(g_iheap, block, n * 4);
    return block;
}
bool idx_store(const u32* data, u32 n, u32& off, u32& bytes) {
    heap_free(g_iheap, off, bytes);
    bytes = n * 4;
    off = heap_alloc(g_iheap, bytes);
    if (off == ~0u) return false;
    memcpy(g_iheap.ptr + off, data, bytes);
    return true;
}
void heap_copy_begin() {}
void heap_copy(ptrdiff_t ring_off, u32 heap_off, u32 n) {
    VkBufferCopy c{(VkDeviceSize)ring_off, heap_off, n};
    vkCmdCopyBuffer(g_ucb, g_ring.b.buf, g_heap_buf.buf, 1, &c);
}
constexpr bool LINE_LOOP_AS_STRIP = true;
constexpr u32 RING_SEG = 8u << 20;
#include "rsx_draw_shared.inl"

// ------------------------------------------------------------------ textures
struct TexEntry { Img* tex = nullptr; u64 hash = 0, sentinel = 0; u32 w = 0, h = 0, fmt = 0; u64 checked_frame = ~0ull, last_change = 0; };
std::unordered_map<u64, TexEntry> g_textures;
VkPipeline g_tex_cs = 0;
bool g_tex_gpu = false;

// staging space for an upload: its buffer and offset
u8* staging(u32 bytes, VkBuffer* buf, ptrdiff_t* off) {
    u8* p = ring_take(g_sring, bytes, 16, off);
    if (p) { *buf = g_sring.b.buf; return p; }
    Buffer b = make_buffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true, false);
    later([b] { vkDestroyBuffer(g_dev, b.buf, nullptr); vkFreeMemory(g_dev, b.mem, nullptr); });
    *buf = b.buf;
    *off = 0;
    return b.ptr;
}

// compute decode of one texture (see tex_gpu in rsx_gl.cpp), recorded in the upload command buffer
void decode_levels(Img* im, u32 base_fmt, bool linear, u32 pitch, u32 bpp, const u8* src, u32 total, u32 levels) {
    VkBuffer buf;
    ptrdiff_t soff;
    u8* p = staging((total + 3) & ~3u, &buf, &soff);
    memcpy(p, src, total);
    vkCmdBindPipeline(g_ucb, VK_PIPELINE_BIND_POINT_COMPUTE, g_tex_cs);
    u32 off = 0;
    for (u32 l = 0; l < levels; l++) {
        u32 lw = std::max(1u, im->w >> l), lh = std::max(1u, im->h >> l);
        VkDescriptorImageInfo ii{0, im->levels[l], VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorBufferInfo bi{buf, (VkDeviceSize)soff, ((VkDeviceSize)total + 3) & ~3ull};
        VkWriteDescriptorSet w[2]{{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
        w[0].dstBinding = 0; w[0].descriptorCount = 1; w[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; w[0].pImageInfo = &ii;
        w[1].dstBinding = 1; w[1].descriptorCount = 1; w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[1].pBufferInfo = &bi;
        vkCmdPushDescriptorSetKHR(g_ucb, VK_PIPELINE_BIND_POINT_COMPUTE, g_pl_cs, 0, 2, w);
        const s32 pc[8] = {(s32)off, (s32)lw, (s32)lh, (s32)base_fmt, linear ? 1 : 0, (s32)(pitch ? pitch : lw * bpp), (s32)bpp, (s32)(ilog2(lw) | (ilog2(lh) << 8))};
        vkCmdPushConstants(g_ucb, g_pl_cs, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), pc);
        vkCmdDispatch(g_ucb, (lw + 7) / 8, (lh + 7) / 8, 1);
        off += lw * lh * bpp;
    }
}

Img* upload_texture(u32 unit, bool* is_surface, int* depth_func) {
    *is_surface = false;
    *depth_func = 0;
    u32 base = 0x1A00 + unit * 0x20;
    u32 offset = reg(base), format = reg(base + 4);
    u32 rect = reg(base + 0x18);
    u32 w = rect >> 16, h = rect & 0xFFFF;
    u32 pitch = reg(0x1840 + unit * 4) & 0xFFFFF;
    u32 fmt = (format >> 8) & 0xFF;
    bool linear = fmt & 0x20;
    u32 base_fmt = fmt & ~0x60u;
    bool main_mem = (format & 3) == 2;
    if (!w || !h || w > 4096 || h > 4096) return nullptr;
    bool dxt = base_fmt == 0x86 || base_fmt == 0x87 || base_fmt == 0x88;
    if (!main_mem && !dxt) {
        bool depth_format = base_fmt >= 0x90 && base_fmt <= 0x93;
        auto dit = g_depths.find(offset);
        if (depth_format && dit != g_depths.end() && dit->second.tex) {
            *is_surface = true;
            *depth_func = (int)((reg(base + 8) >> 28) & 7) + 1;
            resolve_for(dit->second.tex);
            return dit->second.tex;
        }
        auto it = g_surfaces.find(offset);
        if (dit != g_depths.end() && dit->second.tex &&
            (it == g_surfaces.end() || !it->second.tex || dit->second.write_seq > it->second.write_seq)) {
            *is_surface = true;
            *depth_func = 9;
            resolve_for(dit->second.tex);
            return dit->second.tex;
        }
        if (it != g_surfaces.end() && it->second.tex) {
            Surface& sf = it->second;
            if (sf.sig_checked != g_frame) {
                sf.sig_checked = g_frame;
                u64 tf = prof_usec();
                if (sf.sig_bytes && mem_fingerprint(location_to_ea(offset, false), sf.sig_bytes) != sf.mem_sig) { sf.stale = true; g_surf_epoch++; }
                g_prof.t_miss += prof_usec() - tf;
            }
        }
        if (it != g_surfaces.end() && it->second.tex && !it->second.stale) {
            *is_surface = true;
            if (base_fmt == 0x81) *depth_func = 12;
            resolve_for(it->second.tex);
            return it->second.tex;
        }
    }
    u32 ea = location_to_ea(offset, main_mem);
    u32 bpp = 4;
    switch (base_fmt) {
        case 0x81: bpp = 1; break;
        case 0x82: case 0x83: case 0x84: case 0x8B: case 0x8F: case 0x97: case 0x92: bpp = 2; break;
        default: bpp = 4;
    }
    u32 size = dxt ? ((w + 3) / 4) * ((h + 3) / 4) * (base_fmt == 0x86 ? 8 : 16) : (linear && pitch ? pitch * h : w * h * bpp);
    u64 key = ((u64)ea << 32) ^ ((u64)fmt << 24) ^ ((u64)w << 12) ^ h;
    if (auto it = g_textures.find(key); it != g_textures.end() && it->second.tex) {
        TexEntry& e = it->second;
        if (e.checked_frame == g_frame) return e.tex;
        // Textures that have not changed for 60 frames are not read in full every frame (~60 MB of track textures at the
        // grid): over 64 KB a full check every 32 frames (staggered), a word per KB every 4 frames in between and nothing
        // on the others; 16-64 KB a word per 256 bytes on 7 frames of 8. A texture streamed into the same memory rewrites
        // nearly every line, so the sparse words see it.
        if (!linear && size > 16384 && g_frame - e.last_change > 60) {
            const bool big = size > 65536;
            const u64 phase = (g_frame + key) % (big ? 32 : 8);
            if (phase != 0) {
                if (big && phase % 4 != 0) { e.checked_frame = g_frame; return e.tex; }
                if (e.sentinel) {
                    u64 tth = prof_usec();
                    bool same = (hash_range(gmem(ea), size, big ? 1024 : 256) | 1) == e.sentinel;
                    g_prof.t_texhash += prof_usec() - tth;
                    if (same) { e.checked_frame = g_frame; return e.tex; }
                }
            }
        }
    }
    u32 step = size > 65536 ? 64 : 8;
    u64 tth = prof_usec();
    u64 hsh = hash_range(gmem(ea), size, step);
    g_prof.t_texhash += prof_usec() - tth;
    g_prof.tex_hashed += size / step * 8;
    (linear ? g_prof.tex_lin : size > 65536 ? g_prof.tex_big : g_prof.tex_small) += size;
    TexEntry& te = g_textures[key];
    te.checked_frame = g_frame;
    te.sentinel = !linear && size > 16384 ? hash_range(gmem(ea), size, size > 65536 ? 1024 : 256) | 1 : 0;
    if (te.tex && te.hash == hsh) return te.tex;
    te.last_change = g_frame;
    te.hash = hsh;
    u32 levels = std::max(1u, (format >> 16) & 0xF);
    if (linear && !dxt) levels = 1;
    levels = std::min(levels, ilog2(std::max(w, h)) + 1);
    // a new image each time: draws already recorded in this frame keep sampling the old one
    if (te.tex) destroy_image(te.tex);
    VkFormat vf = base_fmt == 0x86 ? VK_FORMAT_BC1_RGBA_UNORM_BLOCK : base_fmt == 0x87 ? VK_FORMAT_BC2_UNORM_BLOCK : base_fmt == 0x88 ? VK_FORMAT_BC3_UNORM_BLOCK : COLOR_FMT;
    te.tex = make_image(w, h, levels, vf, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | (dxt ? 0 : VK_IMAGE_USAGE_STORAGE_BIT));
    te.w = w; te.h = h; te.fmt = fmt;
    u64 tc = prof_usec();
    if (!dxt) {
        u32 total = 0;
        for (u32 l = 0; l < levels; l++) {
            u32 lw = std::max(1u, w >> l), lh = std::max(1u, h >> l);
            total += linear ? (pitch ? pitch * (lh - 1) + lw * bpp : lw * lh * bpp) : lw * lh * bpp;
        }
        decode_levels(te.tex, base_fmt, linear, pitch, bpp, gmem(ea), total, levels);
    } else {
        u32 off = 0;
        for (u32 l = 0; l < levels; l++) {
            u32 lw = std::max(1u, w >> l), lh = std::max(1u, h >> l);
            u32 lsize = ((lw + 3) / 4) * ((lh + 3) / 4) * (base_fmt == 0x86 ? 8 : 16);
            VkBuffer buf;
            ptrdiff_t soff;
            u8* p = staging(lsize, &buf, &soff);
            memcpy(p, gmem(ea + off), lsize);
            VkBufferImageCopy c{};
            c.bufferOffset = (VkDeviceSize)soff;
            c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, l, 0, 1};
            c.imageExtent = {lw, lh, 1};
            vkCmdCopyBufferToImage(g_ucb, buf, te.tex->img, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
            off += lsize;
        }
    }
    g_prof.t_texdec += prof_usec() - tc;
    g_prof.tex_decoded += size;
    return te.tex;
}

// Cube map of texture unit u: see cube_texture in rsx_gl.cpp
std::set<u32> g_cube_faces;
Img* cube_texture(u32 u) {
    u32 base = 0x1A00 + u * 0x20;
    u32 offset = reg(base), rect = reg(base + 0x18);
    u32 w = rect >> 16, h = rect & 0xFFFF;
    if (!w || w != h || w > 4096) return nullptr;
    struct CubeEntry { Img* tex = nullptr; u32 w = 0, pw = 0; u64 seq[6] = {}, checked_seq = ~0ull; };
    static std::unordered_map<u32, CubeEntry> cubes;
    CubeEntry& ce = cubes[offset];
    const u32 pw = scaled(w, std::max(res_scale_x(), res_scale_y()));
    if (!ce.tex || ce.w != w) {
        if (ce.tex) destroy_image(ce.tex);
        ce.tex = make_image(pw, pw, 1, COLOR_FMT, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_SAMPLE_COUNT_1_BIT, true);
        clear_new(ce.tex);
        ce.w = w;
        std::fill(ce.seq, ce.seq + 6, 0);
        ce.checked_seq = ~0ull;
        LOG_I("rsx: cube map at %08x (%ux%u)", offset, w, w);
    }
    if (ce.checked_seq == g_square_write_seq) return ce.tex;
    ce.checked_seq = g_square_write_seq;
    u32 face_bytes = w * w * 4;
    for (u32 f = 0; f < 6; f++) g_cube_faces.insert(offset + f * face_bytes);
    bool copied = false;
    for (u32 f = 0; f < 6; f++) {
        auto it = g_surfaces.find(offset + f * face_bytes);
        if (it == g_surfaces.end() || !it->second.tex || it->second.width != w || it->second.height != w) continue;
        if (it->second.write_seq == ce.seq[f]) continue;
        end_pass();
        const s32 fw = (s32)(scaled(it->second.width, it->second.scale_x)), fh = (s32)(scaled(it->second.height, it->second.scale_y));
        VkImageBlit b{};
        b.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        b.srcOffsets[1] = {fw, fh, 1};
        b.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, f, 1};
        b.dstOffsets[0] = {0, (s32)pw, 0};
        b.dstOffsets[1] = {(s32)pw, 0, 1};
        vkCmdBlitImage(g_cb, it->second.tex->img, VK_IMAGE_LAYOUT_GENERAL, ce.tex->img, VK_IMAGE_LAYOUT_GENERAL, 1, &b, VK_FILTER_LINEAR);
        copied = true;
        ce.seq[f] = it->second.write_seq;
    }
    if (copied) memory_barrier(g_cb);
    return ce.tex;
}

Img* upload_vertex_texture(u32 u) {
    u32 base = 0x0900 + u * 0x20;
    u32 offset = reg(base), format = reg(base + 4), rect = reg(base + 0x18), pitch = reg(base + 0x10) & 0xFFFFF;
    u32 w = rect >> 16, h = rect & 0xFFFF, fmt = (format >> 8) & 0xFF, base_fmt = fmt & ~0x60u;
    bool linear = fmt & 0x20;
    if (!w || !h || w > 4096 || h > 4096) return nullptr;
    u32 comps, bytes;
    switch (base_fmt) {
        case 0x9B: comps = 4; bytes = 4; break;
        case 0x9C: comps = 1; bytes = 4; break;
        case 0x9A: comps = 4; bytes = 2; break;
        case 0x9F: comps = 2; bytes = 2; break;
        default: {
            static std::set<u32> warned;
            if (warned.insert(base_fmt).second) LOG_W("rsx: vertex texture format %02x not supported", base_fmt);
            return nullptr;
        }
    }
    u32 ea = location_to_ea(offset, (format & 3) == 2);
    u32 bpt = comps * bytes, row = linear && pitch ? pitch : w * bpt;
    u64 key = ((u64)ea << 32) ^ ((u64)fmt << 24) ^ ((u64)w << 12) ^ h ^ 0x5A5A000000000000ull;
    if (auto it = g_textures.find(key); it != g_textures.end() && it->second.tex && it->second.checked_frame == g_frame) return it->second.tex;
    u64 hsh = 1469598103934665603ull;
    for (u32 i = 0; i + 4 <= row * h; i += 4) { hsh ^= ld32(ea + i); hsh *= 1099511628211ull; }
    TexEntry& te = g_textures[key];
    te.checked_frame = g_frame;
    if (te.tex && te.hash == hsh) return te.tex;
    te.hash = hsh;
    if (te.tex) destroy_image(te.tex);
    else LOG_I("rsx: vertex texture %u: ea=%08x fmt=%02x %ux%u pitch=%u", u, ea, fmt, w, h, pitch);
    te.tex = make_image(w, h, 1, VK_FORMAT_R32G32B32A32_SFLOAT, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    VkBuffer buf;
    ptrdiff_t soff;
    float* data = (float*)staging(w * h * 16, &buf, &soff);
    u32 lw = ilog2(w), lh = ilog2(h);
    for (u32 y = 0; y < h; y++)
        for (u32 x = 0; x < w; x++) {
            u32 t = linear ? y * row + x * bpt : morton(x, y, lw, lh) * bpt;
            float o[4] = {0, 0, 0, 1};
            for (u32 c = 0; c < comps; c++)
                o[c] = bytes == 4 ? ldf32(ea + t + c * 4) : half_to_float(ld16(ea + t + c * 2));
            memcpy(&data[((size_t)y * w + x) * 4], o, 16);
        }
    VkBufferImageCopy c{};
    c.bufferOffset = (VkDeviceSize)soff;
    c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    c.imageExtent = {w, h, 1};
    vkCmdCopyBufferToImage(g_ucb, buf, te.tex->img, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    te.w = w; te.h = h; te.fmt = fmt;
    return te.tex;
}

VkSamplerAddressMode wrap_mode(u32 v) {
    switch (v) {
        case 1: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
        case 2: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        case 4: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        default: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    }
}

// sampler of unit u for an image (the mapping of the draw loop in rsx_gl.cpp)
u32 unit_sampler(u32 u, bool from_surface, bool depth_image) {
    SamplerKey k{};
    u32 addr = reg(0x1A08 + u * 0x20);
    k.u = wrap_mode(addr & 0xF);
    k.v = wrap_mode((addr >> 8) & 0xF);
    if (from_surface) {
        k.mag = k.min = depth_image ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
        k.mip = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        return get_sampler(k);
    }
    u32 filt = reg(0x1A14 + u * 0x20), ctl0 = reg(0x1A0C + u * 0x20) & 0x7FFFFFF0;
    s32 bias = (s32)(filt << 19) >> 19;
    k.bias = bias / 256.0f;
    k.min_lod = ((ctl0 >> 19) & 0xFFF) / 256.0f;
    k.max_lod = ((ctl0 >> 7) & 0xFFF) / 256.0f;
    static const float aniso_levels[8] = {1, 2, 4, 6, 8, 10, 12, 16};
    k.aniso = aniso_levels[(ctl0 >> 4) & 7];
    // base filter, mip filter, mips used: LINEAR_MIPMAP_LINEAR, NEAREST, LINEAR, NEAREST_MIPMAP_NEAREST,
    // LINEAR_MIPMAP_NEAREST, NEAREST_MIPMAP_LINEAR, LINEAR_MIPMAP_LINEAR, LINEAR_MIPMAP_LINEAR
    static const struct { VkFilter f; VkSamplerMipmapMode m; bool mips; } modes[8] = {
        {VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_LINEAR, true}, {VK_FILTER_NEAREST, VK_SAMPLER_MIPMAP_MODE_NEAREST, false},
        {VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_NEAREST, false}, {VK_FILTER_NEAREST, VK_SAMPLER_MIPMAP_MODE_NEAREST, true},
        {VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_NEAREST, true}, {VK_FILTER_NEAREST, VK_SAMPLER_MIPMAP_MODE_LINEAR, true},
        {VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_LINEAR, true}, {VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_LINEAR, true}};
    auto m = modes[(filt >> 16) & 7];
    bool texel_coords = (reg(0x1A04 + u * 0x20) & 0x4000) != 0;
    if (texel_coords) { m = modes[2]; filt = (filt & ~0x0F000000u) | 0x02000000u; }   // (the crowd's data table)
    k.min = m.f;
    k.mip = m.m;
    k.mag = ((filt >> 24) & 0xF) == 1 ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    if (!m.mips) k.min_lod = k.max_lod = 0;
    if (k.min != VK_FILTER_LINEAR || k.mag != VK_FILTER_LINEAR) k.aniso = 1;
    return get_sampler(k);
}

// ------------------------------------------------------------------ draws
VkBlendFactor blend_factor(u32 f) {
    switch (f) {
        case 0: return VK_BLEND_FACTOR_ZERO;
        case 1: return VK_BLEND_FACTOR_ONE;
        case 0x0300: return VK_BLEND_FACTOR_SRC_COLOR;
        case 0x0301: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        case 0x0302: return VK_BLEND_FACTOR_SRC_ALPHA;
        case 0x0303: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        case 0x0304: return VK_BLEND_FACTOR_DST_ALPHA;
        case 0x0305: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
        case 0x0306: return VK_BLEND_FACTOR_DST_COLOR;
        case 0x0307: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
        case 0x0308: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
        case 0x8001: return VK_BLEND_FACTOR_CONSTANT_COLOR;
        case 0x8002: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
        case 0x8003: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
        case 0x8004: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
        default: return VK_BLEND_FACTOR_ONE;
    }
}
VkBlendOp blend_op(u32 e) {
    switch (e) {
        case 0x8007: return VK_BLEND_OP_MIN;
        case 0x8008: return VK_BLEND_OP_MAX;
        case 0x800A: return VK_BLEND_OP_SUBTRACT;
        case 0x800B: return VK_BLEND_OP_REVERSE_SUBTRACT;
        default: return VK_BLEND_OP_ADD;
    }
}

VkFrontFace g_front = VK_FRONT_FACE_CLOCKWISE;   // kept while culling is off, like glFrontFace
void apply_state(const Surface* target, VkPrimitiveTopology topo) {
    const bool hud = hud_draw(target->offset, reg(0x0310) & 1, reg(0x0A74) & 1);
    float vx = 0, vy = 0, vw = (float)scaled(target->width, target->scale_x), vh = (float)scaled(target->height, target->scale_y);
    if (hud) hud_rect((u32)vw, (u32)vh, &vx, &vy, &vw, &vh);
    // A point on a pixel edge covers the pixel below it in OpenGL (floor) but the one before it in Vulkan, which shifted
    // the per-car lighting table (32 points at integer x) by one entry: the last car of the grid got an unwritten texel
    // (blue). A quarter-pixel shift gives the OpenGL pixel for points at integer and half-integer positions.
    if (topo == VK_PRIMITIVE_TOPOLOGY_POINT_LIST) { vx += 0.25f; vy += 0.25f; }
    set_viewport(vx, vy, vw, vh);
    set_scissor(target_scissor(target, hud));
    const bool v = g_dyn.valid;
    VkCullModeFlags cull = VK_CULL_MODE_NONE;
    if (reg(0x183C) & 1) {
        u32 cf = reg(0x1830);
        cull = cf == 0x0404 ? VK_CULL_MODE_FRONT_BIT : cf == 0x0408 ? VK_CULL_MODE_FRONT_AND_BACK : VK_CULL_MODE_BACK_BIT;
        bool flipped = !((reg(0x1D88) >> 12) & 1);
        bool cw = reg(0x1834) == 0x0900;
        // (same rows as OpenGL but the opposite area sign: GL_CCW is VK_FRONT_FACE_CLOCKWISE)
        g_front = (flipped ? !cw : cw) ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
    }
    if (!v || g_dyn.cull != cull) { vkCmdSetCullMode(g_cb, cull); g_dyn.cull = cull; }
    if (!v || g_dyn.front != g_front) { vkCmdSetFrontFace(g_cb, g_front); g_dyn.front = g_front; }
    if (!v || g_dyn.topo != topo) { vkCmdSetPrimitiveTopology(g_cb, topo); g_dyn.topo = topo; }
    bool list = topo == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST || topo == VK_PRIMITIVE_TOPOLOGY_LINE_LIST || topo == VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    VkBool32 restart = !list || g_list_restart;
    if (!v || g_dyn.restart != restart) { vkCmdSetPrimitiveRestartEnable(g_cb, restart); g_dyn.restart = restart; }
    bool has_depth = g_cur_depth || target->ms_active;
    VkBool32 dt = has_depth && (reg(0x0A74) & 1), dw = has_depth && (reg(0x0A70) & 1);
    VkCompareOp dop = (VkCompareOp)(reg(0x0A6C) ? (reg(0x0A6C) - 0x200) & 7 : 1);
    if (!v || g_dyn.dt != dt) { vkCmdSetDepthTestEnable(g_cb, dt); g_dyn.dt = dt; }
    if (!v || g_dyn.dw != dw) { vkCmdSetDepthWriteEnable(g_cb, dw); g_dyn.dw = dw; }
    if (!v || g_dyn.dop != dop) { vkCmdSetDepthCompareOp(g_cb, dop); g_dyn.dop = dop; }
    VkBool32 blend = reg(0x0310) & 1;
    if (!v || g_dyn.blend != blend) { vkCmdSetColorBlendEnableEXT(g_cb, 0, 1, &blend); g_dyn.blend = blend; }
    if (blend) {
        u32 sf = reg(0x0314), df = reg(0x0318), eq = reg(0x0320), bc = reg(0x031C);
        VkColorBlendEquationEXT e{blend_factor(sf & 0xFFFF), blend_factor(df & 0xFFFF), blend_op(eq & 0xFFFF),
                                  blend_factor(sf >> 16), blend_factor(df >> 16), blend_op(eq >> 16)};
        if (!v || memcmp(&g_dyn.eq, &e, sizeof(e))) { vkCmdSetColorBlendEquationEXT(g_cb, 0, 1, &e); g_dyn.eq = e; }
        const float c[4] = {((bc >> 16) & 255) / 255.0f, ((bc >> 8) & 255) / 255.0f, (bc & 255) / 255.0f, (bc >> 24) / 255.0f};
        if (!v || memcmp(g_dyn.bc, c, sizeof(c))) { vkCmdSetBlendConstants(g_cb, c); memcpy(g_dyn.bc, c, sizeof(c)); }
    } else if (!v) {
        VkColorBlendEquationEXT e{VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD};
        vkCmdSetColorBlendEquationEXT(g_cb, 0, 1, &e);
        g_dyn.eq = e;
        const float c[4] = {};
        vkCmdSetBlendConstants(g_cb, c);
        memcpy(g_dyn.bc, c, sizeof(c));
    }
    u32 cm = reg(0x0324);
    VkColorComponentFlags mask = ((cm >> 16) & 0xFF ? VK_COLOR_COMPONENT_R_BIT : 0) | ((cm >> 8) & 0xFF ? VK_COLOR_COMPONENT_G_BIT : 0) |
                                 (cm & 0xFF ? VK_COLOR_COMPONENT_B_BIT : 0) | ((cm >> 24) & 0xFF ? VK_COLOR_COMPONENT_A_BIT : 0);
    if (!v || g_dyn.mask != mask) { vkCmdSetColorWriteMaskEXT(g_cb, 0, 1, &mask); g_dyn.mask = mask; }
    VkBool32 a2c = target->ms_active && (reg(0x1D7C) & 0x11) == 0x11;
    if (!v || g_dyn.a2c != a2c) { vkCmdSetAlphaToCoverageEnableEXT(g_cb, a2c); g_dyn.a2c = a2c; }
    VkSampleCountFlagBits samples = target->ms_active ? msaa_samples() : VK_SAMPLE_COUNT_1_BIT;
    if (!v || g_dyn.samples != samples) { vkCmdSetRasterizationSamplesEXT(g_cb, samples); g_dyn.samples = samples; }
    g_dyn.valid = true;
}

void bind_sets() {
    vkCmdBindDescriptorSets(g_cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_pl, 1, 1, &g_set1, 0, nullptr);
    vkCmdBindIndexBuffer(g_cb, g_ibuf.buf, 0, VK_INDEX_TYPE_UINT32);
    g_dyn.set0[0] = ~0u;
}

u64 g_draws = 0;
// the last copies in the uniform ring: valid while the ring stays in their segment (it is reused after a full turn)
u32 g_vc_ver = 0;          // vp_ver of the copied constants
ptrdiff_t g_vc_off = 0;
u32 g_vc_seg = ~0u;
VkDrawParams g_last_params;
ptrdiff_t g_params_off = 0;
u32 g_params_seg = ~0u;

void draw(bool indexed) {
    u64 t0 = prof_usec();
    Surface* target = bind_target();
    if (!target || cube_face_skipped(g_cube_faces, target->offset, g_frame)) return;
    static std::vector<float> fp_consts;
    Program* p = get_program(fp_consts);
    u64 t1 = prof_usec();
    g_prof.program += t1 - t0;
    if (g_prim < 1 || g_prim > 10) return;
    static const VkPrimitiveTopology topos[11] = {
        VK_PRIMITIVE_TOPOLOGY_POINT_LIST, VK_PRIMITIVE_TOPOLOGY_POINT_LIST, VK_PRIMITIVE_TOPOLOGY_LINE_LIST, VK_PRIMITIVE_TOPOLOGY_LINE_STRIP,
        VK_PRIMITIVE_TOPOLOGY_LINE_STRIP, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN,
        VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN};
    const VkPrimitiveTopology topo = topos[g_prim];
    const int cls = g_prim == 1 ? 2 : g_prim <= 4 ? 1 : 0;
    if (p->state[cls] != 2) {   // new to the database: built in the background, like a program under OpenGL
        if (p->state[cls] == 0) build_pipeline(p, cls);
        return;
    }

    static thread_local VkDrawParams dp;
    memset(&dp, 0, sizeof(dp));
    bool reads_gamma = false, reads_raw_target = false;   // (the sRGB rule of draw() in rsx_gl.cpp)
    for (float& r : dp.remap) r = (float)0xaae4;
    std::fill(dp.tex_scale, dp.tex_scale + 32, 1.0f);
    static int stale_slot[16] = {};   // a disabled unit keeps its last texture, as an OpenGL binding does
    u64 tu0 = prof_usec();
    for (u32 u = 0; u < 16; u++) {
        dp.slot[u] = stale_slot[u];
        if (!(reg(0x1A0C + u * 0x20) & 0x80000000)) continue;
        u32 conv = 0;
        {
            u32 a = reg(0x1A08 + u * 0x20);
            conv |= (a >> 20) & 0xF;
            if ((conv & 0x7) && (p->tex_used >> u & 1)) reads_gamma = true;
            if (((a >> 12) & 0xF) == 1) conv |= 16;
        }
        if (reg(0x1A04 + u * 0x20) & 4) {
            // the game leaves the cube map enabled while it renders that cube's faces: looking at it for a program that does
            // not read it copied the face after nearly every draw of the pass (~440 blits per frame, each ending the render pass)
            if (!(p->tex_used >> u & 1)) continue;
            Img* ct = cube_texture(u);
            if (ct) sampled(ct);
            SamplerKey k{};
            k.mag = k.min = VK_FILTER_LINEAR; k.mip = VK_SAMPLER_MIPMAP_MODE_NEAREST;
            k.u = k.v = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            dp.slot[u] = ct ? (int)tex_slot(ct, get_sampler(k)) : 0;
            dp.remap[u] = (float)((reg(0x1A10 + u * 0x20) & 0xFFFF) | (conv << 16));
            continue;
        }
        bool from_surface;
        int depth_func;
        struct UnitMemo { u32 r[7]; u64 frame = ~0ull; u32 epoch = 0; Img* t = nullptr; bool surf = false; int dfunc = 0; int slot = 0; };
        static UnitMemo memo[16];
        UnitMemo& um = memo[u];
        const u32 ur[7] = {reg(0x1A00 + u * 0x20), reg(0x1A04 + u * 0x20), reg(0x1A08 + u * 0x20), reg(0x1A0C + u * 0x20),
                           reg(0x1A14 + u * 0x20), reg(0x1A18 + u * 0x20), reg(0x1840 + u * 4)};
        bool memo_hit = um.frame == g_frame && um.epoch == g_surf_epoch && memcmp(um.r, ur, sizeof(ur)) == 0;
        Img* t;
        if (memo_hit) {
            g_prof.memo_hits++;
            t = um.t; from_surface = um.surf; depth_func = um.dfunc;
            if (from_surface && t) resolve_for(t);
        } else {
            g_prof.memo_miss++;
            t = upload_texture(u, &from_surface, &depth_func);
            bool ambiguous = depth_func == 9 || (from_surface && (depth_func == 0 || depth_func == 12) && g_depths.count(ur[0]));
            memcpy(um.r, ur, sizeof(ur));
            um.frame = ambiguous ? ~0ull : g_frame;
            um.epoch = g_surf_epoch;
            um.t = t; um.surf = from_surface; um.dfunc = depth_func;
            um.slot = t ? (int)tex_slot(t, unit_sampler(u, from_surface, is_depth(t->fmt))) : 0;
        }
        if (t && from_surface) sampled(t);
        dp.slot[u] = stale_slot[u] = um.slot;
        dp.tex_flip[u] = from_surface ? 1.0f : 0.0f;
        if (from_surface && depth_func == 0 && !(conv & 0xF) && (p->tex_used >> u & 1)) {
            if ((reg(0x1A18 + u * 0x20) >> 16) >= target->width && (reg(0x1A18 + u * 0x20) & 0xFFFF) >= target->height)
                reads_raw_target = true;
            else if (reg(0x0A74) & 1) reads_gamma = true;
        }
        if (reg(0x1A04 + u * 0x20) & 0x4000) {
            u32 rect = reg(0x1A18 + u * 0x20);
            dp.tex_scale[u * 2] = 1.0f / (float)std::max(1u, rect >> 16);
            dp.tex_scale[u * 2 + 1] = 1.0f / (float)std::max(1u, rect & 0xFFFF);
        }
        dp.tex_depth[u] = (float)depth_func;
        if (depth_func >= 1 && depth_func <= 8 && ((reg(0x1A14 + u * 0x20) >> 24) & 0xF) != 1) dp.tex_depth[u] = (float)(depth_func + 16);
        if (depth_func == 0 || depth_func == 12 || depth_func == 9) dp.remap[u] = (float)(reg(0x1A10 + u * 0x20) & 0xFFFF);
        if (depth_func == 0 || depth_func == 12) dp.remap[u] = (float)((u32)dp.remap[u] | (conv << 16));
    }
    g_prof.t_upload += prof_usec() - tu0;
    for (u32 u = 0; u < 4; u++) {
        if (!(reg(0x090C + u * 0x20) & 0x80000000)) continue;
        Img* t = upload_vertex_texture(u);
        SamplerKey k{};
        k.mag = k.min = VK_FILTER_NEAREST; k.mip = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        k.u = k.v = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        dp.vslot[u] = t ? (int)tex_slot(t, get_sampler(k)) : 0;
    }
    u64 t2 = prof_usec();
    g_prof.textures += t2 - t1;

    u64 ti0 = prof_usec();
    DrawGeom geom;
    if (!gather_indices(indexed, geom)) return;
    u64 tat = prof_usec();
    g_prof.t_index += tat - ti0;
    setup_attrs_gpu(geom.min_v, geom.max_v, dp.va);
    u64 tun = prof_usec();
    g_prof.t_pre += tun - tat;

    bool cube_face = g_cube_faces.count(target->offset) != 0;
    bool encode = (reg(0x1FD8) & 1) && reads_gamma && !reads_raw_target && !cube_face;
    memcpy(dp.vp_scale, &R[0x0A20 >> 2], 16);
    memcpy(dp.vp_offset, &R[0x0A30 >> 2], 16);
    dp.surface[0] = (float)target->width;
    dp.surface[1] = (float)target->height;
    dp.surface[2] = (reg(0x1D88) >> 12) & 1 ? 1.0f : 0.0f;
    dp.alpha_test[0] = (reg(0x0304) & 1) ? 1.0f : 0.0f;
    dp.alpha_test[1] = (float)((s32)reg(0x0308) - 0x200);
    dp.alpha_test[2] = (reg(0x030C) & 255) / 255.0f;
    dp.alpha_test[3] = encode ? 1.0f : 0.0f;
    u32 fm = reg(0x08CC);
    dp.fog[0] = fm == 0x0800 ? 1.f : fm == 0x0801 ? 2.f : fm == 0x0802 ? 4.f : fm == 0x0803 ? 5.f : fm == 0x0804 ? 3.f : 0.f;
    dp.fog[1] = regf(0x08D0);
    dp.fog[2] = regf(0x08D4);
    dp.res_scale[0] = target->scale_x;
    dp.res_scale[1] = target->scale_y;
    dp.ui_clamp = (target->scale_x != 1 || target->scale_y != 1) && !(reg(0x0A74) & 1) ? 1.0f : 0.0f;

    // parameters into the uniform ring (the constants only when they changed since the last draw)
    const u32 ua = (u32)g_props.limits.minUniformBufferOffsetAlignment;
    static ptrdiff_t fc_off = 0;
    static u32 fc_seg = ~0u;
    static std::vector<float> last_fc;
    u8* q;
    if (fc_seg != g_uring.pos / g_uring.seg || last_fc != fp_consts) {
        const u32 fc_bytes = (u32)fp_consts.size() * 4;
        q = ring_take(g_uring, fc_bytes, ua, &fc_off);
        memcpy(q, fp_consts.data(), fc_bytes);
        last_fc = fp_consts;
        fc_seg = (u32)(fc_off / g_uring.seg);
    }
    if (g_vc_ver != vp_ver || g_vc_seg != g_uring.pos / g_uring.seg) {
        q = ring_take(g_uring, sizeof(vp_consts), ua, &g_vc_off);
        memcpy(q, vp_consts, sizeof(vp_consts));
        g_vc_ver = vp_ver;
        g_vc_seg = (u32)(g_vc_off / g_uring.seg);
    }
    if (g_params_seg != g_uring.pos / g_uring.seg || memcmp(&g_last_params, &dp, sizeof(dp))) {
        q = ring_take(g_uring, sizeof(dp), ua, &g_params_off);
        memcpy(q, &dp, sizeof(dp));
        memcpy(&g_last_params, &dp, sizeof(dp));
        g_params_seg = (u32)(g_params_off / g_uring.seg);
    }
    begin_pass(target);
    u64 ta0 = prof_usec();
    g_prof.t_bind += ta0 - tun;
    apply_state(target, topo);
    g_prof.t_apply += prof_usec() - ta0;
    if (g_dyn.pipe != p->pipe[cls]) { vkCmdBindPipeline(g_cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p->pipe[cls]); g_dyn.pipe = p->pipe[cls]; }
    const u32 dyn[3] = {(u32)g_vc_off, (u32)g_params_off, (u32)fc_off};
    if (memcmp(g_dyn.set0, dyn, sizeof(dyn))) {
        vkCmdBindDescriptorSets(g_cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_pl, 0, 1, &g_set0, 3, dyn);
        memcpy(g_dyn.set0, dyn, sizeof(dyn));
    }
    u64 tdr = prof_usec();
    vkCmdDrawIndexed(g_cb, geom.idx_count, 1, (u32)(geom.idx_offset / 4), 0, 0);
    g_prof.t_draw += prof_usec() - tdr;
    g_prof.vertices += prof_usec() - t2;
    g_prof.draws++;
    g_prof.verts += geom.max_v - geom.min_v + 1;
    if (target->main_ea) target->wb_dirty = true;
    g_write_seq++;
    if (reg(0x0324)) target->write_seq = g_write_seq;
    if (target->ms_active) target->ms_dirty = true;
    if (reg(0x0324) && target->width == target->height) g_square_write_seq = g_write_seq;
    if (g_cur_depth && (reg(0x0A74) & 1) && (reg(0x0A70) & 1)) g_cur_depth->write_seq = g_write_seq;
    g_draws++;
}

VkPipeline g_clear_pipe = 0;
void clear(u32 mask) {
    Surface* s = bind_target();
    if (!s || cube_face_skipped(g_cube_faces, s->offset, g_frame)) return;
    if (s->ms_active) s->ms_dirty = true;
    begin_pass(s);
    VkRect2D sc = target_scissor(s);
    VkClearRect rect{sc, 0, 1};
    VkClearAttachment att[2]{};
    u32 n = 0;
    u32 c = reg(0x1D90);
    const float col[4] = {((c >> 16) & 255) / 255.0f, ((c >> 8) & 255) / 255.0f, (c & 255) / 255.0f, (c >> 24) / 255.0f};
    bool partial = (mask & 0xF0) && (mask & 0xF0) != 0xF0;
    if ((mask & 0xF0) && !partial) {
        att[n].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        memcpy(att[n].clearValue.color.float32, col, sizeof(col));
        n++;
    }
    if (mask & 3) {
        att[n].aspectMask = ((mask & 1) ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) | ((mask & 2) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
        att[n].clearValue.depthStencil = {(float)((reg(0x1D8C) >> 8) / 16777215.0), reg(0x1D8C) & 0xFF};
        if (g_cur_depth || s->ms_active) n++;
    }
    if (n && sc.extent.width && sc.extent.height) vkCmdClearAttachments(g_cb, n, att, 1, &rect);
    if (partial && sc.extent.width && sc.extent.height) {   // some channels: clear attachments ignore write masks
        const u32 saved_cm = R[0x0324 >> 2], saved_be = R[0x0310 >> 2], saved_dt = R[0x0A74 >> 2], saved_cull = R[0x183C >> 2];
        R[0x0324 >> 2] = ((mask >> 4) & 1 ? 0x00FF0000u : 0) | ((mask >> 5) & 1 ? 0x0000FF00u : 0) | ((mask >> 6) & 1 ? 0x000000FFu : 0) | ((mask >> 7) & 1 ? 0xFF000000u : 0);
        R[0x0310 >> 2] = 0; R[0x0A74 >> 2] = 0; R[0x183C >> 2] = 0;
        apply_state(s, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
        vkCmdBindPipeline(g_cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_clear_pipe);
        g_dyn.pipe = g_clear_pipe;
        vkCmdPushConstants(g_cb, g_pl, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(col), col);
        vkCmdDraw(g_cb, 3, 1, 0, 0);
        R[0x0324 >> 2] = saved_cm; R[0x0310 >> 2] = saved_be; R[0x0A74 >> 2] = saved_dt; R[0x183C >> 2] = saved_cull;
    }
    if (s->main_ea) s->wb_dirty = true;
    g_write_seq++;
    if (mask & 0xF0) s->write_seq = g_write_seq;
    if ((mask & 0xF0) && s->width == s->height) g_square_write_seq = g_write_seq;
    if ((mask & 3) && g_cur_depth) g_cur_depth->write_seq = g_write_seq;
}

// ------------------------------------------------------------------ occlusion counting
// As in rsx_gl.cpp, in spans; a span cannot cross a render pass here, so one counted span may be several queries.
constexpr u32 QRES = 1u << 16;
std::vector<u32> g_qres(QRES);
u64 g_qnext = 1;
struct ZPassState {
    std::vector<u64> spans;   // query ids since the last clear
    struct Pending { u32 ea; std::vector<u64> spans; u64 serial; };
    std::deque<Pending> pending;
} g_zpass;

void slot_read_queries(Slot& s) {
    if (s.qread) return;
    std::vector<u64> r(s.qused);
    if (s.qused) {
        VkResult res = vkGetQueryPoolResults(g_dev, s.qpool, 0, s.qused, r.size() * 8, r.data(), 8, VK_QUERY_RESULT_64_BIT);
        if (res != VK_SUCCESS) LOG_W("rsx: query results: %d", (int)res);
        for (u32 i = 0; i < s.qused; i++) g_qres[(s.qfirst + i) & (QRES - 1)] = (u32)r[i];
    }
    s.qread = true;
}
void q_begin() {
    Slot& s = g_slots[g_slot];
    if (!g_q.enabled || !g_pass.on || g_q.open) return;
    if (s.qused >= QUERIES) { LOG_W("rsx: out of occlusion queries in a frame"); return; }
    if (!s.qused) s.qfirst = g_qnext;
    vkCmdBeginQuery(g_cb, s.qpool, s.qused, VK_QUERY_CONTROL_PRECISE_BIT);
    s.qused++;
    g_q.id = g_qnext++;
    g_q.open = true;
}
void q_end() {
    if (!g_q.open) return;
    vkCmdEndQuery(g_cb, g_slots[g_slot].qpool, g_slots[g_slot].qused - 1);
    g_zpass.spans.push_back(g_q.id);
    g_q.open = false;
}
void zpass_enable(bool on) {
    if (on == g_q.enabled) return;
    if (on) { g_q.enabled = true; q_begin(); }
    else { q_end(); g_q.enabled = false; }
}
void zpass_resolve() {
    for (Slot& s : g_slots) if (!s.qread && s.serial && s.serial <= g_completed) slot_read_queries(s);
    while (!g_zpass.pending.empty() && g_zpass.pending.front().serial <= g_completed) {
        auto& p = g_zpass.pending.front();
        u32 total = 0;
        for (u64 id : p.spans) total += g_qres[id & (QRES - 1)];
        total = (u32)(total / (res_scale_x() * res_scale_y()));
        st32(p.ea + 8, total);
        g_zpass.pending.pop_front();
    }
}

// ------------------------------------------------------------------ command buffers
void begin_slot() {
    Slot& s = g_slots[g_slot];
    if (s.serial) wait_serial(s.serial);
    slot_read_queries(s);
    s.serial = 0;   // (recording: zpass_resolve must not read its queries)
    check(vkResetCommandPool(g_dev, s.pool, 0), "vkResetCommandPool");
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vkBeginCommandBuffer(s.ucb, &bi), "vkBeginCommandBuffer");
    check(vkBeginCommandBuffer(s.cb, &bi), "vkBeginCommandBuffer");
    g_cb = s.cb;
    g_ucb = s.ucb;
    vkCmdResetQueryPool(s.ucb, s.qpool, 0, QUERIES);
    s.qused = 0;
    s.qread = false;
    g_dyn.valid = false;
    g_dyn.pipe = 0;
    bind_sets();
}

void submit(VkSemaphore wait, VkSemaphore signal) {
    end_pass();
    Slot& s = g_slots[g_slot];
    memory_barrier(s.ucb);
    check(vkEndCommandBuffer(s.ucb), "vkEndCommandBuffer");
    check(vkEndCommandBuffer(s.cb), "vkEndCommandBuffer");
    VkCommandBufferSubmitInfo cbs[2]{{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO}, {VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO}};
    cbs[0].commandBuffer = s.ucb;
    cbs[1].commandBuffer = s.cb;
    VkSemaphoreSubmitInfo sig[2]{{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO}, {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO}};
    sig[0].semaphore = g_timeline;
    sig[0].value = g_serial;
    sig[0].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    sig[1].semaphore = signal;
    sig[1].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkSemaphoreSubmitInfo wt{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    wt.semaphore = wait;
    wt.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    si.commandBufferInfoCount = 2;
    si.pCommandBufferInfos = cbs;
    si.signalSemaphoreInfoCount = signal ? 2 : 1;
    si.pSignalSemaphoreInfos = sig;
    si.waitSemaphoreInfoCount = wait ? 1 : 0;
    si.pWaitSemaphoreInfos = &wt;
    check(vkQueueSubmit2(g_queue, 1, &si, 0), "vkQueueSubmit2");
    s.serial = g_serial;
    g_serial++;
    g_slot = (g_slot + 1) % SLOTS;
    completed();
    begin_slot();
    run_dead();
}

// ------------------------------------------------------------------ swapchain
VkSwapchainKHR g_swap = 0;
std::vector<VkImage> g_swap_img;
std::vector<VkImageView> g_swap_view;
std::vector<VkSemaphore> g_swap_done;
VkSemaphore g_acquire[8];
u32 g_acquire_i = 0;
VkSurfaceFormatKHR g_swap_fmt;
VkExtent2D g_swap_ext{};

bool create_swapchain() {
    if (host::quit_requested()) return false;   // the window goes first while the game shuts down
    VkSurfaceCapabilitiesKHR caps;
    check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_phys, g_surface, &caps), "surface capabilities");
    VkExtent2D ext = caps.currentExtent;
    if (ext.width == ~0u) {
        int w = 1280, h = 720;
        host::gl_drawable_size(&w, &h);
        ext = {(u32)w, (u32)h};
    }
    if (!ext.width || !ext.height) return false;   // minimised
    u32 nf = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(g_phys, g_surface, &nf, nullptr);
    std::vector<VkSurfaceFormatKHR> fmts(nf);
    vkGetPhysicalDeviceSurfaceFormatsKHR(g_phys, g_surface, &nf, fmts.data());
    if (!g_swap_fmt.format) {
        g_swap_fmt = fmts[0];
        for (auto& f : fmts)   // raw values: the game's output is already encoded
            if (f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) { g_swap_fmt = f; break; }
    }
    u32 nm = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(g_phys, g_surface, &nm, nullptr);
    std::vector<VkPresentModeKHR> modes(nm);
    vkGetPhysicalDeviceSurfacePresentModesKHR(g_phys, g_surface, &nm, modes.data());
    auto has = [&](VkPresentModeKHR m) { return std::find(modes.begin(), modes.end(), m) != modes.end(); };
    // F1CE_VSYNC as in host_sdl.cpp: 1 = wait for vblank, 0 = off, -1 = adaptive
    int vs = getenv("F1CE_VSYNC") ? atoi(getenv("F1CE_VSYNC")) : 1;
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    if (vs == 0) mode = has(VK_PRESENT_MODE_IMMEDIATE_KHR) ? VK_PRESENT_MODE_IMMEDIATE_KHR : has(VK_PRESENT_MODE_MAILBOX_KHR) ? VK_PRESENT_MODE_MAILBOX_KHR : mode;
    if (vs < 0 && has(VK_PRESENT_MODE_FIFO_RELAXED_KHR)) mode = VK_PRESENT_MODE_FIFO_RELAXED_KHR;
    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = g_surface;
    ci.minImageCount = std::max(caps.minImageCount, 3u);
    if (caps.maxImageCount) ci.minImageCount = std::min(ci.minImageCount, caps.maxImageCount);
    ci.imageFormat = g_swap_fmt.format;
    ci.imageColorSpace = g_swap_fmt.colorSpace;
    ci.imageExtent = ext;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode = mode;
    ci.clipped = VK_TRUE;
    VkSwapchainKHR old = g_swap;
    ci.oldSwapchain = old;
    check(vkCreateSwapchainKHR(g_dev, &ci, nullptr, &g_swap), "vkCreateSwapchainKHR");
    if (old) {
        vkQueueWaitIdle(g_queue);
        for (VkImageView v : g_swap_view) vkDestroyImageView(g_dev, v, nullptr);
        vkDestroySwapchainKHR(g_dev, old, nullptr);
    }
    u32 n = 0;
    vkGetSwapchainImagesKHR(g_dev, g_swap, &n, nullptr);
    g_swap_img.resize(n);
    vkGetSwapchainImagesKHR(g_dev, g_swap, &n, g_swap_img.data());
    g_swap_view.clear();
    for (VkImage im : g_swap_img) g_swap_view.push_back(make_view(im, g_swap_fmt.format, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 1));
    VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    while (g_swap_done.size() < n) {
        VkSemaphore s;
        check(vkCreateSemaphore(g_dev, &si, nullptr, &s), "vkCreateSemaphore");
        g_swap_done.push_back(s);
    }
    g_swap_ext = ext;
    LOG_I("rsx: swapchain %ux%u, %u images, format %d, present mode %d", ext.width, ext.height, n, (int)g_swap_fmt.format, (int)mode);
    return true;
}

bool acquire(u32* index, VkSemaphore* sem) {
    if (!g_swap && !create_swapchain()) return false;
    for (int tries = 0; tries < 3; tries++) {
        *sem = g_acquire[g_acquire_i];
        VkResult r = vkAcquireNextImageKHR(g_dev, g_swap, 2000000000ull, *sem, 0, index);
        if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) { g_acquire_i = (g_acquire_i + 1) % 8; return true; }
        if (r != VK_ERROR_OUT_OF_DATE_KHR) { LOG_W("rsx: vkAcquireNextImageKHR: %d", (int)r); return false; }
        if (!create_swapchain()) return false;
    }
    return false;
}
void present(u32 index) {
    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &g_swap_done[index];
    pi.swapchainCount = 1;
    pi.pSwapchains = &g_swap;
    pi.pImageIndices = &index;
    VkResult r = vkQueuePresentKHR(g_queue, &pi);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) create_swapchain();
    else if (r != VK_SUCCESS) LOG_W("rsx: vkQueuePresentKHR: %d", (int)r);
}

// the window area the image goes to (present_rect in rsx_gl.cpp)
void present_rect(const Surface& s, s32 ww, s32 wh, s32* x, s32* y, s32* w, s32* h) {
    static const bool stretch = getenv("F1CE_STRETCH") && atoi(getenv("F1CE_STRETCH")) != 0;
    *x = 0; *y = 0; *w = ww; *h = wh;
    if (stretch || !s.width || !s.height) return;
    double a = present_aspect(s.width, s.height);
    if ((double)ww / wh > a) { *w = (s32)(wh * a + 0.5); *x = (ww - *w) / 2; }
    else { *h = (s32)(ww / a + 0.5); *y = (wh - *h) / 2; }
}

void progress_bar(double frac, const char* what) {
    u32 index;
    VkSemaphore sem;
    if (!acquire(&index, &sem)) return;
    image_barrier(g_cb, g_swap_img[index], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo c{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    c.imageView = g_swap_view[index];
    c.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    c.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    c.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    c.clearValue.color = {{0.05f, 0.05f, 0.06f, 1}};
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, g_swap_ext};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &c;
    vkCmdBeginRendering(g_cb, &ri);
    s32 ww = (s32)g_swap_ext.width, wh = (s32)g_swap_ext.height;
    s32 bw = ww * 6 / 10, bh = std::max(8, wh / 60), bx = (ww - bw) / 2, by = wh / 2 - bh / 2;
    auto bar = [&](s32 x, s32 y, s32 w, s32 h, float r, float g, float b) {
        VkClearAttachment a{};
        a.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        a.clearValue.color = {{r, g, b, 1}};
        VkClearRect cr{{{x, y}, {(u32)w, (u32)h}}, 0, 1};
        vkCmdClearAttachments(g_cb, 1, &a, 1, &cr);
    };
    bar(bx - 2, by - 2, bw + 4, bh + 4, 0.35f, 0.35f, 0.38f);
    bar(bx, by, std::max(1, (s32)(bw * frac)), bh, 0.86f, 0.10f, 0.10f);
    for (const TextRect& t : progress_label(frac, what, ww, wh, by)) bar(t.x, t.y, t.w, t.h, 0.80f, 0.80f, 0.82f);
    vkCmdEndRendering(g_cb);
    image_barrier(g_cb, g_swap_img[index], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    submit(sem, g_swap_done[index]);
    present(index);
}

// Presentation with a scaled image or FXAA (present_pass in rsx_gl.cpp): the display buffer goes to an image with a mip
// chain, sampled at the level that matches the window
bool fxaa_on() { static const bool v = getenv("F1CE_FXAA") && atoi(getenv("F1CE_FXAA")) != 0; return v; }
VkPipeline g_present_pipe = 0;
Img* g_present_img = nullptr;

void present_pass(Surface& s, u32 index) {
    const u32 pw = scaled(s.width, s.scale_x), ph = scaled(s.height, s.scale_y);
    u32 levels = ilog2(std::max(pw, ph)) + 1;
    if (!g_present_img || g_present_img->w != pw || g_present_img->h != ph) {
        if (g_present_img) destroy_image(g_present_img);
        g_present_img = make_image(pw, ph, levels, COLOR_FMT, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    }
    s32 rx, ry, rw, rh;
    present_rect(s, (s32)g_swap_ext.width, (s32)g_swap_ext.height, &rx, &ry, &rw, &rh);
    float level = std::max(0.0f, std::log2(std::min((float)pw / (float)rw, (float)ph / (float)rh)));
    u32 want = std::min(levels, (u32)std::ceil(level) + 1);
    {
        VkImageBlit b{};
        b.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        b.srcOffsets[1] = {(s32)pw, (s32)ph, 1};
        b.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        b.dstOffsets[1] = {(s32)pw, (s32)ph, 1};
        vkCmdBlitImage(g_cb, s.tex->img, VK_IMAGE_LAYOUT_GENERAL, g_present_img->img, VK_IMAGE_LAYOUT_GENERAL, 1, &b, VK_FILTER_NEAREST);
        memory_barrier(g_cb);
        for (u32 l = 1; l < want; l++) {
            b.srcSubresource.mipLevel = l - 1;
            b.srcOffsets[1] = {(s32)std::max(1u, pw >> (l - 1)), (s32)std::max(1u, ph >> (l - 1)), 1};
            b.dstSubresource.mipLevel = l;
            b.dstOffsets[1] = {(s32)std::max(1u, pw >> l), (s32)std::max(1u, ph >> l), 1};
            vkCmdBlitImage(g_cb, g_present_img->img, VK_IMAGE_LAYOUT_GENERAL, g_present_img->img, VK_IMAGE_LAYOUT_GENERAL, 1, &b, VK_FILTER_LINEAR);
            memory_barrier(g_cb);
        }
    }
    SamplerKey k{};
    k.mag = k.min = VK_FILTER_LINEAR; k.mip = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    k.u = k.v = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    k.max_lod = 15.0f;
    u32 slot = tex_slot(g_present_img, get_sampler(k));
    float lw = std::max(1.0f, std::floor(pw / std::exp2(std::floor(level)))), lh = std::max(1.0f, std::floor(ph / std::exp2(std::floor(level))));
    image_barrier(g_cb, g_swap_img[index], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo c{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    c.imageView = g_swap_view[index];
    c.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    c.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    c.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    c.clearValue.color = {{0, 0, 0, 1}};
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, g_swap_ext};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &c;
    vkCmdBeginRendering(g_cb, &ri);
    VkViewport vp{(float)rx, (float)ry, (float)rw, (float)rh, 0, 1};
    VkRect2D sc{{0, 0}, g_swap_ext};
    vkCmdSetViewportWithCount(g_cb, 1, &vp);
    vkCmdSetScissorWithCount(g_cb, 1, &sc);
    vkCmdSetCullMode(g_cb, VK_CULL_MODE_NONE);
    vkCmdSetFrontFace(g_cb, VK_FRONT_FACE_CLOCKWISE);
    vkCmdSetPrimitiveTopology(g_cb, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    vkCmdSetPrimitiveRestartEnable(g_cb, VK_FALSE);
    vkCmdSetDepthTestEnable(g_cb, VK_FALSE);
    vkCmdSetDepthWriteEnable(g_cb, VK_FALSE);
    vkCmdSetDepthCompareOp(g_cb, VK_COMPARE_OP_ALWAYS);
    const float bc[4] = {};
    vkCmdSetBlendConstants(g_cb, bc);
    VkBool32 off = VK_FALSE;
    vkCmdSetColorBlendEnableEXT(g_cb, 0, 1, &off);
    VkColorBlendEquationEXT e{VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD};
    vkCmdSetColorBlendEquationEXT(g_cb, 0, 1, &e);
    VkColorComponentFlags m = 0xF;
    vkCmdSetColorWriteMaskEXT(g_cb, 0, 1, &m);
    vkCmdSetAlphaToCoverageEnableEXT(g_cb, VK_FALSE);
    vkCmdSetRasterizationSamplesEXT(g_cb, VK_SAMPLE_COUNT_1_BIT);
    vkCmdBindPipeline(g_cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_present_pipe);
    // u_win.w < 0: the window's y runs against the image's
    struct { float p[4], win[4]; int slot; } pc = {{level, fxaa_on() ? 1.0f : 0.0f, 1.0f / lw, 1.0f / lh}, {(float)rx, (float)(ry + rh), (float)rw, -(float)rh}, (int)slot};
    vkCmdPushConstants(g_cb, g_pl, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
    vkCmdDraw(g_cb, 3, 1, 0, 0);
    vkCmdEndRendering(g_cb);
    g_dyn.valid = false;
    g_dyn.pipe = 0;
    image_barrier(g_cb, g_swap_img[index], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
}

void present_blit(Surface* s, u32 index) {
    image_barrier(g_cb, g_swap_img[index], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkClearColorValue black{{0, 0, 0, 1}};
    VkImageSubresourceRange all{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(g_cb, g_swap_img[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &all);
    if (s) {
        s32 rx, ry, rw, rh;
        present_rect(*s, (s32)g_swap_ext.width, (s32)g_swap_ext.height, &rx, &ry, &rw, &rh);
        memory_barrier(g_cb);
        VkImageBlit b{};
        b.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        b.srcOffsets[0] = {0, (s32)(scaled(s->height, s->scale_y)), 0};   // (surfaces are stored bottom-up)
        b.srcOffsets[1] = {(s32)(scaled(s->width, s->scale_x)), 0, 1};
        b.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        b.dstOffsets[0] = {rx, ry, 0};
        b.dstOffsets[1] = {rx + rw, ry + rh, 1};
        vkCmdBlitImage(g_cb, s->tex->img, VK_IMAGE_LAYOUT_GENERAL, g_swap_img[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, VK_FILTER_LINEAR);
    }
    image_barrier(g_cb, g_swap_img[index], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
}

// ------------------------------------------------------------------ set-up
void create_device() {
    auto get = (PFN_vkGetInstanceProcAddr)host::vk_begin();
    if (!get) fatal("no Vulkan library or window");
    vkCreateInstance = (PFN_vkCreateInstance)get(nullptr, "vkCreateInstance");
    vkEnumerateInstanceVersion = (PFN_vkEnumerateInstanceVersion)get(nullptr, "vkEnumerateInstanceVersion");
    if (!vkCreateInstance || !vkEnumerateInstanceVersion) fatal("Vulkan 1.1 loader required");
    u32 next = 0;
    const char* const* exts = host::vk_instance_extensions(&next);
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "f1ce";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = next;
    ici.ppEnabledExtensionNames = exts;
    const char* validation = "VK_LAYER_KHRONOS_validation";
    if (getenv("F1CE_VK_VALIDATION")) { ici.enabledLayerCount = 1; ici.ppEnabledLayerNames = &validation; }
    check(vkCreateInstance(&ici, nullptr, &g_inst), "vkCreateInstance");
#define X(name) name = (PFN_##name)get(g_inst, #name); if (!name) fatal("missing " #name);
    VK_INSTANCE_FUNCS(X)
#undef X
    g_surface = (VkSurfaceKHR)host::vk_create_surface(g_inst);
    if (!g_surface) fatal("no window surface");
    u32 n = 0;
    vkEnumeratePhysicalDevices(g_inst, &n, nullptr);
    std::vector<VkPhysicalDevice> devs(n);
    vkEnumeratePhysicalDevices(g_inst, &n, devs.data());
    int best = -1;
    for (VkPhysicalDevice d : devs) {   // a discrete GPU with Vulkan 1.3, else the first with it
        VkPhysicalDeviceProperties pr;
        vkGetPhysicalDeviceProperties(d, &pr);
        if (pr.apiVersion < VK_API_VERSION_1_3) continue;
        int score = pr.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2 : 1;
        if (score > best) { best = score; g_phys = d; g_props = pr; }
    }
    if (!g_phys) fatal("no GPU with Vulkan 1.3");
    vkGetPhysicalDeviceMemoryProperties(g_phys, &g_memprops);
    vkGetPhysicalDeviceQueueFamilyProperties(g_phys, &n, nullptr);
    std::vector<VkQueueFamilyProperties> qf(n);
    vkGetPhysicalDeviceQueueFamilyProperties(g_phys, &n, qf.data());
    bool found = false;
    for (u32 i = 0; i < n && !found; i++) {
        VkBool32 pres = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(g_phys, i, g_surface, &pres);
        if (pres && (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && (qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT)) { g_qfam = i; found = true; }
    }
    if (!found) fatal("no queue that draws and presents");

    // features
    VkPhysicalDeviceExtendedDynamicState3FeaturesEXT eds3{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT};
    VkPhysicalDevicePrimitiveTopologyListRestartFeaturesEXT plr{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIMITIVE_TOPOLOGY_LIST_RESTART_FEATURES_EXT};
    plr.pNext = &eds3;
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    f13.pNext = &plr;
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    f12.pNext = &f13;
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    f2.pNext = &f12;
    vkGetPhysicalDeviceFeatures2(g_phys, &f2);
#define NEED(v) if (!(v)) fatal("the GPU lacks " #v);
    NEED(f13.dynamicRendering) NEED(f13.synchronization2) NEED(f12.timelineSemaphore) NEED(f12.scalarBlockLayout)
    NEED(f12.runtimeDescriptorArray) NEED(f12.descriptorBindingPartiallyBound) NEED(f12.descriptorBindingSampledImageUpdateAfterBind)
    NEED(f12.descriptorBindingUpdateUnusedWhilePending) NEED(f2.features.textureCompressionBC) NEED(f2.features.shaderSampledImageArrayDynamicIndexing)
    NEED(eds3.extendedDynamicState3ColorBlendEnable) NEED(eds3.extendedDynamicState3ColorBlendEquation) NEED(eds3.extendedDynamicState3ColorWriteMask)
    NEED(eds3.extendedDynamicState3AlphaToCoverageEnable) NEED(eds3.extendedDynamicState3RasterizationSamples)
#undef NEED
    u32 nx = 0;
    vkEnumerateDeviceExtensionProperties(g_phys, nullptr, &nx, nullptr);
    std::vector<VkExtensionProperties> xp(nx);
    vkEnumerateDeviceExtensionProperties(g_phys, nullptr, &nx, xp.data());
    auto ext = [&](const char* x) { for (auto& e : xp) if (!strcmp(e.extensionName, x)) return true; return false; };
    std::vector<const char*> dev_exts = {VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME};
    for (const char* x : dev_exts) if (!ext(x)) { char b[128]; snprintf(b, sizeof(b), "the GPU lacks %s", x); fatal(b); }
    g_list_restart = ext(VK_EXT_PRIMITIVE_TOPOLOGY_LIST_RESTART_EXTENSION_NAME) && plr.primitiveTopologyListRestart;
    if (g_list_restart) dev_exts.push_back(VK_EXT_PRIMITIVE_TOPOLOGY_LIST_RESTART_EXTENSION_NAME);

    // what is enabled: only the features used
    VkPhysicalDeviceExtendedDynamicState3FeaturesEXT on3{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT};
    on3.extendedDynamicState3ColorBlendEnable = on3.extendedDynamicState3ColorBlendEquation = on3.extendedDynamicState3ColorWriteMask = VK_TRUE;
    on3.extendedDynamicState3AlphaToCoverageEnable = on3.extendedDynamicState3RasterizationSamples = VK_TRUE;
    VkPhysicalDevicePrimitiveTopologyListRestartFeaturesEXT onr{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIMITIVE_TOPOLOGY_LIST_RESTART_FEATURES_EXT};
    onr.primitiveTopologyListRestart = g_list_restart;
    onr.pNext = &on3;
    VkPhysicalDeviceVulkan13Features on13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    on13.dynamicRendering = on13.synchronization2 = VK_TRUE;
    on13.pNext = &onr;
    VkPhysicalDeviceVulkan12Features on12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    on12.timelineSemaphore = on12.scalarBlockLayout = on12.runtimeDescriptorArray = VK_TRUE;
    on12.descriptorBindingPartiallyBound = on12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
    on12.descriptorBindingUpdateUnusedWhilePending = VK_TRUE;
    on12.pNext = &on13;
    VkPhysicalDeviceFeatures2 on2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    on2.features.textureCompressionBC = on2.features.shaderSampledImageArrayDynamicIndexing = VK_TRUE;
    on2.features.samplerAnisotropy = f2.features.samplerAnisotropy;
    on2.features.occlusionQueryPrecise = f2.features.occlusionQueryPrecise;
    on2.features.independentBlend = VK_FALSE;
    on2.pNext = &on12;
    const float prio = 1.0f;
    VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qi.queueFamilyIndex = g_qfam;
    qi.queueCount = 1;
    qi.pQueuePriorities = &prio;
    VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    di.pNext = &on2;
    di.queueCreateInfoCount = 1;
    di.pQueueCreateInfos = &qi;
    di.enabledExtensionCount = (u32)dev_exts.size();
    di.ppEnabledExtensionNames = dev_exts.data();
    check(vkCreateDevice(g_phys, &di, nullptr, &g_dev), "vkCreateDevice");
#define X(name) name = (PFN_##name)vkGetDeviceProcAddr(g_dev, #name); if (!name) fatal("missing " #name);
    VK_DEVICE_FUNCS(X)
#undef X
    vkGetDeviceQueue(g_dev, g_qfam, 0, &g_queue);

    // depth: D24S8 where the GPU has it, else 32-bit float depth
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(g_phys, VK_FORMAT_D24_UNORM_S8_UINT, &fp);
    constexpr VkFormatFeatureFlags ZF = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    if ((fp.optimalTilingFeatures & ZF) != ZF) g_depth_fmt = VK_FORMAT_D32_SFLOAT_S8_UINT;
    LOG_I("rsx: Vulkan on %s, API %u.%u.%u, depth format %d", g_props.deviceName, VK_API_VERSION_MAJOR(g_props.apiVersion),
          VK_API_VERSION_MINOR(g_props.apiVersion), VK_API_VERSION_PATCH(g_props.apiVersion), (int)g_depth_fmt);
}

void create_objects() {
    VkSemaphoreTypeCreateInfo tt{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    tt.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    si.pNext = &tt;
    check(vkCreateSemaphore(g_dev, &si, nullptr, &g_timeline), "vkCreateSemaphore");
    si.pNext = nullptr;
    for (VkSemaphore& s : g_acquire) check(vkCreateSemaphore(g_dev, &si, nullptr, &s), "vkCreateSemaphore");
    for (Slot& s : g_slots) {
        VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pi.queueFamilyIndex = g_qfam;
        check(vkCreateCommandPool(g_dev, &pi, nullptr, &s.pool), "vkCreateCommandPool");
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = s.pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 2;
        VkCommandBuffer cbs[2];
        check(vkAllocateCommandBuffers(g_dev, &ai, cbs), "vkAllocateCommandBuffers");
        s.cb = cbs[0]; s.ucb = cbs[1];
        VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qi.queryType = VK_QUERY_TYPE_OCCLUSION;
        qi.queryCount = QUERIES;
        check(vkCreateQueryPool(g_dev, &qi, nullptr, &s.qpool), "vkCreateQueryPool");
    }

    // rings and heaps
    constexpr VkBufferUsageFlags SSBO = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    auto ring = [](Ring& r, u32 seg, u32 nseg, VkBufferUsageFlags usage, bool video) {
        r.seg = seg; r.nseg = nseg;
        r.b = make_buffer((VkDeviceSize)seg * nseg + (64u << 10), usage, true, video);   // (a descriptor's range reaches past the last offset)
    };
    ring(g_ring, RING_SEG, 8, SSBO | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, false);
    ring(g_uring, 16u << 20, 16, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true);
    ring(g_sring, 72u << 20, 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | SSBO, false);
    g_heap.size = 256u << 20;
    g_heap_buf = make_buffer(g_heap.size, SSBO | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false, false);
    heap_insert_free(g_heap, 0, g_heap.size);
    g_ibuf = make_buffer(IDX_SIZE, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, true, false);
    g_iheap.ptr = g_ibuf.ptr;
    g_iheap.size = IDX_SIZE;
    heap_insert_free(g_iheap, IDX_RING, IDX_SIZE - IDX_RING);
    g_iring.b = g_ibuf;
    g_iring.seg = IDX_RING / 4;
    g_iring.nseg = 4;
    g_vtx_gpu = true;
    g_tex_gpu = true;

    // descriptors
    VkDescriptorSetLayoutBinding b0[5]{};
    for (u32 i = 0; i < 5; i++) {
        b0[i].binding = i;
        b0[i].descriptorType = i < 3 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        b0[i].descriptorCount = 1;
        b0[i].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo lc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    lc.bindingCount = 5;
    lc.pBindings = b0;
    check(vkCreateDescriptorSetLayout(g_dev, &lc, nullptr, &g_layout0), "vkCreateDescriptorSetLayout");
    VkDescriptorSetLayoutBinding b1[2]{};
    for (u32 i = 0; i < 2; i++) {
        b1[i].binding = i;
        b1[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b1[i].descriptorCount = MAX_SLOTS;
        b1[i].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    const VkDescriptorBindingFlags bf[2] = {
        VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT,
        VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT};
    VkDescriptorSetLayoutBindingFlagsCreateInfo fi{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
    fi.bindingCount = 2;
    fi.pBindingFlags = bf;
    lc.pNext = &fi;
    lc.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    lc.bindingCount = 2;
    lc.pBindings = b1;
    check(vkCreateDescriptorSetLayout(g_dev, &lc, nullptr, &g_layout1), "vkCreateDescriptorSetLayout");
    VkDescriptorPoolSize sizes[3] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 3}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, MAX_SLOTS * 2}};
    VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dp.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    dp.maxSets = 2;
    dp.poolSizeCount = 3;
    dp.pPoolSizes = sizes;
    VkDescriptorPool pool;
    check(vkCreateDescriptorPool(g_dev, &dp, nullptr, &pool), "vkCreateDescriptorPool");
    VkDescriptorSetLayout ls[2] = {g_layout0, g_layout1};
    VkDescriptorSetAllocateInfo da{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    da.descriptorPool = pool;
    da.descriptorSetCount = 2;
    da.pSetLayouts = ls;
    VkDescriptorSet sets[2];
    check(vkAllocateDescriptorSets(g_dev, &da, sets), "vkAllocateDescriptorSets");
    g_set0 = sets[0]; g_set1 = sets[1];
    VkDescriptorBufferInfo bi[5] = {{g_uring.b.buf, 0, sizeof(vp_consts)}, {g_uring.b.buf, 0, sizeof(VkDrawParams)}, {g_uring.b.buf, 0, 16384},
                                    {g_ring.b.buf, 0, VK_WHOLE_SIZE}, {g_heap_buf.buf, 0, VK_WHOLE_SIZE}};
    VkWriteDescriptorSet w[5]{};
    for (u32 i = 0; i < 5; i++) {
        w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[i].dstSet = g_set0;
        w[i].dstBinding = i;
        w[i].descriptorCount = 1;
        w[i].descriptorType = b0[i].descriptorType;
        w[i].pBufferInfo = &bi[i];
    }
    vkUpdateDescriptorSets(g_dev, 5, w, 0, nullptr);

    VkPushConstantRange pr{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 36};
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 2;
    pl.pSetLayouts = ls;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &pr;
    check(vkCreatePipelineLayout(g_dev, &pl, nullptr, &g_pl), "vkCreatePipelineLayout");

    // texture decode pipeline
    VkDescriptorSetLayoutBinding bc[2]{};
    bc[0].binding = 0; bc[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; bc[0].descriptorCount = 1; bc[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bc[1].binding = 1; bc[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; bc[1].descriptorCount = 1; bc[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayoutCreateInfo lcc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    lcc.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    lcc.bindingCount = 2;
    lcc.pBindings = bc;
    check(vkCreateDescriptorSetLayout(g_dev, &lcc, nullptr, &g_layout_cs), "vkCreateDescriptorSetLayout");
    VkPushConstantRange prc{VK_SHADER_STAGE_COMPUTE_BIT, 0, 32};
    VkPipelineLayoutCreateInfo plc{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plc.setLayoutCount = 1;
    plc.pSetLayouts = &g_layout_cs;
    plc.pushConstantRangeCount = 1;
    plc.pPushConstantRanges = &prc;
    check(vkCreatePipelineLayout(g_dev, &plc, nullptr, &g_pl_cs), "vkCreatePipelineLayout");

    // pipeline cache from the last session
    VkPipelineCacheCreateInfo pci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    std::vector<u8> cache;
    if (FILE* f = fopen((vk_cache_dir() + "/pipelines.bin").c_str(), "rb")) {
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (sz > 0) { cache.resize((size_t)sz); if (fread(cache.data(), 1, cache.size(), f) != cache.size()) cache.clear(); }
        fclose(f);
    }
    pci.initialDataSize = cache.size();
    pci.pInitialData = cache.data();
    check(vkCreatePipelineCache(g_dev, &pci, nullptr, &g_pcache), "vkCreatePipelineCache");

    const std::string cs = std::string(
        "#version 450 core\n"
        "layout(local_size_x = 8, local_size_y = 8) in;\n"
        "layout(set = 0, binding = 0, rgba8) writeonly uniform image2D u_dst;\n"
        "layout(std430, set = 0, binding = 1) readonly buffer SRC { uint d[]; } u_src;\n"
        "layout(push_constant) uniform PC { ivec4 u_lvl; ivec4 u_lay; };\n"
        "uint rd8(int a) { return (u_src.d[a >> 2] >> uint((a & 3) * 8)) & 255u; }\n") + tex_decode_glsl();
    VkComputePipelineCreateInfo cpi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cpi.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpi.stage.module = shader_module(2, cs);
    cpi.stage.pName = "main";
    cpi.layout = g_pl_cs;
    if (!cpi.stage.module) fatal("texture decode shader");
    check(vkCreateComputePipelines(g_dev, g_pcache, 1, &cpi, nullptr, &g_tex_cs), "vkCreateComputePipelines");

    begin_slot();

    // entry 0 of the texture arrays, and of every sampler lookup: black, linear, clamped
    SamplerKey k{};
    k.mag = k.min = VK_FILTER_LINEAR; k.mip = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    k.u = k.v = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    get_sampler(k);
    g_black = make_image(1, 1, 1, COLOR_FMT, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    clear_new(g_black);
    g_black_cube = make_image(1, 1, 1, COLOR_FMT, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_SAMPLE_COUNT_1_BIT, true);
    clear_new(g_black_cube);
    write_slot(0, false, g_black->view, g_samplers[0]);
    write_slot(0, true, g_black_cube->view, g_samplers[0]);

    // the auxiliary pipelines
    const std::string fs_vs =
        "#version 450 core\nvoid main() { vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2); gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0); }\n";
    const std::string clear_fs =
        "#version 450 core\nlayout(push_constant) uniform PC { vec4 u_col; };\nlayout(location = 0) out vec4 o;\nvoid main() { o = u_col; }\n";
    const std::string pres_fs = std::string(
        "#version 450 core\n#extension GL_EXT_nonuniform_qualifier : require\n"
        "layout(set = 1, binding = 0) uniform sampler2D g_tex[];\n"
        "layout(push_constant) uniform PC { vec4 u_p; vec4 u_win; int u_slot; };\n"
        "layout(location = 0) out vec4 o;\n"
        "#define u_src g_tex[u_slot]\n") + present_glsl();
    VkShaderModule fvs = shader_module(0, fs_vs);
    g_clear_pipe = make_pipeline(fvs, shader_module(1, clear_fs), VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, COLOR_FMT, g_depth_fmt);
    create_swapchain();
    g_present_pipe = make_pipeline(fvs, shader_module(1, pres_fs), VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, g_swap_fmt.format, VK_FORMAT_UNDEFINED);
    if (!g_clear_pipe || !g_present_pipe) fatal("auxiliary pipelines");
}

void vk_init_once() {
    static bool tried = false;
    if (tried) return;
    tried = true;
    g_shader_vk = true;
    g_vtx_gpu = true;
    create_device();
    create_objects();
    precompile_programs();
    host::input_clock_reset();
    start_workers();
    g_vk_ok = true;
    LOG_I("rsx: Vulkan renderer ready");
}
}  // namespace

// ------------------------------------------------------------------ public interface
// Called on the PPU thread before the RSX thread starts: a cold start builds pipelines for ~20 s, and with the RSX
// thread stuck in that the PPU outlasted its 15 s throttle and overran the FIFO on the first frame.
void init() { vk_init_once(); }

void method_2d(u32 sub, u32 mm, u32 arg) {
    if (method_2d_inline(sub, mm, arg)) return;
    Nv0039 xf;
    if (!nv0039_transfer(sub, mm, xf)) return;
    auto [src, dst, len, lines, src_main, dst_main] = xf;
    // render target copied: copy the surface too (the memory copy has stale contents)
    auto it = src_main ? g_surfaces.end() : g_surfaces.find(src);
    if (it == g_surfaces.end() || !it->second.tex || dst_main) return;
    Surface& s = it->second;
    u32 w = std::min(len / 4, s.width), h = std::min(lines, s.height);
    auto dit = g_surfaces.find(dst);
    Surface* d = dit != g_surfaces.end() && dit->second.tex ? &dit->second : get_surface(dst, w, h, s.format);
    u32 cw = std::min(w, d->width), ch = std::min(h, d->height);
    resolve_ms(s);
    end_pass();
    VkImageBlit b{};
    b.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    b.srcOffsets[1] = {(s32)(scaled(cw, s.scale_x)), (s32)(scaled(ch, s.scale_y)), 1};
    b.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    b.dstOffsets[1] = {(s32)(scaled(cw, d->scale_x)), (s32)(scaled(ch, d->scale_y)), 1};
    vkCmdBlitImage(g_cb, s.tex->img, VK_IMAGE_LAYOUT_GENERAL, d->tex->img, VK_IMAGE_LAYOUT_GENERAL, 1, &b, s.scale_x == d->scale_x && s.scale_y == d->scale_y ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
    memory_barrier(g_cb);
    d->last_use = g_frame;
}

bool report(u32 type, u32 ea) {
    if (type != 1 || !g_vk_ok) return false;   // CELL_GCM_ZPASS_PIXEL_CNT
    if (g_q.enabled) { q_end(); q_begin(); }    // the report covers everything drawn so far
    g_zpass.pending.push_back({ea, g_zpass.spans, g_serial});
    return true;
}

// rsx::g_upload_words: as method() for each word of a run on one upload port
void upload_words(u32 m, const u32* a, u32 n, bool non_inc) {
    if (non_inc) R[m >> 2] = a[n - 1];
    else memcpy(&R[m >> 2], a, n * 4);
    if (m < 0x0C00) {
        const u32 k = std::min(n, 512 * 4 - std::min(vp_load_pos, 512u * 4));
        memcpy(&vp_words[vp_load_pos], a, k * 4);
        vp_load_pos += k;
        g_vp_gen++;
        return;
    }
    for (u32 i = 0; i < n && vp_const_pos < 468 * 4; i++) {
        vp_slot_ver[vp_const_pos >> 2] = ++vp_ver;
        memcpy(&vp_consts[vp_const_pos++], &a[i], 4);
    }
}

void method(u32 m, u32 arg) {
    m &= 0xFFFC;
    static const auto special = [] {
        std::array<bool, 0x800> t{};
        for (u32 x : {0x1D7Cu, 0x1D84u, 0x17C8u, 0x1E9Cu, 0x1EFCu, 0x1D94u, 0x1808u, 0x1814u, 0x1824u}) t[x >> 2] = true;
        for (u32 x = 0x0B80; x < 0x0C00; x += 4) t[x >> 2] = true;
        for (u32 x = 0x1F00; x < 0x1F80; x += 4) t[x >> 2] = true;
        return t;
    }();
    static bool inited = false;
    if (__builtin_expect(inited, 1) && m < 0x2000 && !special[m >> 2]) { R[m >> 2] = arg; return; }
    vk_init_once();
    inited = true;
    if (!g_plain_on) {
        for (u32 i = 0; i < 0x800; i++) g_plain[i] = !special[i];
        g_upload_words = upload_words;
        g_plain_on = true;
    }
    if (m == 0x1D7C) { R[m >> 2] = arg; zpass_enable(arg & 1); return; }   // ANTI_ALIASING_CONTROL / ZPASS_PIXEL_COUNT_ENABLE
    if (m == 0x17C8 && arg == 1) {                                         // CLEAR_REPORT_VALUE (ZPASS_PIXEL_CNT)
        bool on = g_q.enabled;
        zpass_enable(false);
        g_zpass.spans.clear();
        zpass_enable(on);
        return;
    }
    R[m >> 2] = arg;
    if (m >> 13) { method_2d(m >> 13, m & 0x1FFF, arg); return; }
    if (m >= 0x0B80 && m < 0x0C00) {
        if (vp_load_pos < 512 * 4) vp_words[vp_load_pos++] = arg;
        g_vp_gen++;
        return;
    }
    if (m >= 0x1F00 && m < 0x1F80) {
        if (vp_const_pos < 468 * 4) {
            vp_slot_ver[vp_const_pos >> 2] = ++vp_ver;
            memcpy(&vp_consts[vp_const_pos++], &arg, 4);
        }
        return;
    }
    switch (m) {
        case 0x1E9C: vp_load_pos = (arg & 511) * 4; break;
        case 0x1EFC: vp_const_pos = (arg % 468) * 4; break;
        case 0x1D94: { u64 t = prof_usec(); clear(arg); g_fe.t_clear += prof_usec() - t; break; }
        case 0x1808:
            if (arg) {
                g_prim = arg;
                g_array_ranges.clear();
                g_index_ranges.clear();
            } else {
                u64 t = prof_usec();
                if (!g_index_ranges.empty()) draw(true);
                else if (!g_array_ranges.empty()) draw(false);
                g_fe.t_draw += prof_usec() - t;
                g_array_ranges.clear();
                g_index_ranges.clear();
            }
            break;
        case 0x1814: g_array_ranges.push_back({arg & 0xFFFFFF, (arg >> 24) + 1}); break;
        case 0x1824: g_index_ranges.push_back({arg & 0xFFFFFF, (arg >> 24) + 1}); break;
        default: break;
    }
}

u64 g_flip_t0 = 0, g_flip_t1 = 0;
void flip(u32 ea, u32 pitch, u32 width, u32 height) {
    g_flip_t0 = time_usec();
    note_display(ea - mem::RSX_LOCAL);
    vk_init_once();
    (void)pitch; (void)width; (void)height;
    resolve_all_ms();
    end_pass();
    static u64 start_us = 0;
    if (!start_us) start_us = time_usec();
    g_frame_targets.clear();
    g_frame++;
    if (g_frame % 300 == 0) LOG_I("rsx flip: display buffer offset %08x", ea - mem::RSX_LOCAL);
    u32 offset = ea - mem::RSX_LOCAL;
    static const char* dump_list = getenv("F1CE_DUMP_SURFACES");
    if (dump_list && g_frame % 300 == 0) {
        std::string l = dump_list;
        size_t pos = 0;
        while (pos < l.size()) {
            size_t e = l.find(',', pos);
            dump_surface((u32)strtoul(l.substr(pos, e == std::string::npos ? std::string::npos : e - pos).c_str(), nullptr, 16), "");
            if (e == std::string::npos) break;
            pos = e + 1;
        }
    }
    auto it = g_surfaces.find(offset);
    Surface* shown = it != g_surfaces.end() && it->second.tex ? &it->second : nullptr;
    if (host::dump_frame_due() && shown) {   // (the image is read back only for frames that are dumped)
        const u32 pw = scaled(shown->width, shown->scale_x), ph = scaled(shown->height, shown->scale_y);
        std::vector<u32> px((size_t)pw * ph), fl(px.size());
        read_image(shown->tex, pw, ph, px.data());
        for (u32 y = 0; y < ph; y++) memcpy(&fl[(size_t)y * pw], &px[(size_t)(ph - 1 - y) * pw], pw * 4);
        host::dump_frame(fl.data(), pw, ph);
    }
    completed();
    zpass_resolve();
    for (auto& [k, sf] : g_surfaces)
        if (sf.main_ea) { write_back_pending(&sf); write_back_start(&sf); }
    g_flip_t1 = time_usec();
    u64 ts = time_usec();
    u32 index = 0;
    VkSemaphore sem = 0;
    heaps_end_frame();
    u64 tp[5] = {ts};
    if (acquire(&index, &sem)) {
        tp[1] = time_usec();
        if (shown && (res_scaled() || fxaa_on())) present_pass(*shown, index);
        else present_blit(shown, index);
        submit(sem, g_swap_done[index]);
        tp[2] = time_usec();
        present(index);
        tp[3] = time_usec();
    } else {
        submit();
    }
    static u64 flip_serial[2] = {};   // at most two frames in flight
    wait_serial(flip_serial[1]);
    tp[4] = time_usec();
    if (tp[4] - g_flip_t0 > 10000 && tp[3]) {   // debug: where a slow flip went
        static int n = 0;
        if (n++ < 200)
            LOG_I("rsx flip slow: %.1f ms | before %.1f heaps %.1f acquire %.1f submit %.1f present %.1f wait %.1f", (tp[4] - g_flip_t0) / 1e3,
                  (g_flip_t1 - g_flip_t0) / 1e3, (ts - g_flip_t1) / 1e3, (tp[1] - ts) / 1e3, (tp[2] - tp[1]) / 1e3, (tp[3] - tp[2]) / 1e3, (tp[4] - tp[3]) / 1e3);
    }
    flip_serial[1] = flip_serial[0];
    flip_serial[0] = g_serial - 1;
    g_prof.other += time_usec() - ts;
    {
        static u64 window_start = time_usec(), window_frames = 0;
        window_frames++;
        if (time_usec() - window_start >= 5000000) {
            u64 now = time_usec(), wall = now - window_start;
            double nf = (double)window_frames;
            window_frames = 0;
            static u64 idle_last = 0;
            u64 idle = g_rsx_idle_us - idle_last;
            idle_last = g_rsx_idle_us;
            LOG_I("perf vtx: hashed %.1f MB vertex + %.1f MB texture data, %.0f indices rebuilt (region grow %.0f changed %.0f, per draw %.0f) | ms: ring wait %.1f (%.0f segments) index %.1f hash %.1f convert+upload %.1f draw call %.1f",
                  g_prof.hashed / nf / 1e6, g_prof.tex_hashed / nf / 1e6, g_prof.idx_built / nf, g_prof.ir_grow / nf, g_prof.ir_changed / nf, g_prof.idx_old / nf,
                  g_prof.t_ringwait / nf / 1e3, g_prof.ring_segs / nf, g_prof.t_index / nf / 1e3, g_prof.t_hash / nf / 1e3, g_prof.t_conv / nf / 1e3, g_prof.t_draw / nf / 1e3);
            LOG_I("perf tex: hash %.2f ms over MB linear %.1f small %.1f large %.1f; surface fingerprints %.2f ms", g_prof.t_texhash / nf / 1e3,
                  g_prof.tex_lin / nf / 1e6, g_prof.tex_small / nf / 1e6, g_prof.tex_big / nf / 1e6, g_prof.t_miss / nf / 1e3);
            LOG_I("perf: %.1f fps | per frame ms: program %.1f textures %.1f (state %.1f uploads %.1f) vertices %.1f swap %.1f, fifo idle %.1f | %llu draws %llu verts",
                  nf * 1e6 / wall, g_prof.program / nf / 1e3, g_prof.textures / nf / 1e3, g_prof.t_apply / nf / 1e3, g_prof.t_upload / nf / 1e3,
                  g_prof.vertices / nf / 1e3, g_prof.other / nf / 1e3, idle / nf / 1e3, (unsigned long long)(g_prof.draws / nf), (unsigned long long)(g_prof.verts / nf));
            LOG_I("perf vertex heap: %.1f MB in use, %zu raw regions, %u failed allocations, %zu blocks pending; %llu pipelines built so far",
                  g_heap.used / 1048576.0, g_raw_regions.size(), g_heap.failed, g_heap.pending.size(), (unsigned long long)g_compiled.load());
            LOG_I("perf front: ms draw calls %.2f (attributes %.2f params+pass %.2f) clears %.2f flip %.2f semaphore waits %.2f | %.0f commands %.0f words, %.0f slow entries (%.0f nops %.0f flow)",
                  g_fe.t_draw / nf / 1e3, g_prof.t_pre / nf / 1e3, g_prof.t_bind / nf / 1e3, g_fe.t_clear / nf / 1e3, g_fe.t_flip / nf / 1e3,
                  g_fe.t_sem / nf / 1e3, g_fe.cmds / nf, g_fe.words / nf, g_fe.entries / nf, g_fe.nops / nf, g_fe.flows / nf);
            g_fe = {};
            g_prof = {};
            window_start = now;
        }
    }
    {   // test aid as in rsx_gl.cpp: F1CE_QUIT_AFTER_3D=<s> saves the shown frame and the 3D target, then exits
        static const double quit_after = getenv("F1CE_QUIT_AFTER_3D") ? atof(getenv("F1CE_QUIT_AFTER_3D")) : -1;
        static u64 last_draws = 0, scene_us = 0;
        u64 frame_draws = g_draws - last_draws;
        g_last_frame_draws = frame_draws;
        last_draws = g_draws;
        if (quit_after >= 0) {
            if (!scene_us && frame_draws >= 800) {
                scene_us = time_usec();
                LOG_I("rsx: 3D scene detected at frame %llu (%llu draws), %.1f s after the first frame", (unsigned long long)g_frame,
                      (unsigned long long)frame_draws, (time_usec() - start_us) / 1e6);
            }
            if (scene_us && time_usec() - scene_us >= (u64)(quit_after * 1e6)) {
                dump_surface(offset, "_quit");
                dump_surface(0x740000, "_quit");
                LOG_I("rsx: F1CE_QUIT_AFTER_3D reached at frame %llu, exiting", (unsigned long long)g_frame);
                if (g_pcache_dirty) save_pipeline_cache();
                process_exit(0);
            }
        }
    }
    if (g_frame % 600 == 1) {
        LOG_I("rsx: frame %llu, %llu draws, %zu programs, %zu textures, %zu surfaces, %zu index buffers", (unsigned long long)g_frame,
              (unsigned long long)g_draws, g_programs.size(), g_textures.size(), g_surfaces.size(), g_idx_cache.size());
        if (g_pcache_dirty) save_pipeline_cache();
    }
    evict_index_caches(60);
    if (g_frame % 60 == 0) evict_raw_regions();
}
}  // namespace vk
}  // namespace rsx
