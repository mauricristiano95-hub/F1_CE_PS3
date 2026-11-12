// f1ce_launcher.ini without the Windows profile API (the launcher also builds for Linux): sections and keys kept in
// their order, comments and unknown keys kept as they were. Read as UTF-8, or as the ANSI code page when it is not
// valid UTF-8 (files written by the Win32 launcher); written as UTF-8.
#pragma once
#include <string>
#include <vector>

class Ini {
public:
    bool load(const std::string& path);   // false: no file (the Ini is empty)
    bool save(const std::string& path) const;
    // nullptr: no such key
    const std::string* get(const std::string& section, const std::string& key) const;
    std::string get(const std::string& section, const std::string& key, const std::string& def) const;
    int get_int(const std::string& section, const std::string& key, int def) const;
    void set(const std::string& section, const std::string& key, const std::string& value);
    void erase(const std::string& section, const std::string& key);
    std::vector<std::pair<std::string, std::string>> keys(const std::string& section) const;

private:
    struct Line { std::string key, value, raw; };   // key empty: raw (comment, blank)
    struct Section { std::string name; std::vector<Line> lines; };
    std::vector<Section> sections_;   // [0]: the lines before any section (name "")
    Section* find(const std::string& name);
    const Section* find(const std::string& name) const;
};
