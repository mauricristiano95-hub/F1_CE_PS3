// Index lists and raw vertex regions, written once for both renderers: this file is included in each one's anonymous
// namespace, after these are defined:
//   u8* ring_alloc(u32 bytes, ptrdiff_t* off)                  space in the stream ring
//   ptrdiff_t idx_stream(const u32* data, u32 n)               a list used by this draw only
//   bool idx_store(const u32* data, u32 n, u32& off, u32& bytes)   a cached list in the index arena
//   void heap_copy_begin(), void heap_copy(ptrdiff_t ring_off, u32 heap_off, u32 n)   stream ring -> vertex heap
//   BlockHeap g_heap, g_iheap; constexpr bool LINE_LOOP_AS_STRIP; constexpr u32 RING_SEG (stream ring segment size)
struct IdxEntry {
    u32 off = ~0u, bytes = 0;   // block in the index arena (~0u: none)
    u64 hash = 0; u32 count = 0, min_v = 0, max_v = 0; u64 last_change = 0, last_frame = 0;
    u32 id_src = 0, id_count = 0, id_flags = 0;
    u64 seen_frame = 0;   // as VtxEntry::seen_frame
    u64 checked_frame = ~0ull;
};
std::unordered_map<u64, IdxEntry> g_idx_cache;   // converted index lists (see draw())
// Index regions: the raw index buffer (byte-swapped, restart index marked) converted over a range that grows to cover
// the draws, which then draw at an offset with base vertex -min_v instead of rebuilding and rebasing their own list
// (sub-ranges of static index buffers change with the camera; ~500k indices were rebuilt per frame)
struct IdxRegion {
    u32 off = ~0u, bytes = 0;   // block in the index arena
    u32 ea = 0, flags = 0, first = 0, count = 0;
    u64 hash = 0, checked_frame = ~0ull, last_frame = 0;
    u32 changes = 0;   // content changes seen: rewritten (ring) buffers go back to per-draw lists
    std::vector<u32> cpu;                                   // converted indices (for min/max of sub-ranges)
    std::unordered_map<u64, std::pair<u32, u32>> minmax;    // (first << 32 | count) -> vertex range
};
std::unordered_map<u64, IdxRegion> g_idx_regions;

// drops entries not drawn for 120 frames from 1/slices of the map's buckets (cursor: the next bucket). The race keeps
// 50-120k index lists: one pass over all of them took 7-15 ms, so a frame missed its vblank every 2 s; called every
// frame with 60 slices, each entry is still looked at once every 60 frames. (erase never rehashes, so the buckets hold
// still during a slice; a rehash between frames only moves the cursor's place)
template <class Map> void evict_slice(Map& m, size_t& cursor, u32 slices) {
    const size_t nb = m.bucket_count();
    if (!nb || m.empty()) return;
    const size_t per = slices <= 1 ? nb : nb / slices + 1;
    static std::vector<u64> dead;
    dead.clear();
    for (size_t i = 0; i < per && i < nb; i++) {
        const size_t b = (cursor + i) % nb;
        for (auto it = m.begin(b); it != m.end(b); ++it)
            if (g_frame - it->second.last_frame > 120) dead.push_back(it->first);
    }
    cursor = (cursor + per) % nb;
    for (u64 k : dead) {
        auto it = m.find(k);
        heap_free(g_iheap, it->second.off, it->second.bytes);
        m.erase(it);
    }
}
void evict_index_caches(u32 slices) {
    static size_t regions_cursor = 0, lists_cursor = 0;
    evict_slice(g_idx_regions, regions_cursor, slices);
    evict_slice(g_idx_cache, lists_cursor, slices);
}

