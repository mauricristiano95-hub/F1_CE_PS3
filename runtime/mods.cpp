// Mod packs (season packs). F1CE_MOD_PACK=<name> selects MODS/<name>/ (F1CE_MODS_DIR overrides the MODS folder, the
// launcher passes the one next to it); unset, nothing changes.
//
// Inside a pack, folders before the first marker are free (the Content Manager's CARS/<team>/, DRIVERS/<driver>/,
// ...); the marker says where the rest of the path points:
//  - a folder named like an archive (offline.wad, monaco.wad, update.wad, ...): the rest is the path inside that
//    archive, e.g. CARS/Ferrari/offline.wad/Models/Cars/Ferrari/Body.gtf; _unknown/<8 hex digits>.<ext> names an entry
//    by its hash (entries whose path is not known yet);
//  - a folder named USRDIR: the rest is a loose game file, e.g. USRDIR/tracks/monaco/environmentsettings.txt.
// Files outside any marker (pack.ini, previews) are the manager's and ignored here.
//
// Archives (tools/wad.py has the format): the game reads the header, then count x 16-byte entries {hash, offset,
// size, stored} (Wad ctor 0x4A8E4) and opens an entry by its name hash (0x4A4B8); stored == size means raw. A
// changed archive is served as a virtual file: a rebuilt table, the original file shifted behind it, then each pack
// file as a raw entry. The game's own reader does the rest, so no game code is patched.
#include "mods.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <vector>

namespace stdfs = std::filesystem;

namespace {
constexpr u64 ALIGN = 0x800;   // the game's entries are 64-byte aligned; any multiple keeps that

std::string lower(std::string s) {
    for (auto& ch : s) ch = (char)tolower((unsigned char)ch);
    return s;
}
u64 align_up(u64 v) { return (v + ALIGN - 1) & ~(ALIGN - 1); }
u32 be32(const u8* p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }
void put_be32(u8* p, u32 v) { p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16); p[2] = (u8)(v >> 8); p[3] = (u8)v; }

FILE* open_read(const stdfs::path& p) {
#ifdef _WIN32
    return _wfopen(p.wstring().c_str(), L"rb");
#else
    return fopen(p.c_str(), "rb");
#endif
}
bool seek(FILE* f, u64 off) {
#ifdef _WIN32
    return _fseeki64(f, (s64)off, SEEK_SET) == 0;
#else
    return fseeko(f, (off_t)off, SEEK_SET) == 0;
#endif
}

struct PackIndex {
    std::unordered_map<std::string, std::string> loose;                          // usrdir-relative, lower -> host
    std::unordered_map<std::string, std::map<u32, stdfs::path>> wads;            // archive file name, lower
};

PackIndex build_index() {
    PackIndex ix;
    const char* pack = getenv("F1CE_MOD_PACK");
    if (!pack || !*pack) return ix;
    const char* dir = getenv("F1CE_MODS_DIR");
    stdfs::path root = stdfs::u8path(dir && *dir ? dir : "MODS") / stdfs::u8path(pack);
    std::error_code ec;
    if (!stdfs::is_directory(root, ec)) {
        LOG_W("mods: pack folder %s not found", PATH_STR(root).c_str());
        return ix;
    }
    size_t n_wad = 0;
    for (auto it = stdfs::recursive_directory_iterator(root, ec); !ec && it != stdfs::recursive_directory_iterator();
         it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        std::vector<std::string> parts;
        for (const auto& c : it->path().lexically_relative(root)) parts.push_back(PATH_STR(c));
        for (size_t i = 0; i + 1 < parts.size(); i++) {
            std::string m = lower(parts[i]);
            std::string rest;
            for (size_t j = i + 1; j < parts.size(); j++) rest += (rest.empty() ? "" : "/") + parts[j];
            if (m == "usrdir") {
                ix.loose[lower(rest)] = PATH_STR(it->path());
                break;
            }
            if (m.size() > 4 && m.compare(m.size() - 4, 4, ".wad") == 0) {
                u32 h;
                std::string stem = PATH_STR(it->path().stem());
                bool by_hash = i + 3 == parts.size() && lower(parts[i + 1]) == "_unknown" && stem.size() == 8 &&
                               stem.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos;
                h = by_hash ? (u32)strtoul(stem.c_str(), nullptr, 16) : mods::name_hash(rest);
                ix.wads[m][h] = it->path();
                n_wad++;
                break;
            }
        }
    }
    LOG_I("mods: pack \"%s\": %zu archive entries in %zu archives, %zu loose files", pack, n_wad, ix.wads.size(),
          ix.loose.size());
    return ix;
}

