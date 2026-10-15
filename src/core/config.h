#pragma once
// Settings file reader (<iniName> from mc/game_info.h): plain "key = value" lines, '#' or ';' comments, [section]
// headers ignored. Keys are case-insensitive. Launch-time overrides come from environment variables <prefix><KEY>
// (set by the launcher from its command line, e.g. --join 1.2.3.4 -> MAFIACOOP_JOIN=1.2.3.4) and win over the file.
// Set() changes a value at runtime (it then wins over both) and writes it into the file. Thread-safe.
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

namespace mc::core {

class Config {
public:
    bool Load(const std::wstring& path);  // false if the file is missing (env overrides still apply)
    // Prefix of the override variables, e.g. "MAFIACOOP_" (GameInfo::envPrefix). Empty (default): no overrides.
    void SetEnvPrefix(std::string prefix) { _envPrefix = std::move(prefix); }

    bool Has(const std::string& key) const;
    std::string GetString(const std::string& key, const std::string& def = {}) const;
    int GetInt(const std::string& key, int def = 0) const;
    bool GetBool(const std::string& key, bool def = false) const;

    // Sets key = value for this process (wins over the environment and the file) and saves it into the settings
    // file: the first "key = value" line for that key is replaced (comments and other lines are kept), else a line
    // is appended. The file is written next to itself and then moved over it. Returns false if it could not be
    // saved (the value is still set for this process). No file was loaded (empty Path()): only sets it.
    bool Set(const std::string& key, const std::string& value);

    const std::wstring& Path() const { return _path; }
    const std::unordered_map<std::string, std::string>& Values() const { return _values; }

    static Config& Get();  // process-wide instance used by the dll

private:
    bool Lookup(const std::string& key, std::string& out) const;

    std::wstring _path;
    std::string _envPrefix;
    std::unordered_map<std::string, std::string> _values;  // lower-case keys
    std::unordered_map<std::string, std::string> _set;     // Set() values, lower-case keys
    mutable std::mutex _mutex;                              // _values, _set
};

} // namespace mc::core