// Raw vertex data per buffer region (vertex pulling): as RegionEntry, but the guest bytes are copied unconverted
// into a heap block. Keyed by base address, stride and element size (the bytes a record's attribute occupies).
struct RawRegion {
    u32 off = ~0u, bytes = 0;   // heap block
    u32 base_ea = 0, stride = 0, elem = 0, first = 0, count = 0;
    u64 hash = 0, checked_frame = ~0ull, last_frame = 0, seen_frame = 0, last_change = 0;
    u64 sentinel = 0;   // sparse hash (one word per KB) taken with the last full hash of a large region; 0: none
};
std::unordered_map<u64, RawRegion> g_raw_regions;
// direct-mapped in front of the map: ~18000 lookups per frame, the map's nodes are scattered (cleared when entries go)
struct RawLookup { u64 key; RawRegion* r; };
RawLookup g_raw_lookup[2048];
RawRegion& raw_region(u64 key) {
    RawLookup& l = g_raw_lookup[key & 2047];
    if (l.key != key || !l.r) l = {key, &g_raw_regions[key]};
    return *l.r;
}

void evict_raw_regions() {
    memset(g_raw_lookup, 0, sizeof(g_raw_lookup));
    for (auto it = g_raw_regions.begin(); it != g_raw_regions.end();)
        if (g_frame - it->second.last_frame > 120) {
            heap_free(g_heap, it->second.off, it->second.bytes);
            it = g_raw_regions.erase(it);
        } else ++it;
}

inline u32 g_first_attr(const u32* group_of, u32 g) {
    for (u32 i = 0; i < 16; i++) if (group_of[i] == g) return i;
    return 0;
}

