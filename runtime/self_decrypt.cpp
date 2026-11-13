// Retail SELF decryption for the game's EBOOT.BIN (see self_decrypt.h). Only what this executable needs: the APP
// key of revision 1 (the 01.00 disc), no NPDRM layer, zlib-compressed segments. Layout as in RPCS3's unself.cpp:
//   SCE header (0x20) | SELF header: ELF header, program headers, section headers, segment table offsets
//   metadata info at 0x20 + se_meta: AES-256-CBC (erk/riv) -> the AES-128 key and IV of the metadata headers
//   metadata headers (CTR): section entries (file offset, size, program index, key/IV slots, flags) + key slots
//   each encrypted section: AES-128-CTR with its key/IV slots, then inflated if compressed
#include "self_decrypt.h"

#include <algorithm>
#include <cstring>
#include <fstream>

namespace eboot {
namespace {
using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;

// the executable this build was translated from (USRDIR/EBOOT.BIN of BCES00005, 01.00)
constexpr u64 EXPECTED_HASH = 0x4503745b63b99d73ull;

// APP keys, revision 0x0001 (public since 2011; RPCS3's key_vault.cpp)
constexpr u8 APP1_ERK[32] = {0x79, 0x48, 0x18, 0x39, 0xC4, 0x06, 0xA6, 0x32, 0xBD, 0xB4, 0xAC, 0x09, 0x3D, 0x73, 0xD9, 0x9A,
                             0xE1, 0x58, 0x7F, 0x24, 0xCE, 0x7E, 0x69, 0x19, 0x2C, 0x1C, 0xD0, 0x01, 0x02, 0x74, 0xA8, 0xAB};
constexpr u8 APP1_RIV[16] = {0x6F, 0x0F, 0x25, 0xE1, 0xC8, 0xC4, 0xB7, 0xAE, 0x70, 0xDF, 0x96, 0x8B, 0x04, 0x52, 0x1D, 0xDA};

u16 be16(const u8* p) { return (u16)(p[0] << 8 | p[1]); }
u32 be32(const u8* p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }
u64 be64(const u8* p) { return (u64)be32(p) << 32 | be32(p + 4); }

// ---------------------------------------------------------------- AES (FIPS-197)
u8 rol8(u8 x, int n) { return (u8)(x << n | x >> (8 - n)); }
u8 xtime(u8 x) { return (u8)(x << 1 ^ (x & 0x80 ? 0x1b : 0)); }
u8 gmul(u8 a, u8 b) {
    u8 r = 0;
    for (; b; b >>= 1, a = xtime(a))
        if (b & 1) r ^= a;
    return r;
}
u32 ror32(u32 x, int n) { return x >> n | x << (32 - n); }

struct Tables {
    u8 sbox[256], inv[256];
    u32 te[4][256];   // SubBytes + MixColumns of the byte in row r of a column
    Tables() {
        u8 p = 1, q = 1;
        do {   // p walks the multiplicative group (generator 3), q its inverses
            p = (u8)(p ^ xtime(p));
            q = (u8)(q ^ q << 1);
            q = (u8)(q ^ q << 2);
            q = (u8)(q ^ q << 4);
            if (q & 0x80) q ^= 0x09;
            sbox[p] = (u8)(q ^ rol8(q, 1) ^ rol8(q, 2) ^ rol8(q, 3) ^ rol8(q, 4) ^ 0x63);
        } while (p != 1);
        sbox[0] = 0x63;
        for (int i = 0; i < 256; i++) inv[sbox[i]] = (u8)i;
        for (int i = 0; i < 256; i++) {
            const u8 s = sbox[i], s2 = xtime(s);
            te[0][i] = (u32)s2 << 24 | (u32)s << 16 | (u32)s << 8 | (u8)(s2 ^ s);
            for (int r = 1; r < 4; r++) te[r][i] = ror32(te[0][i], 8 * r);
        }
    }
};
const Tables& T() {
    static const Tables t;
    return t;
}

struct Aes {
    u32 rk[60];
    int rounds;
    Aes(const u8* key, int bits) {
        const int nk = bits / 32;
        rounds = nk + 6;
        const u8* sb = T().sbox;
        for (int i = 0; i < nk; i++) rk[i] = be32(key + 4 * i);
        u8 rcon = 1;
        for (int i = nk; i < 4 * (rounds + 1); i++) {
            u32 t = rk[i - 1];
            if (i % nk == 0) {
                t = (u32)sb[t >> 16 & 0xff] << 24 | (u32)sb[t >> 8 & 0xff] << 16 | (u32)sb[t & 0xff] << 8 | sb[t >> 24];
                t ^= (u32)rcon << 24;
                rcon = xtime(rcon);
            } else if (nk > 6 && i % nk == 4) {
                t = (u32)sb[t >> 24] << 24 | (u32)sb[t >> 16 & 0xff] << 16 | (u32)sb[t >> 8 & 0xff] << 8 | sb[t & 0xff];
            }
            rk[i] = rk[i - nk] ^ t;
        }
    }
    void encrypt(const u8* in, u8* out) const {
        const Tables& t = T();
        u32 s[4], n[4];
        for (int c = 0; c < 4; c++) s[c] = be32(in + 4 * c) ^ rk[c];
        for (int r = 1; r < rounds; r++) {
            for (int c = 0; c < 4; c++)
                n[c] = t.te[0][s[c] >> 24] ^ t.te[1][s[(c + 1) & 3] >> 16 & 0xff] ^ t.te[2][s[(c + 2) & 3] >> 8 & 0xff] ^
                       t.te[3][s[(c + 3) & 3] & 0xff] ^ rk[4 * r + c];
            memcpy(s, n, sizeof(s));
        }
        for (int c = 0; c < 4; c++) {
            const u32 v = ((u32)t.sbox[s[c] >> 24] << 24 | (u32)t.sbox[s[(c + 1) & 3] >> 16 & 0xff] << 16 |
                           (u32)t.sbox[s[(c + 2) & 3] >> 8 & 0xff] << 8 | t.sbox[s[(c + 3) & 3] & 0xff]) ^ rk[4 * rounds + c];
            out[4 * c] = (u8)(v >> 24), out[4 * c + 1] = (u8)(v >> 16), out[4 * c + 2] = (u8)(v >> 8), out[4 * c + 3] = (u8)v;
        }
    }
    void decrypt(const u8* in, u8* out) const {   // state byte s[r + 4c]; only for the 64-byte metadata info
        const u8* inv = T().inv;
        u8 s[16], n[16];
        auto add_key = [&](int r) {
            for (int c = 0; c < 4; c++)
                for (int b = 0; b < 4; b++) s[b + 4 * c] ^= (u8)(rk[4 * r + c] >> (24 - 8 * b));
        };
        auto inv_shift_sub = [&] {
            for (int c = 0; c < 4; c++)
                for (int b = 0; b < 4; b++) n[b + 4 * ((c + b) & 3)] = inv[s[b + 4 * c]];
            memcpy(s, n, 16);
        };
        memcpy(s, in, 16);
        add_key(rounds);
        for (int r = rounds - 1; r > 0; r--) {
            inv_shift_sub();
            add_key(r);
            for (int c = 0; c < 4; c++) {
                const u8 a0 = s[4 * c], a1 = s[4 * c + 1], a2 = s[4 * c + 2], a3 = s[4 * c + 3];
                s[4 * c] = gmul(a0, 14) ^ gmul(a1, 11) ^ gmul(a2, 13) ^ gmul(a3, 9);
                s[4 * c + 1] = gmul(a0, 9) ^ gmul(a1, 14) ^ gmul(a2, 11) ^ gmul(a3, 13);
                s[4 * c + 2] = gmul(a0, 13) ^ gmul(a1, 9) ^ gmul(a2, 14) ^ gmul(a3, 11);
                s[4 * c + 3] = gmul(a0, 11) ^ gmul(a1, 13) ^ gmul(a2, 9) ^ gmul(a3, 14);
            }
        }
        inv_shift_sub();
        add_key(0);
        memcpy(out, s, 16);
    }
};

void cbc_decrypt(const Aes& a, const u8* iv, u8* d, size_t n) {
    u8 prev[16], cur[16];
    memcpy(prev, iv, 16);
    for (size_t o = 0; o + 16 <= n; o += 16) {
        memcpy(cur, d + o, 16);
        a.decrypt(cur, d + o);
        for (int i = 0; i < 16; i++) d[o + i] ^= prev[i];
        memcpy(prev, cur, 16);
    }
}

void ctr_crypt(const Aes& a, const u8* iv, u8* d, size_t n) {
    u8 ctr[16], ks[16];
    memcpy(ctr, iv, 16);
    for (size_t o = 0; o < n; o += 16) {
        a.encrypt(ctr, ks);
        for (size_t i = 0; i < 16 && o + i < n; i++) d[o + i] ^= ks[i];
        for (int i = 15; i >= 0 && ++ctr[i] == 0; i--) {}
    }
}

// ---------------------------------------------------------------- inflate (RFC 1950/1951, after zlib's puff.c)
struct Inflate {
    const u8* in;
    size_t n, pos = 0;
    u32 buf = 0;
    int cnt = 0;
    bool bad = false;
    std::vector<u8> out;