const PackIndex& index() {
    static const PackIndex ix = build_index();
    return ix;
}

struct WadLayout {
    std::vector<u8> head;            // rebuilt header + table, padded to `base`
    u64 base = 0;                    // virtual offset of the original file's byte 0
    u64 orig_size = 0;
    struct Added { u64 off, size; stdfs::path path; };
    std::vector<Added> added;        // ascending offsets
    u64 total = 0;
};

std::shared_ptr<const WadLayout> build_layout(const std::string& host, const std::map<u32, stdfs::path>& repl) {
    FILE* f = open_read(stdfs::u8path(host));
    if (!f) return nullptr;
    u8 hdr[8];
    std::vector<u8> tab;
    bool ok = fread(hdr, 1, 8, f) == 8;
    u32 count = ok ? be32(hdr + 4) : 0;
    if (ok && count < 0x100000) {
        tab.resize((size_t)count * 16);
        ok = fread(tab.data(), 1, tab.size(), f) == tab.size();
    } else ok = false;
    std::error_code ec;
    u64 orig_size = stdfs::file_size(stdfs::u8path(host), ec);
    fclose(f);
    if (!ok || ec) {
        LOG_W("mods: %s is not an archive, left as is", host.c_str());
        return nullptr;
    }
    auto L = std::make_shared<WadLayout>();
    std::map<u32, size_t> slot;                                  // hash -> entry index
    for (u32 i = 0; i < count; i++) slot[be32(&tab[i * 16])] = i;
    size_t n_new = 0;
    for (auto& [h, p] : repl)
        if (!slot.count(h)) n_new++;
    u32 n = count + (u32)n_new;
    L->base = align_up(8 + (u64)n * 16);
    L->orig_size = orig_size;
    L->head.assign(L->base, 0);
    put_be32(&L->head[0], be32(hdr));
    put_be32(&L->head[4], n);
    for (u32 i = 0; i < count; i++) {
        u8* e = &L->head[8 + i * 16];
        memcpy(e, &tab[i * 16], 16);
        put_be32(e + 4, (u32)(be32(e + 4) + L->base));
    }
    u64 off = align_up(L->base + orig_size);
    u32 next = count;
    size_t replaced = 0;
    for (auto& [h, p] : repl) {
        u64 sz = stdfs::file_size(p, ec);
        if (ec || sz >= 0xFFFFFFFFull) continue;
        auto s = slot.find(h);
        u8* e = &L->head[8 + (s != slot.end() ? s->second : next++) * 16];
        replaced += s != slot.end();
        put_be32(e, h);
        put_be32(e + 4, (u32)off);
        put_be32(e + 8, (u32)sz);
        put_be32(e + 12, (u32)sz);                               // stored == size: the game reads it raw
        L->added.push_back({off, sz, p});
        off = align_up(off + sz);
    }
    L->total = off;
    if (L->total >= 0xFFFFFFFFull) {
        LOG_W("mods: %s would exceed 4 GB, left as is", host.c_str());
        return nullptr;
    }
    LOG_I("mods: %s: %zu entries replaced, %zu added", PATH_STR(stdfs::u8path(host).filename()).c_str(), replaced,
          L->added.size() - replaced);
    return L;
}

std::shared_ptr<const WadLayout> layout_for(const std::string& host) {
    const auto& ix = index();
    if (ix.wads.empty()) return nullptr;
    auto w = ix.wads.find(lower(PATH_STR(stdfs::u8path(host).filename())));
    if (w == ix.wads.end()) return nullptr;
    static std::mutex m;
    static std::unordered_map<std::string, std::shared_ptr<const WadLayout>> cache;
    std::lock_guard lk(m);
    auto c = cache.find(host);
    if (c != cache.end()) return c->second;
    return cache[host] = build_layout(host, w->second);
}