// sets u_va for the attributes of this draw (vertices min_v .. max_v); the draw then uses base vertex 0
void setup_attrs_gpu(u32 min_v, u32 max_v, int* va) {
    u32 input_mask = reg(0x1FF0), nverts = max_v - min_v + 1;
    memset(va, 0, 64 * sizeof(int));
    struct Memo { u64 key, h; };
    Memo memo[16];
    u32 nmemo = 0;
    auto hash_of = [&](u32 ea, u32 n) {
        u64 memo_key = ((u64)ea << 32) ^ n;
        for (u32 m = 0; m < nmemo; m++) if (memo[m].key == memo_key) return memo[m].h;
        u64 th = prof_usec();
        u64 h = hash_range(gmem(ea), n, n > 65536 ? 64 : 8);
        g_prof.t_hash += prof_usec() - th;
        g_prof.hashed += n;
        if (nmemo < 16) memo[nmemo++] = {memo_key, h};
        return h;
    };
    // raw bytes into the stream ring: offset of the copy, or -1
    auto stream = [&](u32 ea, u32 n) -> long long {
        ptrdiff_t roff;
        u8* rp = ring_alloc(n, &roff);
        if (!rp) return -1;
        u64 tc = prof_usec();
        memcpy(rp, gmem(ea), n);
        g_prof.t_conv += prof_usec() - tc;
        return (long long)roff;
    };
    // Attributes interleaved in one record share a region: the buffer is hashed and copied once, not once per attribute
    // (va_fetch reads bytes at any offset). Same stride, and together they fit in a record.
    u32 group_ea[16], group_end[16], group_of[16];
    u32 ngroups = 0;
    for (u32 i = 0; i < 16; i++) {
        u32 fmt = reg(0x1740 + i * 4);
        u32 type = fmt & 0xF, size = (fmt >> 4) & 0xF, stride = (fmt >> 8) & 0xFF;
        group_of[i] = 16;
        if (!size || !(input_mask & (1u << i)) || !stride || (fmt >> 16) > 1) continue;
        u32 off = reg(0x1680 + i * 4);
        u32 ea = location_to_ea(off & 0x7FFFFFFF, off >> 31);
        u32 end = ea + (type == 6 ? 4 : std::min(size, 4u) * (type == 2 ? 4 : type == 4 || type == 7 ? 1 : 2));
        u32 g = 0;
        for (; g < ngroups; g++) {
            if (((reg(0x1740 + g_first_attr(group_of, g) * 4) >> 8) & 0xFF) != stride) continue;
            if (std::max(end, group_end[g]) - std::min(ea, group_ea[g]) <= stride) break;
        }
        if (g == ngroups) { group_ea[g] = ea; group_end[g] = end; ngroups++; }
        group_ea[g] = std::min(ea, group_ea[g]);
        group_end[g] = std::max(end, group_end[g]);
        group_of[i] = g;
    }
    for (u32 i = 0; i < 16; i++) {
        u32 fmt = reg(0x1740 + i * 4);
        u32 type = fmt & 0xF, size = (fmt >> 4) & 0xF;
        if (!size || !(input_mask & (1u << i))) continue;   // disabled: type 0
        u32 stride = (fmt >> 8) & 0xFF, freq = fmt >> 16;
        bool modulo = ((reg(0x1FC0) >> i) & 1) != 0;
        u32 off = reg(0x1680 + i * 4);
        u32 base_ea = location_to_ea(off & 0x7FFFFFFF, off >> 31);
        u32 elem = type == 6 ? 4 : std::min(size, 4u) * (type == 2 ? 4 : type == 4 || type == 7 ? 1 : 2);
        u32 rmin = min_v, rmax = max_v;
        if (freq > 1) {
            if (modulo) { rmin = 0; rmax = std::min(freq, nverts) - 1; } else { rmin = min_v / freq; rmax = max_v / freq; }
        }
        if (!stride) rmin = rmax = 0;
        u32 src = base_ea + rmin * stride, bytes = (rmax - rmin) * stride + elem;
        long long base = -1;   // byte offset of the attribute's record rmin
        bool in_ring = true;
        const u32 attr_ea = base_ea;
        if (stride && freq <= 1) {
            const u32 base_ea = group_ea[group_of[i]], elem = group_end[group_of[i]] - base_ea;
            u64 key = mix64(((u64)base_ea << 32) ^ (stride << 8) ^ elem) ^ 0x7f4a7c15ull;
            RawRegion& re = raw_region(key);
            if (re.base_ea != base_ea || re.stride != stride || re.elem != elem) {
                heap_free(g_heap, re.off, re.bytes);
                re = RawRegion{};
                re.base_ea = base_ea; re.stride = stride; re.elem = elem;
            }
            re.last_frame = g_frame;
            auto region_bytes = [&](u32 count) { return (count - 1) * stride + elem; };
            auto upload = [&](u32 first, u32 count) {   // a new block with records first .. first+count-1
                u32 n = region_bytes(count), ea = base_ea + first * stride;
                heap_free(g_heap, re.off, re.bytes);
                re.off = heap_alloc(g_heap, n);
                re.bytes = n;
                re.first = first; re.count = count;
                if (re.off == ~0u) return;
                u64 tc = prof_usec();
                heap_copy_begin();
                for (u32 done = 0; done < n;) {   // in pieces of at most one ring segment
                    u32 k = std::min(n - done, RING_SEG);
                    ptrdiff_t roff;
                    u8* rp = ring_alloc(k, &roff);
                    if (!rp) break;
                    memcpy(rp, gmem(ea + done), k);
                    heap_copy(roff, re.off + done, k);
                    done += k;
                }
                g_prof.t_conv += prof_usec() - tc;
                g_prof.conv_verts += count;
            };
            bool covers = re.off != ~0u && min_v >= re.first && max_v < re.first + re.count;
            if (covers) {
                // small regions that changed recently (ring buffers rewritten between draws) are checked every draw
                bool small = re.count * stride <= 16384 && g_frame - re.last_change < 60;
                if (small || re.checked_frame != g_frame) {
                    const u32 n = region_bytes(re.count), ea = base_ea + re.first * stride;
                    // Meshes over 16 KB that have not changed for 60 frames: on 7 frames of 8 (staggered) only a word per 256 bytes
                    // is compared (the full checks read ~6 MB per frame, memory-bound); a mesh streamed into the buffer
                    // rewrites nearly every line, so the sparse words see it in the same frame, anything else within 8.
                    bool sparse_ok = false;
                    if (n > 16384 && re.sentinel && g_frame - re.last_change > 60 && (g_frame + (key >> 7)) % 8 != 0) {
                        u64 th = prof_usec();
                        sparse_ok = (hash_range(gmem(ea), n, 256) | 1) == re.sentinel;
                        g_prof.t_hash += prof_usec() - th;
                        g_prof.hashed += n / 32;
                    }
                    if (!sparse_ok) {
                        u64 h = hash_of(ea, n);
                        if (h != re.hash) { upload(re.first, re.count); re.hash = h; g_prof.conv_changed += re.count; re.last_change = g_frame; }
                        re.sentinel = n > 16384 ? hash_range(gmem(ea), n, 256) | 1 : 0;
                    }
                    re.checked_frame = g_frame;
                }
            } else if (re.off == ~0u && re.seen_frame && re.seen_frame != g_frame) {
                // seen in an earlier frame: give the region its own block
                re.hash = hash_of(base_ea + min_v * stride, region_bytes(nverts));
                upload(min_v, nverts);
                re.checked_frame = re.last_change = g_frame;
                g_prof.conv_new += nverts;
            } else if (re.off != ~0u) {
                // grow to the union when the ranges are close, else start over at this draw's range
                u32 nf = std::min(re.first, min_v), ne = std::max(re.first + re.count, max_v + 1);
                if (ne - nf > 4 * std::max(re.count, nverts) + 4096) { nf = min_v; ne = max_v + 1; }
                re.hash = hash_of(base_ea + nf * stride, region_bytes(ne - nf));
                upload(nf, ne - nf);
                re.checked_frame = re.last_change = g_frame;
                g_prof.conv_new += re.count;
            } else {
                re.seen_frame = g_frame;   // first sighting (ring-buffer data at a new address every frame): streamed
            }
            if (re.off != ~0u && min_v >= re.first && max_v < re.first + re.count) {
                base = (long long)re.off + (long long)(rmin - re.first) * stride + (attr_ea - base_ea);
                in_ring = false;
            }
        }
        if (base < 0) {
            base = stream(src, bytes);
            g_prof.conv_new += rmax - rmin + 1;
            if (base < 0) { LOG_W("rsx: vertex data of %u bytes does not fit the stream ring", bytes); continue; }
        }
        va[i * 4] = (int)(base - (long long)rmin * stride);   // record 0 (may lie before the buffer start)
        va[i * 4 + 1] = (int)stride;
        va[i * 4 + 2] = (int)(type | (size << 4) | (modulo ? 256 : 0) | (in_ring ? 512 : 0));
        va[i * 4 + 3] = (int)freq;
    }
}

