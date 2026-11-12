#include "ini.h"

#include <cstdio>
#include <cstdlib>
#include <strings.h>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {
bool valid_utf8(const std::string& s) {
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = (unsigned char)s[i];
        const int n = c < 0x80 ? 0 : (c >> 5) == 6 ? 1 : (c >> 4) == 14 ? 2 : (c >> 3) == 30 ? 3 : -1;
        if (n < 0 || i + n >= s.size() + (n ? 0 : 1)) return false;
        for (int k = 1; k <= n; k++)
            if (((unsigned char)s[i + k] >> 6) != 2) return false;
        i += n + 1;
    }
    return true;
}
std::string from_ansi(const std::string& s) {
#ifdef _WIN32
    const int wn = MultiByteToWideChar(CP_ACP, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(wn, L'\0');
    MultiByteToWideChar(CP_ACP, 0, s.data(), (int)s.size(), w.data(), wn);
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), wn, nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), wn, out.data(), n, nullptr, nullptr);
    return out;
#else
    std::string out;   // (Latin-1)
    for (unsigned char c : s) {
        if (c < 0x80) out += (char)c;
        else { out += (char)(0xc0 | c >> 6); out += (char)(0x80 | (c & 0x3f)); }
    }
    return out;
#endif
}
FILE* open_file(const std::string& path, const char* mode) {
#ifdef _WIN32
    auto wide = [](const std::string& s) {
        const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
        std::wstring w(n, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
        w.resize(n ? n - 1 : 0);
        return w;
    };
    return _wfopen(wide(path).c_str(), wide(mode).c_str());
#else
    return fopen(path.c_str(), mode);
#endif
}
std::string trim(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t\r");
    if (a == std::string::npos) return {};
    return s.substr(a, s.find_last_not_of(" \t\r") + 1 - a);
}
}  // namespace

bool Ini::load(const std::string& path) {
    sections_.assign(1, Section{});
    FILE* f = open_file(path, "rb");
    if (!f) return false;
    std::string text;
    char buf[8192];
    for (size_t n; (n = fread(buf, 1, sizeof(buf), f)) > 0;) text.append(buf, n);
    fclose(f);
    if (text.compare(0, 3, "\xEF\xBB\xBF") == 0) text.erase(0, 3);
    if (!valid_utf8(text)) text = from_ansi(text);
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(pos, end - pos);
        pos = end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::string t = trim(line);
        if (t.size() > 1 && t[0] == '[' && t.back() == ']') {
            sections_.push_back({trim(t.substr(1, t.size() - 2)), {}});
            continue;
        }
        const size_t eq = t.find('=');
        if (t.empty() || t[0] == ';' || t[0] == '#' || eq == std::string::npos || eq == 0)
            sections_.back().lines.push_back({{}, {}, line});
        else
            sections_.back().lines.push_back({trim(t.substr(0, eq)), trim(t.substr(eq + 1)), {}});
    }
    return true;
}

bool Ini::save(const std::string& path) const {
    std::string text;
    for (size_t i = 0; i < sections_.size(); i++) {
        const Section& s = sections_[i];
        if (i > 0) text += "[" + s.name + "]\r\n";
        for (const Line& l : s.lines) text += (l.key.empty() ? l.raw : l.key + "=" + l.value) + "\r\n";
    }
    // (written next to it, then moved over it: a crash never leaves half a file)
    const std::string tmp = path + ".tmp";
    FILE* f = open_file(tmp, "wb");
    if (!f) return false;
    const bool ok = fwrite(text.data(), 1, text.size(), f) == text.size();
    if (fclose(f) != 0 || !ok) return false;
#ifdef _WIN32
    auto wide = [](const std::string& s) {
        const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
        std::wstring w(n, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
        return w;
    };
    return MoveFileExW(wide(tmp).c_str(), wide(path).c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
#else
    return rename(tmp.c_str(), path.c_str()) == 0;
#endif
}

Ini::Section* Ini::find(const std::string& name) {
    for (size_t i = 1; i < sections_.size(); i++)
        if (!strcasecmp(sections_[i].name.c_str(), name.c_str())) return &sections_[i];
    return nullptr;
}
const Ini::Section* Ini::find(const std::string& name) const { return const_cast<Ini*>(this)->find(name); }

const std::string* Ini::get(const std::string& section, const std::string& key) const {
    if (const Section* s = find(section))
        for (const Line& l : s->lines)
            if (!l.key.empty() && !strcasecmp(l.key.c_str(), key.c_str())) return &l.value;
    return nullptr;
}
std::string Ini::get(const std::string& section, const std::string& key, const std::string& def) const {
    const std::string* v = get(section, key);
    return v ? *v : def;
}
int Ini::get_int(const std::string& section, const std::string& key, int def) const {
    const std::string* v = get(section, key);
    return v && !v->empty() ? atoi(v->c_str()) : def;
}

void Ini::set(const std::string& section, const std::string& key, const std::string& value) {
    if (sections_.empty()) sections_.assign(1, Section{});
    Section* s = find(section);
    if (!s) {
        sections_.push_back({section, {}});
        s = &sections_.back();
    }
    for (Line& l : s->lines)
        if (!l.key.empty() && !strcasecmp(l.key.c_str(), key.c_str())) {
            l.value = value;
            return;
        }
    // (after the section's last key, before trailing blank lines)
    size_t at = s->lines.size();
    while (at > 0 && s->lines[at - 1].key.empty() && trim(s->lines[at - 1].raw).empty()) at--;
    s->lines.insert(s->lines.begin() + at, Line{key, value, {}});
}

void Ini::erase(const std::string& section, const std::string& key) {
    if (Section* s = find(section))
        for (size_t i = 0; i < s->lines.size(); i++)
            if (!s->lines[i].key.empty() && !strcasecmp(s->lines[i].key.c_str(), key.c_str())) {
                s->lines.erase(s->lines.begin() + i);
                return;
            }
}

std::vector<std::pair<std::string, std::string>> Ini::keys(const std::string& section) const {
    std::vector<std::pair<std::string, std::string>> out;
    if (const Section* s = find(section))
        for (const Line& l : s->lines)
            if (!l.key.empty()) out.push_back({l.key, l.value});
    return out;
}
