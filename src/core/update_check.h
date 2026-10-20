#pragma once
// Update check (core 0.13): once per game start the mod asks https://justaddmp.com/api/version/<id> (GameInfo::
// updateId) for the newest release of the mod and, when it is newer than GameInfo::modVersion, the co-op UI shows a
// notice at the main menu. The request runs on its own thread with short timeouts; nothing here ever blocks the game,
// and every failure (offline, timeout, odd answer) only writes a log line.
//
// The version parsing, comparison and answer parsing below take no game and no network, so tests/update_check.cpp
// calls them directly.
//
// Testing with a fake older version: start the launcher with --update-as 0.1.0 (the dll reads <envPrefix>UPDATE_AS
// and checks as if that were its version). --update-url <url> points the check at another server.
#include <string>
#include <string_view>

namespace mc::core::update {

// "v0.15.0", "0.15", "1.2.3-beta.2", "V2.0.0+build5": an optional v, one to three numeric parts (missing ones are 0),
// an optional pre-release after '-' (dot-separated identifiers) and an ignored build suffix after '+'.
struct Version {
    unsigned major = 0, minor = 0, patch = 0;
    std::string pre;  // "" = a release
};

// False for anything else (empty, letters in a numeric part, more than three parts, numbers over 999999999, ...).
bool ParseVersion(std::string_view text, Version* out);

// <0, 0, >0. A release is newer than any pre-release of the same numbers; pre-releases compare identifier by
// identifier (numeric ones as numbers and below alphanumeric ones; a longer list wins when all else is equal).
int Compare(const Version& a, const Version& b);

// True only when both parse and `latest` is newer than `current`.
bool IsNewer(std::string_view latest, std::string_view current);

// The endpoint's answer: {"mod":"mafia-de","latest":"v0.16.0","url":"https://...",...}. latest is "" when the mod
// has no release yet ("latest":null). False when the text is not such an answer.
struct Answer {
    std::string latest, url;
};
bool ParseAnswer(std::string_view json, Answer* out);

// "v0.16.0" -> "v0.16.0", "0.16.0" -> "v0.16.0" (how versions are shown to players).
std::string Display(std::string_view version);

// The notice's text, e.g. "MafiaCoop v0.16.0 is out (you have v0.15.0). Get it at justaddmp.com".
std::string NoticeText(std::string_view modName, std::string_view latest, std::string_view current);

constexpr const char* kDefaultUrl = "https://justaddmp.com/api/version/";

// ---- the check (dll) ----------------------------------------------------------------------------------------------
enum class State { Idle, Running, UpToDate, NewerAvailable, Failed };

// Starts the check on a thread of its own (at most once per process; later calls do nothing). url: the full URL to
// ask; current: the version to compare with. Returns at once.
void Start(const std::string& url, const std::string& current);

// The check's state; when NewerAvailable, *latest gets the newer version as the endpoint wrote it. Any thread.
State Poll(std::string* latest = nullptr);

// Blocking fetch used by Start's thread (and the tests): GET url, each step (name lookup, connect, send,
// receive) limited to timeoutMs. False on any failure
// (bad URL, no connection, timeout, status other than 200, body over 16 KB); *error says what, for the log.
bool Fetch(const std::string& url, unsigned timeoutMs, std::string* body, std::string* error);

}  // namespace mc::core::update
