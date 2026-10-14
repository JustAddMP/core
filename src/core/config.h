#pragma once
// MafiaCoop.ini reader: plain "key = value" lines, '#' or ';' comments, [section] headers ignored.
// Keys are case-insensitive. Launch-time overrides come from environment variables MAFIACOOP_<KEY>
// (set by MafiaCoop.exe from its command line, e.g. --join 1.2.3.4 -> MAFIACOOP_JOIN=1.2.3.4) and win over the file.
#include <string>
#include <unordered_map>

namespace mc::core {

class Config {
public:
    bool Load(const std::wstring& path);  // false if the file is missing (env overrides still apply)

    bool Has(const std::string& key) const;
    std::string GetString(const std::string& key, const std::string& def = {}) const;
    int GetInt(const std::string& key, int def = 0) const;
    bool GetBool(const std::string& key, bool def = false) const;

    const std::wstring& Path() const { return _path; }
    const std::unordered_map<std::string, std::string>& Values() const { return _values; }

    static Config& Get();  // process-wide instance used by the dll

private:
    bool Lookup(const std::string& key, std::string& out) const;

    std::wstring _path;
    std::unordered_map<std::string, std::string> _values;  // lower-case keys
};

} // namespace mc::core