struct VirtualWad final : mods::VirtualFile {
    std::shared_ptr<const WadLayout> L;
    FILE* orig = nullptr;
    FILE* cur = nullptr;          // the pack file read last
    size_t cur_i = SIZE_MAX;
    ~VirtualWad() override {
        if (orig) fclose(orig);
        if (cur) fclose(cur);
    }
    u64 size() const override { return L->total; }
    size_t read(u64 off, void* dst, size_t n) override {
        u8* out = (u8*)dst;
        size_t done = 0;
        while (done < n && off < L->total) {
            size_t want = n - done, got = 0;
            if (off < L->base) {
                got = (size_t)std::min<u64>(want, L->base - off);
                memcpy(out + done, &L->head[off], got);
            } else if (off < L->base + L->orig_size) {
                got = (size_t)std::min<u64>(want, L->base + L->orig_size - off);
                got = seek(orig, off - L->base) ? fread(out + done, 1, got, orig) : 0;
                if (!got) break;
            } else {
                // a pack file, or the padding before one
                auto it = std::upper_bound(L->added.begin(), L->added.end(), off,
                                           [](u64 o, const WadLayout::Added& a) { return o < a.off; });
                size_t i = (size_t)(it - L->added.begin());
                if (i > 0 && off < L->added[i - 1].off + L->added[i - 1].size) {
                    const auto& a = L->added[i - 1];
                    if (cur_i != i - 1) {
                        if (cur) fclose(cur);
                        cur = open_read(a.path);
                        cur_i = i - 1;
                    }
                    got = (size_t)std::min<u64>(want, a.off + a.size - off);
                    got = cur && seek(cur, off - a.off) ? fread(out + done, 1, got, cur) : 0;
                    if (!got) break;
                } else {
                    u64 end = i < L->added.size() ? L->added[i].off : L->total;
                    got = (size_t)std::min<u64>(want, end - off);
                    memset(out + done, 0, got);
                }
            }
            done += got;
            off += got;
        }
        return done;
    }
};
}  // namespace

namespace mods {

u32 name_hash(const std::string& path) {
    static const auto table = [] {
        std::array<u32, 256> t{};
        for (u32 i = 0; i < 256; i++) {
            u32 c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? (c >> 1) ^ 0xEDB88320u : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    if (path.size() == 9 && path[0] == '#' && path.find_first_not_of("0123456789ABCDEFabcdef", 1) == std::string::npos)
        return (u32)strtoul(path.c_str() + 1, nullptr, 16);
    u32 h = 0;   // register starts at 0, not ~0 (game's choice)
    for (unsigned char ch : path) {
        u8 b = ch == '\\' ? '/' : (u8)tolower(ch);
        h = table[(h ^ b) & 0xFF] ^ (h >> 8);
    }
    return ~h;
}

std::string loose_override(const std::string& host_path) {
    const auto& ix = index();
    if (ix.loose.empty()) return {};
    stdfs::path usr = stdfs::u8path(g_game_dir) / "USRDIR";
    std::string rel = PATH_STR(stdfs::path(stdfs::u8path(host_path).lexically_relative(usr).generic_string()));
    if (rel.empty() || rel.rfind("..", 0) == 0) return {};
    auto it = ix.loose.find(lower(rel));
    if (it == ix.loose.end()) return {};
    LOG_I("mods: %s <- %s", rel.c_str(), it->second.c_str());
    return it->second;
}

std::unique_ptr<VirtualFile> open_wad(const std::string& host_path) {
    auto L = layout_for(host_path);
    if (!L) return nullptr;
    auto v = std::make_unique<VirtualWad>();
    v->L = std::move(L);
    v->orig = open_read(stdfs::u8path(host_path));
    if (!v->orig) return nullptr;
    return v;
}

s64 wad_size(const std::string& host_path) {
    auto L = layout_for(host_path);
    return L ? (s64)L->total : -1;
}

}  // namespace mods
