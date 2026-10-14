#pragma once
// Line logger: timestamp + thread id, every line written straight to the file (no user-mode buffering).
// File: <dir>\<fileName>, previous run rotated to <fileName>.1.
#include <cstdarg>
#include <string>

namespace mc::core::log {

enum class Level { Debug, Info, Warn, Error };

// Opens <dir>\<fileName> (creating <dir>), rotating an existing file to <fileName>.1. Safe to call once.
bool Init(const std::wstring& dir, const wchar_t* fileName);
void Shutdown();
bool IsOpen();

void Write(Level level, const char* fmt, ...);
void WriteV(Level level, const char* fmt, va_list args);
// For the crash handler: does not block forever on the log lock.
void WriteUnsafe(Level level, const char* fmt, ...);

} // namespace mc::core::log

#define MC_LOG_DEBUG(...) ::mc::core::log::Write(::mc::core::log::Level::Debug, __VA_ARGS__)
#define MC_LOG_INFO(...)  ::mc::core::log::Write(::mc::core::log::Level::Info, __VA_ARGS__)
#define MC_LOG_WARN(...)  ::mc::core::log::Write(::mc::core::log::Level::Warn, __VA_ARGS__)
#define MC_LOG_ERROR(...) ::mc::core::log::Write(::mc::core::log::Level::Error, __VA_ARGS__)
