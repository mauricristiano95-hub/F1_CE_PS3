// HLE: cellSaveData (List/Auto Save/Load, Delete).
//
// The save API is callback driven: the system hands the game a directory list, then the directory
// status, then asks repeatedly for file operations until the game reports it is done. Where the PS3
// shows its save data list (list save / load, delete) the player chooses in an overlay dialog
// (overlay.h), as on the console and in RPCS3: every profile is a directory (P000, P001, ...), and
// the game's "new data" item creates another. Without a dialog (F1CE_SAVE_UI=0, headless) the
// existing directory, or the game's new-data directory, is selected automatically.
//
// Storage: ps3_data/dev_hdd0/home/00000001/savedata/<DIRNAME>/ with the game's files plus
// "_params.bin" holding the system file parameters block the game provides.
#include "kernel.h"
#include "overlay.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <vector>

namespace stdfs = std::filesystem;
extern std::string g_title_id;   // loader.cpp

namespace {
enum : s32 {
    CBRESULT_OK_LAST_NOCONFIRM = 2, CBRESULT_OK_LAST = 1, CBRESULT_OK_NEXT = 0,
    RET_CANCEL = 1,   // CELL_SAVEDATA_RET_CANCEL: the player backed out of the list
    ERROR_CBRESULT = (s32)0x8002B401, ERROR_NODATA = (s32)0x8002B410,
};
enum : u32 { OP_READ = 0, OP_WRITE = 1, OP_DELETE = 2, OP_WRITE_NOTRUNC = 3 };

// structure sizes / offsets used by the save API
constexpr u32 CBRESULT_SIZE = 0x20;
constexpr u32 DIRLIST_ENTRY = 0x30;          // dirName[32], listParam[8], reserved[8]
constexpr u32 SYSPARAM_SIZE = 0x610;         // title, subtitle, detail, attribute, listParam, reserved
constexpr u32 FILESTAT_SIZE = 0x38;          // type, size, times, fileName[13]
constexpr u32 STATGET_DIR = 0x08;            // dir stat inside stat-get: 3 times + dirName[32]
constexpr u32 STATGET_PARAM = 0x08 + 0x38;   // system file params
constexpr u32 STATGET_TAIL = STATGET_PARAM + SYSPARAM_SIZE;   // bind, sizeKB, sysSizeKB, fileNum, fileListNum, fileList

std::mutex g_save_mutex;   // one save operation at a time (also protects the scratch block)
u32 g_scratch = 0;         // guest scratch memory for the structures handed to callbacks

stdfs::path save_root() {
    return stdfs::u8path(g_data_dir) / "dev_hdd0" / "home" / "00000001" / "savedata";
}

std::vector<std::string> split_prefixes(const std::string& s) {
    std::vector<std::string> v;
    size_t pos = 0;
    while (pos <= s.size()) {
        size_t e = s.find('|', pos);
        v.push_back(s.substr(pos, e == std::string::npos ? std::string::npos : e - pos));
        if (e == std::string::npos) break;
        pos = e + 1;
    }
    return v;
}

bool read_file(const stdfs::path& p, std::vector<u8>& data) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    data.assign((std::istreambuf_iterator<char>(f)), {});
    return true;
}

// a save directory as the list shows it
struct DirInfo {
    std::string name;
    s64 mtime = 0;              // newest file, seconds since the epoch
    u64 size = 0;               // bytes
    std::vector<u8> params;     // _params.bin (the system file parameters), SYSPARAM_SIZE bytes
};

s64 epoch_seconds(stdfs::file_time_type t) {
    using namespace std::chrono;
    return duration_cast<seconds>(t - stdfs::file_time_type::clock::now() + system_clock::now().time_since_epoch()).count();
}

DirInfo dir_info(const std::string& name) {
    DirInfo d;
    d.name = name;
    const stdfs::path path = save_root() / stdfs::u8path(name);
    std::error_code ec;
    for (auto& e : stdfs::directory_iterator(path, ec)) {
        if (!e.is_regular_file(ec) || e.path().extension() == ".tmp") continue;
        d.size += e.file_size(ec);
        d.mtime = std::max(d.mtime, epoch_seconds(e.last_write_time(ec)));
    }
    read_file(path / "_params.bin", d.params);
    d.params.resize(SYSPARAM_SIZE);
    return d;
}

std::string param_str(const DirInfo& d, u32 off, u32 len) {   // title 0x000, subtitle 0x080, detail 0x100
    const char* p = (const char*)d.params.data() + off;
    return std::string(p, strnlen(p, len));
}

