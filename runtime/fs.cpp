// PS3 path mapping and file I/O on the host file system.
#include "lv2_fs.h"
#include "mods.h"

#include <cstdio>
#include <filesystem>
#include <vector>

namespace stdfs = std::filesystem;

namespace {
enum : s32 {
    O_WRONLY = 1, O_RDWR = 2, O_ACCMODE = 3, O_CREAT = 0x40, O_EXCL = 0x80, O_TRUNC = 0x200, O_APPEND = 0x400,
};

struct File {
    FILE* f = nullptr;
    std::string path;
    std::shared_ptr<mods::VirtualFile> vf;   // an archive changed by the mod pack (f stays null)
    u64 vpos = 0;
};
struct Dir {
    std::vector<std::pair<std::string, bool>> entries;   // name, is_dir
    size_t pos = 0;
};
std::mutex g_fs_mutex;
std::unordered_map<u32, File> g_files;
std::unordered_map<u32, Dir> g_dirs;
u32 g_next_fd = 3;

// Case-insensitive lookup of each path component (the game was authored for a case-insensitive FS).
stdfs::path resolve_case(const stdfs::path& p) {
    if (stdfs::exists(p)) return p;
    stdfs::path cur = p.root_path();
    for (const auto& part : p.relative_path()) {
        stdfs::path next = cur / part;
        if (!stdfs::exists(next) && stdfs::is_directory(cur)) {
            std::string want = PATH_STR(part);
            for (auto& ch : want) ch = (char)tolower((unsigned char)ch);
            for (const auto& e : stdfs::directory_iterator(cur)) {
                std::string n = PATH_STR(e.path().filename());
                for (auto& ch : n) ch = (char)tolower((unsigned char)ch);
                if (n == want) { next = e.path(); break; }
            }
        }
        cur = next;
    }
    return cur;
}

void write_stat(u32 out, bool dir, u64 size) {
    st32(out + 0, dir ? (0040000 | 0777) : (0100000 | 0666));
    st32(out + 4, 0);
    st32(out + 8, 0);
    u64 t = 1199145600;   // 2008-01-01
    st64(out + 12, t);
    st64(out + 20, t);
    st64(out + 28, t);
    st64(out + 36, size);
    st64(out + 44, 0x10000);
}
}  // namespace

std::string fs_host_path(const std::string& g) {
    auto starts = [&](const char* p) { return g.rfind(p, 0) == 0; };
    stdfs::path game = stdfs::u8path(g_game_dir);
    stdfs::path out;
    if (starts("/app_home/")) out = game / "USRDIR" / stdfs::u8path(g.substr(10));
    else if (starts("/dev_bdvd/PS3_GAME/")) out = game / stdfs::u8path(g.substr(19));
    else if (starts("/dev_bdvd/")) out = game.parent_path() / stdfs::u8path(g.substr(10));
    else if (starts("/host_root/")) out = game / "USRDIR" / stdfs::u8path(g.substr(11));
    else if (starts("/dev_hdd0/") || starts("/dev_hdd1/") || starts("/dev_usb000/"))
        out = stdfs::u8path(g_data_dir) / stdfs::u8path(g.substr(1));
    else out = game / "USRDIR" / stdfs::u8path(g[0] == '/' ? g.substr(1) : g);
    return PATH_STR(resolve_case(out.lexically_normal()));
}

