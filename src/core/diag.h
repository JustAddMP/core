#pragma once
// Session diagnostics registry: named numbers a game layer (or core) publishes for the periodic session summaries
// (SUM records every 10 s, a log line every 60 s) and the end-of-session summary (FIN), see dll/session_log.h.
//
//   Set(name, v)    a level (copies shown now, the leash distance): the summary shows the latest value
//   Total(name, v)  a running count the caller keeps (snaps so far): the summary shows what it added since the last
//                   summary and in all; a count that went back (its owner was reset) adds from 0 again
//   Add(name, n)    a count kept here (events): the same as Total for the summaries
//
// Names are short tokens without spaces or '=' ("traffic.copies", "vis.ped.show"); others are rejected. At most
// kMaxNames names (later new ones are dropped and counted). Thread-safe and cheap (a lock and a map lookup): fine at
// event rate and for a few dozen values a second, not for every entity every frame.
#include <cstdint>
#include <string>
#include <vector>

namespace mc::core::diag {

constexpr size_t kMaxNames = 512;

void Set(const char* name, double value);
void Total(const char* name, uint64_t value);
void Add(const char* name, uint64_t n = 1);
// Forgets everything (a new session).
void Reset();

struct Value {
    std::string name;
    bool count = false;    // Total / Add (else a level)
    double level = 0;      // a level's latest value
    uint64_t sum = 0;      // a count's total since Reset
    uint64_t delta = 0;    // a count's increase since the last TakeDeltas
};
// Every value, by name. TakeDeltas also starts the next delta window (one caller: the periodic summary).
std::vector<Value> Snapshot();
std::vector<Value> TakeDeltas();
uint64_t Dropped();  // new names refused (kMaxNames or a bad name)

// "name=v name=d/t ..." in name order, split into lines of at most maxChars (no value is cut). Pure (tests).
// withDelta: counts as <delta>/<total>, else only <total>.
std::vector<std::string> Format(const std::vector<Value>& values, size_t maxChars, bool withDelta);
bool ValidName(const char* name);

// The bookkeeping of one count (pure; tests): a new reading of the owner's running count.
void Accumulate(uint64_t& last, uint64_t& sum, uint64_t& delta, uint64_t reading);

}  // namespace mc::core::diag