// the directories matching the prefixes ("A|B"), in the order the game asked for (CellSaveDataSetList sortType:
// 0 modified time, 1 subtitle; sortOrder: 0 descending, 1 ascending)
std::vector<DirInfo> list_dirs(const std::string& prefixes, u32 sort_type, u32 sort_order) {
    std::vector<DirInfo> out;
    std::error_code ec;
    if (!stdfs::is_directory(save_root(), ec)) return out;
    auto pre = split_prefixes(prefixes);
    for (auto& e : stdfs::directory_iterator(save_root(), ec)) {
        if (!e.is_directory()) continue;
        std::string n = PATH_STR(e.path().filename());
        for (auto& p : pre)
            if (!p.empty() && n.rfind(p, 0) == 0) { out.push_back(dir_info(n)); break; }
    }
    std::sort(out.begin(), out.end(), [&](const DirInfo& a, const DirInfo& b) {
        if (sort_type == 1) {
            const std::string sa = param_str(a, 0x80, 128), sb = param_str(b, 0x80, 128);
            if (sa != sb) return sort_order == 1 ? sa < sb : sa > sb;
        } else if (a.mtime != b.mtime) {
            return sort_order == 1 ? a.mtime < b.mtime : a.mtime > b.mtime;
        }
        return a.name < b.name;
    });
    return out;
}

overlay::SaveEntry save_entry(const DirInfo& d) {
    overlay::SaveEntry e;
    e.dir = d.name;
    e.title = param_str(d, 0, 128);
    e.subtitle = param_str(d, 0x80, 128);
    e.detail = param_str(d, 0x100, 1024);
    if (e.title.empty()) e.title = d.name;
    e.mtime = d.mtime;
    e.size = d.size;
    read_file(save_root() / stdfs::u8path(d.name) / "ICON0.PNG", e.icon_png);
    return e;
}

s32 cb_result(u32 res) { return (s32)ld32(res); }

struct SaveOp {
    bool save;
    u32 set_buf, func_stat, func_file, userdata;
};