    Inflate(const u8* p, size_t len) : in(p), n(len) {}

    struct Huff { u16 count[16], sym[320]; };

    int bits(int k) {
        while (cnt < k) {
            if (pos >= n) { bad = true; return 0; }
            buf |= (u32)in[pos++] << cnt;
            cnt += 8;
        }
        const int v = (int)(buf & ((1u << k) - 1));
        buf >>= k;
        cnt -= k;
        return v;
    }
    static void build(Huff& h, const u8* len, int num) {
        u16 offs[16];
        memset(h.count, 0, sizeof(h.count));
        for (int i = 0; i < num; i++) h.count[len[i]]++;
        offs[1] = 0;
        for (int l = 1; l < 15; l++) offs[l + 1] = (u16)(offs[l] + h.count[l]);
        for (int i = 0; i < num; i++)
            if (len[i]) h.sym[offs[len[i]]++] = (u16)i;
    }
    int decode(const Huff& h) {
        int code = 0, first = 0, index = 0;
        for (int l = 1; l < 16 && !bad; l++) {
            code |= bits(1);
            const int c = h.count[l];
            if (code - c < first) return h.sym[index + (code - first)];
            index += c;
            first = (first + c) << 1;
            code <<= 1;
        }
        bad = true;
        return 0;
    }
    void codes(const Huff& lit, const Huff& dist) {
        static const u16 lbase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                      35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
        static const u8 lext[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
        static const u16 dbase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769,
                                      1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
        static const u8 dext[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
        for (;;) {
            int s = decode(lit);
            if (bad) return;
            if (s < 256) { out.push_back((u8)s); continue; }
            if (s == 256) return;
            s -= 257;
            if (s >= 29) { bad = true; return; }
            const int len = lbase[s] + bits(lext[s]);
            const int ds = decode(dist);
            if (bad || ds >= 30) { bad = true; return; }
            const size_t d = dbase[ds] + (size_t)bits(dext[ds]);
            if (d > out.size()) { bad = true; return; }
            for (int i = 0; i < len; i++) out.push_back(out[out.size() - d]);
        }
    }
    bool run(size_t expect) {
        out.reserve(expect);
        if (n < 2 || (in[0] & 0x0f) != 8 || (in[1] & 0x20) || ((in[0] << 8 | in[1]) % 31)) return false;
        pos = 2;
        for (int last = 0; !last && !bad;) {
            last = bits(1);
            const int type = bits(2);
            if (type == 0) {
                buf >>= cnt & 7, cnt &= ~7;
                const int len = bits(16), nlen = bits(16);
                if (len != (~nlen & 0xffff)) return false;
                for (int i = 0; i < len && !bad; i++) out.push_back((u8)bits(8));
            } else if (type == 1) {
                static Huff fl, fd;
                static const bool init = [] {
                    u8 l[288];
                    memset(l, 8, 144), memset(l + 144, 9, 112), memset(l + 256, 7, 24), memset(l + 280, 8, 8);
                    build(fl, l, 288);
                    memset(l, 5, 30);
                    build(fd, l, 30);
                    return true;
                }();
                (void)init;
                codes(fl, fd);
            } else if (type == 2) {
                static const u8 order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
                const int nlen = bits(5) + 257, ndist = bits(5) + 1, ncode = bits(4) + 4;
                if (nlen > 286 || ndist > 30) return false;
                u8 l[320] = {};
                for (int i = 0; i < ncode; i++) l[order[i]] = (u8)bits(3);
                Huff lc, lit, dist;
                build(lc, l, 19);
                for (int i = 0; i < nlen + ndist && !bad;) {
                    int s = decode(lc), rep = 0;
                    u8 v = 0;
                    if (s < 16) { l[i++] = (u8)s; continue; }
                    if (s == 16) {
                        if (!i) return false;
                        v = l[i - 1], rep = 3 + bits(2);
                    } else rep = s == 17 ? 3 + bits(3) : 11 + bits(7);
                    if (i + rep > nlen + ndist) return false;
                    while (rep--) l[i++] = v;
                }
                build(lit, l, nlen);
                build(dist, l + nlen, ndist);
                codes(lit, dist);
            } else return false;
        }
        return !bad && out.size() == expect;
    }
};

std::vector<u8> read_file(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<u8>((std::istreambuf_iterator<char>(f)), {});
}
}  // namespace

std::string decrypt_self(const std::vector<u8>& self, std::vector<u8>& elf) {
    const char* damaged = "could not be decrypted (incomplete or damaged copy).";
    const u8* d = self.data();
    const size_t size = self.size();
    if (size < 0x90 || memcmp(d, "SCE\0", 4) || be16(d + 10) != 1) return "is not a PS3 executable (SELF).";
    const u16 revision = be16(d + 8);
    const u32 meta = 0x20 + be32(d + 12);
    const u64 hsize = be64(d + 16);
    const u64 elf_off = be64(d + 0x30), phdr_off = be64(d + 0x38), shdr_off = be64(d + 0x40);
    if (hsize > size || meta + 0x60 > hsize || elf_off + 64 > hsize) return damaged;
    const u8* eh = d + elf_off;
    if (memcmp(eh, "\x7f" "ELF\x02\x02", 6)) return "is not a 64-bit PPU executable.";
    const u64 e_phoff = be64(eh + 32), e_shoff = be64(eh + 40);
    const u16 phnum = be16(eh + 56), shnum = be16(eh + 60);
    if (phdr_off + phnum * 56ull > hsize || (shnum && shdr_off + shnum * 64ull > size)) return damaged;

    // metadata: info (key + IV) then headers, decrypted in a copy of the header
    std::vector<u8> h(d, d + hsize);
    if (revision != 0x8000) {   // (0x8000: fake-signed, metadata in the clear)
        if (revision != 0x0001)
            return "uses encryption key revision " + std::to_string(revision) +
                   ", not the one of the game's 01.00 disc: it is another version or a different game.";
        cbc_decrypt(Aes(APP1_ERK, 256), APP1_RIV, &h[meta], 0x40);
        for (int i = 0; i < 16; i++)
            if (h[meta + 16 + i] || h[meta + 48 + i]) return damaged;   // the key and IV pads are zero
        ctr_crypt(Aes(&h[meta], 128), &h[meta + 32], &h[meta + 0x40], hsize - meta - 0x40);
    }
    const u8* mh = &h[meta + 0x40];
    const u32 sections = be32(mh + 12), key_slots = be32(mh + 16);
    const u8* sec = mh + 0x20;
    const u8* keys = sec + sections * 0x30ull;
    if (meta + 0x60 + sections * 0x30ull + key_slots * 16ull > hsize) return damaged;

    // the ELF as RPCS3 writes it: ELF header, program headers, segments at their offsets, section headers
    u64 out_size = std::max<u64>(64, e_phoff + phnum * 56ull);
    if (shnum) out_size = std::max<u64>(out_size, e_shoff + shnum * 64ull);
    for (u16 i = 0; i < phnum; i++) {
        const u8* ph = d + phdr_off + i * 56;
        out_size = std::max<u64>(out_size, be64(ph + 8) + be64(ph + 32));
    }
    if (out_size > (256u << 20)) return damaged;
    elf.assign(out_size, 0);
    memcpy(&elf[0], eh, 64);
    memcpy(&elf[e_phoff], d + phdr_off, phnum * 56ull);
    if (shnum) memcpy(&elf[e_shoff], d + shdr_off, shnum * 64ull);
    for (u32 s = 0; s < sections; s++) {
        const u8* e = sec + s * 0x30;
        const u64 off = be64(e), len = be64(e + 8);
        const u32 type = be32(e + 16), prog = be32(e + 20), encrypted = be32(e + 32);
        const u32 key = be32(e + 36), iv = be32(e + 40), compressed = be32(e + 44);
        if (type != 2) continue;   // (1: section headers, 3: SCE version: not part of the ELF)
        if (prog >= phnum || off + len > size) return damaged;
        std::vector<u8> data(d + off, d + off + len);
        if (encrypted == 3) {
            if (key >= key_slots || iv >= key_slots) return damaged;
            ctr_crypt(Aes(keys + key * 16, 128), keys + iv * 16, data.data(), data.size());
        }
        const u8* ph = d + phdr_off + prog * 56;
        const u64 p_off = be64(ph + 8), p_filesz = be64(ph + 32);
        if (compressed == 2) {
            Inflate z(data.data(), data.size());
            if (!z.run(p_filesz)) return damaged;
            data.swap(z.out);
        }
        if (data.size() > p_filesz) data.resize(p_filesz);
        memcpy(&elf[p_off], data.data(), data.size());
    }
    return {};
}

uint64_t image_hash(const std::vector<u8>& elf) {
    if (elf.size() < 64 || memcmp(elf.data(), "\x7f" "ELF\x02\x02", 6)) return 0;
    u64 h = 0xcbf29ce484222325ull;
    auto mix = [&](const u8* p, size_t n) {
        for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 0x100000001b3ull;
    };
    const u64 phoff = be64(&elf[32]);
    const u16 phnum = be16(&elf[56]);
    if (phoff + phnum * 56ull > elf.size()) return 0;
    mix(&elf[24], 8);   // e_entry
    for (u16 i = 0; i < phnum; i++) {
        const u8* ph = &elf[phoff + i * 56];
        const u64 off = be64(ph + 8), fsz = be64(ph + 32);
        if (off + fsz > elf.size()) return 0;
        mix(ph, 56);
        mix(elf.data() + off, fsz);
    }
    return h ? h : 1;
}

std::string load(const std::filesystem::path& game, std::vector<u8>& elf) {
    namespace fs = std::filesystem;
    const fs::path bin = game / "USRDIR" / "EBOOT.BIN", plain = game / "USRDIR" / "EBOOT.elf";
    const char* other = "is not the executable this build was made from (F1 Championship Edition BCES00005, "
                        "version 01.00, from the disc): another version or region, a game update or a damaged copy.";
    std::error_code ec;
    std::string bin_problem;
    if (fs::is_regular_file(bin, ec)) {
        std::vector<u8> self = read_file(bin);
        if (self.size() >= 4 && !memcmp(self.data(), "\x7f" "ELF", 4)) elf.swap(self);   // (already decrypted)
        else bin_problem = decrypt_self(self, elf);
        if (bin_problem.empty()) {
            if (image_hash(elf) == EXPECTED_HASH) return {};
            bin_problem = other;
        }
        bin_problem = "USRDIR/EBOOT.BIN " + bin_problem;
    }
    // a missing or unusable EBOOT.BIN: an EBOOT.elf decrypted elsewhere, if it is the right one
    if (fs::is_regular_file(plain, ec)) {
        elf = read_file(plain);
        const u64 h = image_hash(elf);
        if (h == EXPECTED_HASH) return {};
        if (bin_problem.empty()) {
            elf.clear();
            return std::string("USRDIR/EBOOT.elf ") + (h ? other : "is not a decrypted PPU executable.") +
                   " Put the disc's original USRDIR/EBOOT.BIN in the folder instead.";
        }
    }
    elf.clear();
    return bin_problem.empty() ? "USRDIR/EBOOT.BIN was not found in this PS3_GAME folder." : bin_problem;
}

}  // namespace eboot