namespace fs {

s32 open(const std::string& path, s32 flags, u32 fd_out) {
    std::string host = fs_host_path(path);
    stdfs::path hp = stdfs::u8path(host);
    bool exists = stdfs::exists(hp);
    if (exists && stdfs::is_directory(hp)) return CELL_EISDIR;
    if (!exists && !(flags & O_CREAT)) {
        LOG_W("fs open: %s -> not found (%s)", path.c_str(), host.c_str());
        return CELL_ENOENT;
    }
    if (exists && (flags & O_CREAT) && (flags & O_EXCL)) return CELL_EEXIST;
    const char* mode = "rb";
    int acc = flags & O_ACCMODE;
    if (acc == 0 && exists) {
        std::string repl = mods::loose_override(host);
        if (!repl.empty()) {
            host = repl;
            hp = stdfs::u8path(host);
        }
        if (std::shared_ptr<mods::VirtualFile> vf = mods::open_wad(host)) {
            std::lock_guard lk(g_fs_mutex);
            u32 fd = g_next_fd++;
            g_files[fd] = {nullptr, path, std::move(vf)};
            st32(fd_out, fd);
            LOG_I("fs open: %s flags=%x -> fd %u (mod pack archive)", path.c_str(), flags, fd);
            return CELL_OK;
        }
    }
    if (acc != 0) {
        if (!exists || (flags & O_TRUNC)) mode = acc == O_WRONLY ? "wb" : "w+b";
        else mode = (flags & O_APPEND) ? "a+b" : "r+b";
        stdfs::create_directories(hp.parent_path());
    }
#ifdef _WIN32
    FILE* f = _wfopen(hp.wstring().c_str(), std::wstring(mode, mode + strlen(mode)).c_str());
#else
    FILE* f = fopen(host.c_str(), mode);
#endif
    if (!f) return CELL_ENOENT;
    std::lock_guard lk(g_fs_mutex);
    u32 fd = g_next_fd++;
    g_files[fd] = {f, path};
    st32(fd_out, fd);
    LOG_I("fs open: %s flags=%x -> fd %u", path.c_str(), flags, fd);
    return CELL_OK;
}

s32 read(u32 fd, u32 buf, u64 size, u32 nread_out) {
    std::lock_guard lk(g_fs_mutex);
    auto it = g_files.find(fd);
    if (it == g_files.end()) return CELL_EBADF;
    File& fl = it->second;
    size_t n;
    if (fl.vf) {
        n = fl.vf->read(fl.vpos, gmem(buf), (size_t)size);
        fl.vpos += n;
    } else n = fread(gmem(buf), 1, (size_t)size, fl.f);
    if (nread_out) st64(nread_out, n);
    return CELL_OK;
}

s32 write(u32 fd, u32 buf, u64 size, u32 nwrite_out) {
    std::lock_guard lk(g_fs_mutex);
    auto it = g_files.find(fd);
    if (it == g_files.end()) return CELL_EBADF;
    if (it->second.vf) return CELL_EBADF;   // opened read-only
    size_t n = fwrite(gmem(buf), 1, (size_t)size, it->second.f);
    if (nwrite_out) st64(nwrite_out, n);
    return CELL_OK;
}

s32 close(u32 fd) {
    std::lock_guard lk(g_fs_mutex);
    auto it = g_files.find(fd);
    if (it == g_files.end()) return CELL_EBADF;
    if (it->second.f) fclose(it->second.f);
    g_files.erase(it);
    return CELL_OK;
}

s32 lseek(u32 fd, s64 offset, u32 whence, u32 pos_out) {
    std::lock_guard lk(g_fs_mutex);
    auto it = g_files.find(fd);
    if (it == g_files.end()) return CELL_EBADF;
    if (File& fl = it->second; fl.vf) {
        s64 pos = offset + (whence == 0 ? 0 : whence == 1 ? (s64)fl.vpos : (s64)fl.vf->size());
        if (pos < 0) return CELL_EINVAL;
        fl.vpos = (u64)pos;
        if (pos_out) st64(pos_out, (u64)pos);
        return CELL_OK;
    }
    int w = whence == 0 ? SEEK_SET : whence == 1 ? SEEK_CUR : SEEK_END;
#ifdef _WIN32
    if (_fseeki64(it->second.f, offset, w)) return CELL_EINVAL;
    s64 pos = _ftelli64(it->second.f);
#else
    if (fseeko(it->second.f, offset, w)) return CELL_EINVAL;
    s64 pos = ftello(it->second.f);
#endif
    if (pos_out) st64(pos_out, (u64)pos);
    return CELL_OK;
}

s32 stat(const std::string& path, u32 out) {
    std::string host = fs_host_path(path);
    stdfs::path hp = stdfs::u8path(host);
    std::error_code ec;
    if (!stdfs::exists(hp, ec)) return CELL_ENOENT;
    bool dir = stdfs::is_directory(hp, ec);
    if (!dir) {
        std::string repl = mods::loose_override(host);
        if (!repl.empty()) hp = stdfs::u8path(host = repl);
        if (s64 vs = mods::wad_size(host); vs >= 0) {
            write_stat(out, false, (u64)vs);
            return CELL_OK;
        }
    }
    write_stat(out, dir, dir ? 0 : (u64)stdfs::file_size(hp, ec));
    return CELL_OK;
}

s32 fstat(u32 fd, u32 out) {
    std::string path;
    {
        std::lock_guard lk(g_fs_mutex);
        auto it = g_files.find(fd);
        if (it == g_files.end()) return CELL_EBADF;
        if (it->second.vf) {
            write_stat(out, false, it->second.vf->size());
            return CELL_OK;
        }
        fflush(it->second.f);
        path = it->second.path;
    }
    return stat(path, out);
}

s32 opendir(const std::string& path, u32 fd_out) {
    stdfs::path hp = stdfs::u8path(fs_host_path(path));
    std::error_code ec;
    if (!stdfs::is_directory(hp, ec)) return CELL_ENOENT;
    Dir d;
    d.entries.push_back({".", true});
    d.entries.push_back({"..", true});
    for (const auto& e : stdfs::directory_iterator(hp, ec)) d.entries.push_back({PATH_STR(e.path().filename()), e.is_directory()});
    std::lock_guard lk(g_fs_mutex);
    u32 fd = g_next_fd++;
    g_dirs[fd] = std::move(d);
    st32(fd_out, fd);
    return CELL_OK;
}

// CellFsDirent { u8 d_type; u8 d_namlen; char d_name[256]; }
s32 readdir(u32 fd, u32 out, u32 nread_out) {
    std::lock_guard lk(g_fs_mutex);
    auto it = g_dirs.find(fd);
    if (it == g_dirs.end()) return CELL_EBADF;
    Dir& d = it->second;
    if (d.pos >= d.entries.size()) {
        st64(nread_out, 0);
        return CELL_OK;
    }
    auto& [name, isdir] = d.entries[d.pos++];
    st8(out, isdir ? 1 : 2);
    st8(out + 1, (u8)std::min<size_t>(name.size(), 255));
    gstr_write(out + 2, name, 256);
    st64(nread_out, 258);
    return CELL_OK;
}

s32 closedir(u32 fd) {
    std::lock_guard lk(g_fs_mutex);
    return g_dirs.erase(fd) ? CELL_OK : CELL_EBADF;
}

s32 mkdir(const std::string& path) {
    std::error_code ec;
    stdfs::path hp = stdfs::u8path(fs_host_path(path));
    if (stdfs::exists(hp, ec)) return CELL_EEXIST;
    stdfs::create_directories(hp, ec);
    return ec ? CELL_EPERM : CELL_OK;
}
s32 rename(const std::string& from, const std::string& to) {
    std::error_code ec;
    stdfs::rename(stdfs::u8path(fs_host_path(from)), stdfs::u8path(fs_host_path(to)), ec);
    return ec ? CELL_ENOENT : CELL_OK;
}
s32 rmdir(const std::string& path) {
    std::error_code ec;
    stdfs::remove(stdfs::u8path(fs_host_path(path)), ec);
    return ec ? CELL_ENOENT : CELL_OK;
}
s32 unlink(const std::string& path) { return rmdir(path); }
s32 truncate(const std::string& path, u64 size) {
    std::error_code ec;
    stdfs::resize_file(stdfs::u8path(fs_host_path(path)), size, ec);
    return ec ? CELL_ENOENT : CELL_OK;
}
s32 ftruncate(u32 fd, u64 size) {
    std::string path;
    {
        std::lock_guard lk(g_fs_mutex);
        auto it = g_files.find(fd);
        if (it == g_files.end()) return CELL_EBADF;
        fflush(it->second.f);
        path = it->second.path;
    }
    return truncate(path, size);
}

}  // namespace fs