// status + file phases for one selected directory
s32 run_dir(PPUContext& c, const SaveOp& op, const std::string& dir, bool is_new) {
    stdfs::path path = save_root() / stdfs::u8path(dir);
    std::error_code ec;
    if (!op.save && !stdfs::is_directory(path, ec)) return ERROR_NODATA;
    u32 res = g_scratch, stat_get = g_scratch + 0x40, stat_set = g_scratch + 0x800;
    u32 file_get = g_scratch + 0x840, file_set = g_scratch + 0x8C0;
    memset(gmem(g_scratch), 0, 0x1000);
    st32(res + 16, op.userdata);

    // ---- status
    st32(stat_get + 0, 1024 * 1024);   // free space (KB)
    st32(stat_get + 4, is_new ? 1 : 0);
    gstr_write(stat_get + STATGET_DIR + 24, dir, 32);
    std::vector<u8> params;
    if (read_file(path / "_params.bin", params))
        memcpy(gmem(stat_get + STATGET_PARAM), params.data(), std::min<size_t>(params.size(), SYSPARAM_SIZE));
    // file list: written into the game's buffer (setBuf->buf), limited by setBuf->fileListMax
    u32 file_max = ld32(op.set_buf + 4), buf = ld32(op.set_buf + 0x24), buf_size = ld32(op.set_buf + 0x20);
    u32 nfiles = 0, nlisted = 0, size_kb = 0;
    if (stdfs::is_directory(path, ec)) {
        for (auto& e : stdfs::directory_iterator(path, ec)) {
            std::string n = PATH_STR(e.path().filename());
            if (!e.is_regular_file() || n == "_params.bin" || e.path().extension() == ".tmp") continue;
            nfiles++;
            u64 sz = e.file_size();
            size_kb += (u32)((sz + 1023) / 1024);
            if (nlisted < file_max && (nlisted + 1) * FILESTAT_SIZE <= buf_size) {
                u32 fs = buf + nlisted * FILESTAT_SIZE;
                memset(gmem(fs), 0, FILESTAT_SIZE);
                st32(fs, 1);   // normal file
                st64(fs + 8, sz);
                gstr_write(fs + 0x28, n, 13);
                nlisted++;
            }
        }
    }
    st32(stat_get + STATGET_TAIL + 4, size_kb);
    st32(stat_get + STATGET_TAIL + 8, 64);
    st32(stat_get + STATGET_TAIL + 12, nfiles);
    st32(stat_get + STATGET_TAIL + 16, nlisted);
    st32(stat_get + STATGET_TAIL + 20, buf);
    guest_call(c, op.func_stat, {res, stat_get, stat_set});
    s32 r = cb_result(res);
    LOG_I("savedata: %s '%s' status callback -> %d", op.save ? "save" : "load", dir.c_str(), r);
    if (r < 0) return ERROR_CBRESULT;
    if (op.save) {
        // CellSaveDataStatSet: setParam, reCreateMode (2 YES / 3 YES_RESET_OWNER: the directory starts empty), indicator
        const u32 recreate = ld32(stat_set + 4) & 0xFFFF;
        if ((recreate == 2 || recreate == 3) && stdfs::is_directory(path, ec)) {
            for (auto& e : stdfs::directory_iterator(path, ec))
                if (e.is_regular_file()) stdfs::remove(e.path(), ec);
            LOG_I("savedata: '%s' recreated", dir.c_str());
        }
        stdfs::create_directories(path, ec);
        if (u32 sp = ld32(stat_set)) {
            std::ofstream f(path / "_params.bin", std::ios::binary);
            f.write((const char*)gmem(sp), SYSPARAM_SIZE);
        }
    }
    if (r == CBRESULT_OK_LAST || r == CBRESULT_OK_LAST_NOCONFIRM) return CELL_OK;

    // ---- files
    for (int guard = 0; guard < 1024; guard++) {
        memset(gmem(file_set), 0, 0x40);
        guest_call(c, op.func_file, {res, file_get, file_set});
        r = cb_result(res);
        if (r < 0) return ERROR_CBRESULT;
        if (r == CBRESULT_OK_LAST || r == CBRESULT_OK_LAST_NOCONFIRM) break;
        u32 fop = ld32(file_set), ftype = ld32(file_set + 8);
        std::string name = gstr(ld32(file_set + 0x1C), 64);
        u32 foff = ld32(file_set + 0x20), fsize = ld32(file_set + 0x24), fbuf_size = ld32(file_set + 0x28);
        u32 fbuf = ld32(file_set + 0x2C);
        if (ftype >= 2) {   // content files (icons, sound): name fixed by type
            static const char* names[] = {"", "", "ICON0.PNG", "ICON1.PAM", "PIC1.PNG", "SND0.AT3"};
            name = names[std::min<u32>(ftype, 5)];
        }
        stdfs::path fp = path / stdfs::u8path(name);
        u32 done = 0;
        switch (fop) {
            case OP_READ: {
                std::vector<u8> d;
                if (read_file(fp, d) && foff < d.size()) {
                    done = (u32)std::min<size_t>({d.size() - foff, (size_t)fsize, (size_t)fbuf_size});
                    memcpy(gmem(fbuf), d.data() + foff, done);
                }
                break;
            }
            case OP_WRITE: case OP_WRITE_NOTRUNC: {
                std::vector<u8> d;
                if (fop == OP_WRITE_NOTRUNC) read_file(fp, d);
                else d.clear();
                if (d.size() < (size_t)foff + fsize) d.resize((size_t)foff + fsize);
                memcpy(d.data() + foff, gmem(fbuf), fsize);
                // written next to it and renamed over it: a crash mid-write must not leave a broken save (the game
                // reports a corrupt profile and turns autosave off)
                stdfs::path tmp = fp;
                tmp += ".tmp";
                {
                    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
                    f.write((const char*)d.data(), (std::streamsize)d.size());
                    if (!f) { LOG_E("savedata: cannot write '%s'", PATH_STR(tmp).c_str()); break; }
                }
                stdfs::rename(tmp, fp, ec);
                if (ec) { LOG_E("savedata: cannot replace '%s': %s", PATH_STR(fp).c_str(), ec.message().c_str()); break; }
                done = fsize;
                break;
            }
            case OP_DELETE: stdfs::remove(fp, ec); break;
        }
        st32(file_get, done);   // excSize
        LOG_I("savedata: file op %u '%s' offset %u size %u -> %u bytes", fop, name.c_str(), foff, fsize, done);
    }
    return CELL_OK;
}

