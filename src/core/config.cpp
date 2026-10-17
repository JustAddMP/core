#include "core/config.h"

#include <windows.h>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <vector>

namespace mc::core {
namespace {

std::string Trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

std::string Lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

} // namespace

Config& Config::Get() {
    static Config instance;
    return instance;
}

bool Config::Load(const std::wstring& path) {
    std::lock_guard lock(_mutex);
    _path = path;
    _values.clear();
    std::ifstream in(path);
    if (!in) return false;
    std::string line;
    bool first = true;
    while (std::getline(in, line)) {
        if (first && line.size() >= 3 && line.compare(0, 3, "\xEF\xBB\xBF") == 0) line.erase(0, 3);  // UTF-8 BOM
        first = false;
        std::string t = Trim(line);
        if (t.empty() || t[0] == '#' || t[0] == ';' || t[0] == '[') continue;
        size_t eq = t.find('=');
        if (eq == std::string::npos) continue;
        std::string key = Lower(Trim(t.substr(0, eq)));
        std::string value = Trim(t.substr(eq + 1));
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"') value = value.substr(1, value.size() - 2);
        if (!key.empty()) _values[key] = value;
    }
    return true;
}

bool Config::Lookup(const std::string& key, std::string& out) const {
    {
        std::lock_guard lock(_mutex);
        auto it = _set.find(Lower(key));
        if (it != _set.end()) {
            out = it->second;
            return true;
        }
    }
    std::string envName = _envPrefix;
    for (char c : key) {
        envName += (c == '.' || c == '-') ? '_' : static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    // Wide API + UTF-8: the launcher passes its UTF-16 command line through, and the ini is UTF-8. The ANSI API would
    // turn a non-ASCII --name or --password into codepage bytes (or '?') that match nothing else.
    const std::wstring wname(envName.begin(), envName.end());  // key: ASCII
    const DWORD need = _envPrefix.empty() ? 0 : GetEnvironmentVariableW(wname.c_str(), nullptr, 0);
    if (need > 1) {
        std::wstring w(need, L'\0');
        const DWORD n = GetEnvironmentVariableW(wname.c_str(), w.data(), need);
        if (n > 0 && n < need) {
            const int len = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(n), nullptr, 0, nullptr, nullptr);
            if (len > 0) {
                out.assign(static_cast<size_t>(len), '\0');
                WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(n), out.data(), len, nullptr, nullptr);
                return true;
            }
        }
    }
    std::lock_guard lock(_mutex);
    auto it = _values.find(Lower(key));
    if (it == _values.end()) return false;
    out = it->second;
    return true;
}

bool Config::Set(const std::string& key, const std::string& value) {
    const std::string lkey = Lower(Trim(key));
    if (lkey.empty() || lkey.find_first_of("=\r\n") != std::string::npos ||
        value.find_first_of("\r\n") != std::string::npos)
        return false;
    std::lock_guard lock(_mutex);
    _set[lkey] = value;
    if (_path.empty()) return true;
    // Rewrite the file: replace the first line for this key, keep everything else as it is.
    std::vector<std::string> lines;
    {
        std::ifstream in(_path, std::ios::binary);
        std::string line;
        while (in && std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            lines.push_back(line);
        }
    }
    const std::string entry = Trim(key) + " = " + value;
    bool replaced = false;
    for (std::string& line : lines) {
        std::string t = Trim(line);
        if (t.size() >= 3 && t.compare(0, 3, "\xEF\xBB\xBF") == 0) t = Trim(t.substr(3));  // UTF-8 BOM
        if (t.empty() || t[0] == '#' || t[0] == ';' || t[0] == '[') continue;
        const size_t eq = t.find('=');
        if (eq == std::string::npos || Lower(Trim(t.substr(0, eq))) != lkey) continue;
        line = entry;
        replaced = true;
        break;
    }
    if (!replaced) lines.push_back(entry);
    const std::wstring tmp = _path + L".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        for (const std::string& line : lines) out << line << "\r\n";
        if (!out.flush()) return false;
    }
    if (!MoveFileExW(tmp.c_str(), _path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    _values[lkey] = value;
    return true;
}

bool Config::Has(const std::string& key) const {
    std::string v;
    return Lookup(key, v);
}

std::string Config::GetString(const std::string& key, const std::string& def) const {
    std::string v;
    return Lookup(key, v) ? v : def;
}

int Config::GetInt(const std::string& key, int def) const {
    std::string v;
    if (!Lookup(key, v) || v.empty()) return def;
    char* end = nullptr;
    long r = std::strtol(v.c_str(), &end, 0);
    return (end && *end == '\0') ? static_cast<int>(r) : def;
}

std::string Config::GetFileString(const std::string& key, const std::string& def) const {
    std::lock_guard lock(_mutex);
    const std::string lkey = Lower(key);
    if (auto it = _set.find(lkey); it != _set.end()) return it->second;
    auto it = _values.find(lkey);
    return it == _values.end() ? def : it->second;
}

bool Config::GetBool(const std::string& key, bool def) const {
    std::string v;
    if (!Lookup(key, v)) return def;
    v = Lower(Trim(v));
    if (v == "1" || v == "true" || v == "yes" || v == "on") return true;
    if (v == "0" || v == "false" || v == "no" || v == "off") return false;
    return def;
}

} // namespace mc::core