// the draw's index list in the arena (or the buffer idx_stream gave); false when there is nothing to draw
struct DrawGeom { u32 min_v, max_v, idx_count; ptrdiff_t idx_offset; };
bool gather_indices(bool indexed, DrawGeom& out) {
    constexpr u32 RESTART = 0xFFFFFFFFu;
    u32 min_v = ~0u, max_v = 0, idx_count = 0;
    ptrdiff_t idx_offset = 0;   // index lists stay absolute: every draw uses base vertex -min_v
    u64 ti0 = prof_usec();
    {
        auto& ranges = indexed ? g_index_ranges : g_array_ranges;
        u32 first = ranges[0].first, count = 0;   // the game splits long draws into 256-element commands
        bool contiguous = true;
        for (auto& r : ranges) { contiguous &= r.first == first + count; count += r.count; }
        u32 ifmt = reg(0x1820);
        bool u16idx = ((ifmt >> 4) & 0xF) == 1;
        bool restart = indexed && (reg(0x1DAC) & 1);
        u32 rv = reg(0x1DB0);
        u32 ea = indexed ? location_to_ea(reg(0x181C), (ifmt & 0xF) != 0) : 0;
        u32 isz = u16idx ? 2 : 4;
        IdxEntry* ie = nullptr;
        bool need = true;
        u64 h = 0;
        static const bool idx_nocache = getenv("F1CE_IDX_NOCACHE") != nullptr;   // debug
        static const bool no_idx_region = getenv("F1CE_IDX_NOREGION") != nullptr;   // debug
        bool region = false;
        // regions are per 16K-index chunk of the buffer, so draws far apart in one buffer don't fight over a region
        u32 chunk = first >> 14;
        if (indexed && contiguous && g_prim != 8 && g_prim != 9 && !(LINE_LOOP_AS_STRIP && g_prim == 3) && !idx_nocache && !no_idx_region && count &&
            ((first + count - 1) >> 14) == chunk) {
            u32 flags = (u16idx ? 1u : 0u) | (restart ? 2u | (rv << 8) : 0u) | (chunk << 24);
            IdxRegion& ir = g_idx_regions[mix64(((u64)ea << 32) ^ flags) ^ 0x2545F491ull];
            if (ir.ea != ea || ir.flags != flags) { heap_free(g_iheap, ir.off, ir.bytes); ir = IdxRegion{}; ir.ea = ea; ir.flags = flags; }
            ir.last_frame = g_frame;
            if (ir.changes >= 3) goto idx_per_draw;
            auto build = [&](u32 f, u32 n) {   // convert raw indices f .. f+n-1 into the region
                ir.first = f; ir.count = n;
                ir.cpu.resize(n);
                const u8* q = gmem(ea + f * isz);
                u32 lo = ~0u, hi = 0;
                convert_indices(q, n, u16idx, restart, rv, ir.cpu.data(), lo, hi);
                idx_store(ir.cpu.data(), n, ir.off, ir.bytes);   // (full arena: off stays ~0u, the draws stream)
                ir.hash = hash_range(q, n * isz, 8);
                ir.checked_frame = g_frame;
                ir.minmax.clear();
                g_prof.idx_built += n;
            };
            bool covers = !ir.cpu.empty() && first >= ir.first && first + count <= ir.first + ir.count;
            if (covers) {
                bool small = ir.count * isz <= 16384 && ir.changes > 0;   // rewritten before: check every draw
                if (small || ir.checked_frame != g_frame) {
                    u64 hh = hash_range(gmem(ea + ir.first * isz), ir.count * isz, 8);
                    g_prof.hashed += ir.count * isz;
                    if (hh != ir.hash) {
                        if (++ir.changes >= 3) goto idx_per_draw;
                        build(ir.first, ir.count);
                        g_prof.ir_changed += ir.count;
                    }
                    ir.checked_frame = g_frame;
                }
            } else {
                u32 nf = first, ne = first + count;
                if (!ir.cpu.empty()) {   // grow to the union (bounded by the chunk)
                    nf = std::min(ir.first, first);
                    ne = std::max(ir.first + ir.count, first + count);
                }
                build(nf, ne - nf);
                g_prof.ir_grow += ne - nf;
            }
            u64 mk = ((u64)first << 32) | count;
            auto mm = ir.minmax.find(mk);
            if (mm == ir.minmax.end()) {
                u32 lo = ~0u, hi = 0;
                index_minmax(&ir.cpu[first - ir.first], count, lo, hi);
                mm =ir.minmax.emplace(mk, std::make_pair(lo, hi)).first;
            }
            min_v = mm->second.first;
            max_v = mm->second.second;
            if (min_v > max_v) return false;
            if (max_v - min_v > 1000000) { LOG_W("rsx: absurd vertex range %u..%u", min_v, max_v); return false; }
            idx_count = count;
            if (ir.off != ~0u) idx_offset = (ptrdiff_t)ir.off + (ptrdiff_t)(first - ir.first) * 4;
            else idx_offset = idx_stream(&ir.cpu[first - ir.first], count);
            region = true;
            need = false;
        }
    idx_per_draw:
        if (!region && contiguous && !idx_nocache) {
            u32 id_src = indexed ? ea + first * isz : first;
            u32 id_flags = g_prim | (indexed ? 0x100 : 0) | (u16idx ? 0x200 : 0) | (restart ? 0x400 | (rv << 16) : 0);
            u64 key = mix64(((u64)id_src << 32) ^ count) ^ mix64(id_flags);
            ie = &g_idx_cache[key];
            bool cached = ie->off != ~0u;
            if (cached && (ie->id_src != id_src || ie->id_count != count || ie->id_flags != id_flags)) ie->hash = 0;   // collision
            ie->id_src = id_src; ie->id_count = count; ie->id_flags = id_flags;
            if (cached && !indexed && ie->hash == 1) need = false;   // array draws: the list depends on the range only
            else if (!indexed) h = 1;
            else if (cached && count * isz > 16384 && ie->checked_frame == g_frame) {
                need = false;   // large index lists are checked once per frame (the same mesh is drawn in several passes)
            } else {
                h = hash_range(gmem(ea + first * isz), count * isz, 8);
                g_prof.hashed += count * isz;
                need = !cached || h != ie->hash;
                ie->checked_frame = g_frame;
            }
            if (cached) ie->last_frame = g_frame;
        }
        if (need) {
            // one pass: byte swap, restart index, vertex range (the lists stay absolute: drawn with base vertex -min_v)
            static std::vector<u32> indices;
            indices.resize(count);
            u32* o = indices.data();
            for (auto& r : ranges) {
                if (indexed) {
                    convert_indices(gmem(ea + r.first * isz), r.count, u16idx, restart, rv, o, min_v, max_v);
                    o += r.count;
                } else if (r.count) {
                    for (u32 i = 0; i < r.count; i++) *o++ = r.first + i;
                    min_v = std::min(min_v, r.first); max_v = std::max(max_v, r.first + r.count - 1);
                }
            }
            indices.resize(o - indices.data());
            if (min_v > max_v) return false;
            if (max_v - min_v > 1000000) { LOG_W("rsx: absurd vertex range %u..%u", min_v, max_v); return false; }
            if (g_prim == 8 || g_prim == 9) {   // quads / quad strips as triangle lists
                std::vector<u32> tris;
                size_t seg = 0;
                for (size_t i = 0; i <= indices.size(); i++) {
                    if (i < indices.size() && indices[i] != RESTART) continue;
                    size_t n = i - seg;
                    if (g_prim == 8)
                        for (size_t q = seg; q + 3 < seg + n; q += 4) {
                            u32 a = indices[q], b = indices[q + 1], c = indices[q + 2], d = indices[q + 3];
                            tris.insert(tris.end(), {a, b, c, a, c, d});
                        }
                    else
                        for (size_t q = seg; q + 3 < seg + n; q += 2) {
                            u32 a = indices[q], b = indices[q + 1], c = indices[q + 2], d = indices[q + 3];
                            tris.insert(tris.end(), {a, b, d, a, d, c});
                        }
                    seg = i + 1;
                }
                indices.swap(tris);
            }
            if (LINE_LOOP_AS_STRIP && g_prim == 3) {   // no line loops in Vulkan: closed line strips
                std::vector<u32> loop;
                size_t seg = 0;
                for (size_t i = 0; i <= indices.size(); i++) {
                    if (i < indices.size() && indices[i] != RESTART) continue;
                    if (i > seg) {
                        loop.insert(loop.end(), indices.begin() + seg, indices.begin() + i);
                        loop.push_back(indices[seg]);
                        loop.push_back(RESTART);
                    }
                    seg = i + 1;
                }
                indices.swap(loop);
            }
            idx_count = (u32)indices.size();
            g_prof.idx_built += idx_count; g_prof.idx_old += idx_count;
            // first sightings are streamed, as for vertex data
            if (ie && ie->off == ~0u && !(ie->seen_frame && ie->seen_frame != g_frame && ie->hash == h)) {
                if (ie->seen_frame != g_frame || ie->hash != h) ie->seen_frame = g_frame;
                ie->hash = h;
                ie->last_frame = g_frame;
                ie = nullptr;
            }
            if (ie && idx_store(indices.data(), idx_count, ie->off, ie->bytes)) {
                ie->hash = h; ie->count = idx_count; ie->min_v = min_v; ie->max_v = max_v;
                ie->last_change = ie->last_frame = g_frame;
                idx_offset = (ptrdiff_t)ie->off;
            } else {
                idx_offset = idx_stream(indices.data(), idx_count);   // (arena full: also cached lists stream)
            }
        } else if (!region) {
            idx_count = ie->count;
            min_v = ie->min_v;
            max_v = ie->max_v;
            idx_offset = (ptrdiff_t)ie->off;
        }
        if (!idx_count) return false;
    }
    out = {min_v, max_v, idx_count, idx_offset};
    return true;
}
