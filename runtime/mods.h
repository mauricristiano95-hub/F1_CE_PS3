// Mod packs (season packs): files of MODS/<pack>/ replace or add game files at run time, original files untouched.
#pragma once
#include "kernel.h"

namespace mods {

// A file whose bytes are composed at open time (a .wad with replaced or added entries).
struct VirtualFile {
    virtual ~VirtualFile() = default;
    virtual u64 size() const = 0;
    virtual size_t read(u64 off, void* dst, size_t n) = 0;
};

// Host path of the pack's replacement for a game file (loose files of USRDIR), or empty.
std::string loose_override(const std::string& host_path);
// The archive as the game must see it, or nullptr when the pack changes nothing in it.
std::unique_ptr<VirtualFile> open_wad(const std::string& host_path);
// Size of that virtual archive (stat), or -1 when the pack changes nothing in it.
s64 wad_size(const std::string& host_path);

// Name hash of the game's Wad lookup (0x1DA20).
u32 name_hash(const std::string& path);

}  // namespace mods
