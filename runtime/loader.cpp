// Loads the game's (decrypted) EBOOT.elf segments into guest memory at runtime and starts the
// main PPU thread. The recompiled code never embeds game data; it is read from the user's copy.
#include "host.h"
#include "kernel.h"

#include <filesystem>
#include <fstream>
#include <vector>

u32 g_tls_addr = 0, g_tls_filesz = 0, g_tls_memsz = 0;
u32 g_malloc_pagesize = 0x100000;
std::string g_game_dir;
std::string g_data_dir;
std::string g_title_id = "UNKNOWN00";

// PARAM.SFO: little-endian key/value table; we only need TITLE_ID.
static void read_title_id(const std::string& sfo_path) {
    std::ifstream f(std::filesystem::u8path(sfo_path), std::ios::binary);
    std::vector<u8> d((std::istreambuf_iterator<char>(f)), {});
    if (d.size() < 20 || memcmp(d.data(), "\0PSF", 4)) return;
    auto le32 = [&](size_t o) { u32 v; memcpy(&v, &d[o], 4); return v; };
    auto le16 = [&](size_t o) { u16 v; memcpy(&v, &d[o], 2); return v; };
    u32 keys = le32(8), data = le32(12), n = le32(16);
    for (u32 i = 0; i < n && 20 + i * 16 + 16 <= d.size(); i++) {
        size_t e = 20 + i * 16;
        std::string key((const char*)&d[keys + le16(e)]);
        if (key == "TITLE_ID") {
            g_title_id = std::string((const char*)&d[data + le32(e + 12)]);
            LOG_I("title id: %s", g_title_id.c_str());
        }
    }
}

namespace {
template <typename T> T be(const u8* p) {
    T v;
    memcpy(&v, p, sizeof(T));
    if constexpr (sizeof(T) == 2) return __builtin_bswap16(v);
    else if constexpr (sizeof(T) == 4) return __builtin_bswap32(v);
    else return __builtin_bswap64(v);
}
}  // namespace

struct LoadedProcess { u32 entry_opd; u32 stack_size; s32 prio; };

u32 g_ticks_per_frame_ea = 0;   // set by F1CE_60FPS

LoadedProcess load_elf(const std::string& path) {
    std::ifstream f(std::filesystem::u8path(path), std::ios::binary);
    if (!f) fatal("cannot open %s", path.c_str());
    std::vector<u8> d((std::istreambuf_iterator<char>(f)), {});
    if (d.size() < 64 || memcmp(d.data(), "\x7f" "ELF\x02\x02", 6)) fatal("%s is not a decrypted PPU ELF", path.c_str());

    LoadedProcess p{be<u32>(&d[24 + 4]), 0x100000, 1001};
    u64 phoff = be<u64>(&d[32]);
    u16 phnum = be<u16>(&d[56]);
    for (u16 i = 0; i < phnum; i++) {
        const u8* ph = &d[phoff + i * 56];
        u32 type = be<u32>(ph);
        u64 off = be<u64>(ph + 8), va = be<u64>(ph + 16), fsz = be<u64>(ph + 32), msz = be<u64>(ph + 40);
        if (type == 1 && msz) {
            memcpy(gmem((u32)va), &d[off], fsz);
            memset(gmem((u32)(va + fsz)), 0, msz - fsz);
            LOG_I("segment %08llx filesz=%llx memsz=%llx", (unsigned long long)va, (unsigned long long)fsz,
                  (unsigned long long)msz);
        } else if (type == 7) {
            g_tls_addr = (u32)va; g_tls_filesz = (u32)fsz; g_tls_memsz = (u32)msz;
        } else if (type == 0x60000001 && fsz >= 0x1c) {   // sys_process_param
            const u8* pp = &d[off];
            p.prio = (s32)be<u32>(pp + 0x10);
            // SYS_PROCESS_PRIMARY_STACK_SIZE_* codes: 0x10=32K, 0x20=64K, 0x30=96K, 0x40=128K, 0x50=256K, 0x60=512K, 0x70=1M
            static const u32 sizes[] = {0x100000, 0x8000, 0x10000, 0x18000, 0x20000, 0x40000, 0x80000, 0x100000};
            u32 code = be<u32>(pp + 0x14);
            p.stack_size = code <= 0x70 && !(code & 0xF) ? sizes[code >> 4] : code;
            g_malloc_pagesize = be<u32>(pp + 0x18);
            LOG_I("process param: sdk=%08x prio=%d stack=%x malloc_pagesize=%x", be<u32>(pp + 0xc), p.prio,
                  p.stack_size, g_malloc_pagesize);
        }
    }
    LOG_I("TLS image %08x filesz=%x memsz=%x", g_tls_addr, g_tls_filesz, g_tls_memsz);
    if (getenv("F1CE_60FPS")) {
        // game ticks per frame, read by the main loop (0x2494a4) every frame; 2 on the 01.00 disc. do_flip sets it
        // from the time each frame took and the vblank thread doubles its rate for the game's frame limiter
        const u32 ticks_per_frame = 0x10044288;
        u32 v = ld32(ticks_per_frame);
        if (v == 2) { g_ticks_per_frame_ea = ticks_per_frame; LOG_I("60 fps: adaptive ticks per frame"); }
        else LOG_W("60 fps: unexpected ticks per frame %08x (not the 01.00 executable?): not patched", v);
    }
    return p;
}

void limit_cpus();   // core.cpp

int main(int argc, char** argv) {
    namespace fs = std::filesystem;
    limit_cpus();
    // Usage: f1ce [path/to/PS3_GAME] [--trace]
    std::string game = "PS3_GAME";
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--trace") g_log_trace = true;
        else game = a;
    }
    if (!fs::exists(fs::u8path(game) / "USRDIR" / "EBOOT.elf"))
        fatal("PS3_GAME folder with a decrypted USRDIR/EBOOT.elf not found at '%s'. Pass it as first argument.",
              game.c_str());
    g_game_dir = PATH_STR(fs::absolute(fs::u8path(game)));
    g_data_dir = PATH_STR(fs::current_path() / "ps3_data");
    fs::create_directories(fs::u8path(g_data_dir) / "dev_hdd0" / "game");
    LOG_I("F1 Championship Edition static recompilation - game dir: %s", g_game_dir.c_str());

    read_title_id(PATH_STR(fs::u8path(g_game_dir) / "PARAM.SFO"));
    host::init();
    mem::init();
    extern void start_watchdog(u32);
    start_watchdog(20);
    extern void start_profiler();
    start_profiler();
    extern void start_host_profiler();
    start_host_profiler();
    if (getenv("F1CE_SPU_BENCH")) { extern void spu_bench(); spu_bench(); exit(0); }
    LoadedProcess p = load_elf(PATH_STR(fs::u8path(g_game_dir) / "USRDIR" / "EBOOT.elf"));
    run_main_thread(p.entry_opd, p.stack_size, p.prio);
    LOG_I("main thread returned");
    return 0;
}
