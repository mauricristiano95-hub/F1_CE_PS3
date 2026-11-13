// The game's executable from a PS3_GAME folder: the disc's encrypted USRDIR/EBOOT.BIN is decrypted in memory,
// so players need no separate decryption step. Shared by the runtime's loader and the launchers.
#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace eboot {

// USRDIR/EBOOT.BIN decrypted, else (a missing or unusable EBOOT.BIN) USRDIR/EBOOT.elf, checked against the
// executable this build was made from. Returns an error message for the player; empty: elf holds the executable.
std::string load(const std::filesystem::path& game, std::vector<uint8_t>& elf);

// a retail (or fake-signed) PPU SELF to its ELF, laid out as RPCS3's decrypter writes it
std::string decrypt_self(const std::vector<uint8_t>& self, std::vector<uint8_t>& elf);

// FNV-1a of the entry point, program headers and segment contents (not the section headers, which
// decrypters write differently); 0: not a 64-bit big-endian ELF
uint64_t image_hash(const std::vector<uint8_t>& elf);

}  // namespace eboot
