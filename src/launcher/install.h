#pragma once
// MafiaCoop.exe --install / --uninstall: the optional "start from Steam" mode (src/proxy/proxy_shared.h).
// --install copies mafiacoop_proxy.dll into the game folder as version.dll and writes mafiacoop_path.txt (the full
// path of the mafiacoop.dll next to MafiaCoop.exe) after a confirmation dialog. It refuses when a version.dll or
// mafiacoop_path.txt that is not ours is already there. --uninstall deletes exactly those two files, and only if
// they are ours. No other file in the game folder is read for writing, renamed or removed.
#include <string>

namespace mc::launcher {

struct InstallIo {
    void (*log)(const std::wstring& line);    // launcher log
    void (*print)(const std::wstring& line);  // console (with --yes)
    bool console = false;                     // --yes: no dialogs, results on the console
};

// gameExeOrDir: the game exe or its folder. selfDir: MafiaCoop.exe's folder (holds mafiacoop_proxy.dll and
// mafiacoop.dll). Exit codes: 0 done (or nothing to do), 1 error, 3 refused (foreign file, game running) or cancelled.
int InstallProxy(const std::wstring& gameExeOrDir, const std::wstring& selfDir, const InstallIo& io);
int UninstallProxy(const std::wstring& gameExeOrDir, const InstallIo& io);

} // namespace mc::launcher