// list phase: hands the matching directories to the game, then the player picks the target directory
s32 run_list(PPUContext& c, bool save, u32 set_list, u32 set_buf, u32 func_list, u32 func_stat, u32 func_file,
             u32 userdata) {
    std::lock_guard lk(g_save_mutex);
    if (!g_scratch) g_scratch = mem::alloc(0x1000, 128);
    memset(gmem(g_scratch), 0, 0x1000);
    u32 res = g_scratch, list_get = g_scratch + 0x40, list_set = g_scratch + 0x100;
    st32(res + 16, userdata);
    std::string prefix = gstr(ld32(set_list + 8));
    const auto dirs = list_dirs(prefix, ld32(set_list), ld32(set_list + 4));
    u32 dir_max = ld32(set_buf), buf = ld32(set_buf + 0x24), buf_size = ld32(set_buf + 0x20);
    u32 n = 0;
    for (auto& d : dirs) {
        if (n >= dir_max || (n + 1) * DIRLIST_ENTRY > buf_size) break;
        u32 e = buf + n * DIRLIST_ENTRY;
        memset(gmem(e), 0, DIRLIST_ENTRY);
        gstr_write(e, d.name, 32);
        memcpy(gmem(e + 32), d.params.data() + 0x508, 8);   // listParam of the stored parameters
        n++;
    }
    st32(list_get + 0, (u32)dirs.size());
    st32(list_get + 4, n);
    st32(list_get + 8, buf);
    guest_call(c, func_list, {res, list_get, list_set});
    s32 r = cb_result(res);
    LOG_I("savedata: %s list (prefix '%s', %u dirs) -> callback %d", save ? "save" : "load", prefix.c_str(), n, r);
    if (r < 0) return ERROR_CBRESULT;
    if (r == CBRESULT_OK_LAST || r == CBRESULT_OK_LAST_NOCONFIRM) return CELL_OK;
    // CellSaveDataListSet: focusPosition, focusDirName, fixedListNum, fixedList, newData
    const u32 focus_pos = ld32(list_set), focus_dir = ld32(list_set + 4);
    const u32 fixed_num = ld32(list_set + 8), fixed = ld32(list_set + 12), new_data = ld32(list_set + 16);
    auto find = [&](const std::string& name) {
        return std::find_if(dirs.begin(), dirs.end(), [&](const DirInfo& d) { return d.name == name; });
    };
    // what the list shows: the game's fixed list (else every directory), and the new data item when saving
    std::vector<overlay::SaveEntry> entries;
    if (fixed_num && fixed) {
        for (u32 i = 0; i < fixed_num; i++)
            if (auto it = find(gstr(fixed + i * DIRLIST_ENTRY, 32)); it != dirs.end()) entries.push_back(save_entry(*it));
    } else {
        for (auto& d : dirs) entries.push_back(save_entry(d));
    }
    const std::string nd = save && new_data && ld32(new_data + 4) ? gstr(ld32(new_data + 4), 32) : "";
    if (!nd.empty() && find(nd) == dirs.end()) {
        // CellSaveDataListNewData: iconPosition (0 head, 1 tail), dirName, icon (title, iconBufSize, iconBuf: a PNG)
        overlay::SaveEntry e;
        e.dir = nd;
        e.is_new = true;
        e.title = "New Saved Data";
        if (u32 icon = ld32(new_data + 8)) {
            if (u32 t = ld32(icon); t && !gstr(t, 128).empty()) e.title = gstr(t, 128);
            const u32 size = ld32(icon + 4), png = ld32(icon + 8);
            if (size && png) e.icon_png.assign(gmem(png), gmem(png) + size);
        }
        if (ld32(new_data) == 0) entries.insert(entries.begin(), std::move(e));
        else entries.push_back(std::move(e));
    }
    if (entries.empty()) {
        if (overlay::message("There is no saved data.")) return RET_CANCEL;
        return save ? ERROR_CBRESULT : ERROR_NODATA;
    }
    // the focused item (focusPosition: 0 focusDirName, 1 list head, 2 list tail, 3 latest, 4 oldest, 5 new data)
    int focus = 0;
    for (int i = 0; i < (int)entries.size(); i++) {
        const overlay::SaveEntry& e = entries[i];
        const bool take = focus_pos == 0 ? focus_dir && e.dir == gstr(focus_dir, 32)
                        : focus_pos == 2 ? true
                        : focus_pos == 3 ? !e.is_new && (entries[focus].is_new || e.mtime > entries[focus].mtime)
                        : focus_pos == 4 ? !e.is_new && (entries[focus].is_new || e.mtime < entries[focus].mtime)
                        : focus_pos == 5 ? e.is_new
                                         : false;
        if (take) focus = i;
    }
    std::string dir;
    bool is_new = false;
    const int pick = overlay::save_list(save ? overlay::SaveMode::Save : overlay::SaveMode::Load, entries, focus);
    if (pick == -1) {
        LOG_I("savedata: %s list cancelled by the player", save ? "save" : "load");
        return RET_CANCEL;
    }
    if (pick >= 0) {
        dir = entries[pick].dir;
        is_new = entries[pick].is_new;
        LOG_I("savedata: the player chose '%s'%s", dir.c_str(), is_new ? " (new)" : "");
    } else {   // no dialog: the game's fixed list first, else the first directory, else its new-data directory
        if (fixed_num && fixed) dir = gstr(fixed, 32);
        else if (n) dir = gstr(buf, 32);
        if (!nd.empty() && (dir.empty() || !fixed_num) && find(nd) == dirs.end()) { dir = nd; is_new = true; }
    }
    if (dir.empty()) return save ? ERROR_CBRESULT : ERROR_NODATA;
    return run_dir(c, {save, set_buf, func_stat, func_file, userdata}, dir, is_new);
}

