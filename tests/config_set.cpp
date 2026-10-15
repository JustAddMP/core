// config_set: Config::Set (src/core/config.h) changes a value for the process and saves it into the settings file
// without touching the other lines; it wins over the file and over the launcher's environment overrides. Works in a
// temp folder. Exits non-zero on any failure.
#include <windows.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "core/config.h"

namespace {

int g_failures = 0;

void Check(bool ok, const char* name, const std::string& detail = {}) {
    std::printf("%s  %-52s %s\n", ok ? "PASS" : "FAIL", name, detail.c_str());
    std::fflush(stdout);
    if (!ok) ++g_failures;
}

std::string ReadAll(const std::wstring& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void WriteAll(const std::wstring& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

}  // namespace

int main() {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    const std::wstring dir = std::wstring(tmp) + L"mc-config-set-test";
    CreateDirectoryW(dir.c_str(), nullptr);
    const std::wstring ini = dir + L"\\test.ini";

    // 1. replace an existing key, keep comments, other keys and a section header
    WriteAll(ini, "; settings\r\n[main]\r\nname = Old\r\nModel = paulie   ; inline is part of the value\r\nport = 1\r\n");
    mc::core::Config c;
    c.SetEnvPrefix("MC_CONFIG_SET_TEST_");
    Check(c.Load(ini), "load");
    Check(c.GetString("model") == "paulie   ; inline is part of the value", "value before", c.GetString("model"));
    Check(c.Set("model", "sam"), "set existing key");
    Check(c.GetString("model") == "sam", "value after set", c.GetString("model"));
    const std::string after = ReadAll(ini);
    Check(after == "; settings\r\n[main]\r\nname = Old\r\nmodel = sam\r\nport = 1\r\n", "file: only that line changed",
          after);

    // 2. a new key is appended
    Check(c.Set("friendly_fire", "0"), "set new key");
    Check(ReadAll(ini).find("port = 1\r\nfriendly_fire = 0\r\n") != std::string::npos, "file: new key appended");
    Check(c.GetInt("friendly_fire", 1) == 0, "new key readable");

    // 3. a fresh Config sees the saved values
    mc::core::Config c2;
    c2.Load(ini);
    Check(c2.GetString("model") == "sam" && c2.GetString("name") == "Old", "reload sees saved values");

    // 4. Set wins over the environment override; the file keeps the new value too
    SetEnvironmentVariableW(L"MC_CONFIG_SET_TEST_NAME", L"FromEnv");
    Check(c.GetString("name") == "FromEnv", "env wins over the file");
    Check(c.Set("name", "Picked"), "set while env override exists");
    Check(c.GetString("name") == "Picked", "set wins over env", c.GetString("name"));
    SetEnvironmentVariableW(L"MC_CONFIG_SET_TEST_NAME", nullptr);

    // 5. rejected input: keys/values with line breaks or '=' in the key
    Check(!c.Set("bad=key", "x"), "reject '=' in key");
    Check(!c.Set("key", "two\nlines"), "reject newline in value");
    Check(ReadAll(ini).find("two") == std::string::npos, "file untouched by rejected set");

    // 6. missing file: Set creates it
    const std::wstring ini2 = dir + L"\\missing.ini";
    DeleteFileW(ini2.c_str());
    mc::core::Config c3;
    Check(!c3.Load(ini2), "load missing file");
    Check(c3.Set("model", "salieri"), "set creates the file");
    Check(ReadAll(ini2) == "model = salieri\r\n", "created file content", ReadAll(ini2));

    // 7. no file at all (never loaded): only in memory
    mc::core::Config c4;
    Check(c4.Set("model", "frank") && c4.GetString("model") == "frank", "in-memory set without a file");

    DeleteFileW(ini.c_str());
    DeleteFileW(ini2.c_str());
    RemoveDirectoryW(dir.c_str());
    std::printf("%s (%d failure(s))\n", g_failures ? "FAILED" : "ALL PASSED", g_failures);
    return g_failures ? 1 : 0;
}