s32 run_auto(PPUContext& c, bool save, const std::string& dir, u32 set_buf, u32 func_stat, u32 func_file, u32 userdata) {
    std::lock_guard lk(g_save_mutex);
    if (!g_scratch) g_scratch = mem::alloc(0x1000, 128);
    std::error_code ec;
    bool exists = stdfs::is_directory(save_root() / stdfs::u8path(dir), ec);
    LOG_I("savedata: auto %s '%s' (%s)", save ? "save" : "load", dir.c_str(), exists ? "exists" : "new");
    return run_dir(c, {save, set_buf, func_stat, func_file, userdata}, dir, !exists);
}

// cellSaveDataDelete: the system's list of all the title's save data; the chosen directory is deleted
s32 run_delete() {
    std::lock_guard lk(g_save_mutex);
    const auto dirs = list_dirs(g_title_id, 0, 0);   // (newest first)
    std::vector<overlay::SaveEntry> entries;
    for (auto& d : dirs) entries.push_back(save_entry(d));
    if (entries.empty()) return overlay::message("There is no saved data.") ? RET_CANCEL : CELL_OK;
    const int pick = overlay::save_list(overlay::SaveMode::Delete, entries, 0);
    if (pick == overlay::UNAVAILABLE) {
        LOG_W("savedata: delete needs the save data dialog; nothing deleted");
        return CELL_OK;
    }
    if (pick < 0) return RET_CANCEL;
    std::error_code ec;
    stdfs::remove_all(save_root() / stdfs::u8path(entries[pick].dir), ec);
    if (ec) LOG_E("savedata: cannot delete '%s': %s", entries[pick].dir.c_str(), ec.message().c_str());
    else LOG_I("savedata: deleted '%s'", entries[pick].dir.c_str());
    return CELL_OK;
}
}  // namespace

namespace {
s32 logged(const char* what, s32 r) {
    if (r == RET_CANCEL) LOG_I("savedata: %s -> cancelled", what);
    else if (r != CELL_OK) LOG_W("savedata: %s -> %08x", what, (u32)r);
    else LOG_I("savedata: %s -> ok", what);
    return r;
}
}  // namespace

// cellSaveDataList{Save,Load}(version, setList, setBuf, funcList, funcStat, funcFile, container, userdata)
HLE_FUNC(cellSaveDataListSave) {
    RET(logged("list save", run_list(c, true, (u32)ARG(1), (u32)ARG(2), (u32)ARG(3), (u32)ARG(4), (u32)ARG(5), (u32)ARG(7))));
}
HLE_FUNC(cellSaveDataListLoad) {
    RET(logged("list load", run_list(c, false, (u32)ARG(1), (u32)ARG(2), (u32)ARG(3), (u32)ARG(4), (u32)ARG(5), (u32)ARG(7))));
}
// cellSaveDataAuto{Save,Load}(version, dirName, errDialog, setBuf, funcStat, funcFile, container, userdata)
HLE_FUNC(cellSaveDataAutoSave) {
    RET(logged("auto save", run_auto(c, true, gstr((u32)ARG(1), 32), (u32)ARG(3), (u32)ARG(4), (u32)ARG(5), (u32)ARG(7))));
}
HLE_FUNC(cellSaveDataAutoLoad) {
    RET(logged("auto load", run_auto(c, false, gstr((u32)ARG(1), 32), (u32)ARG(3), (u32)ARG(4), (u32)ARG(5), (u32)ARG(7))));
}
// cellSaveDataDelete(container): the system's deletion list
HLE_FUNC(cellSaveDataDelete) {
    RET(logged("delete", run_delete()));
}
