#include "core/diag.h"

#include <windows.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>

namespace mc::core::diag {
namespace {

struct Entry {
    bool count = false;
    double level = 0;
    uint64_t last = 0;  // Total: the owner's last reading
    uint64_t sum = 0, delta = 0;
};

SRWLOCK _lock = SRWLOCK_INIT;
std::map<std::string, Entry>* _map = nullptr;  // never destroyed
uint64_t _dropped = 0;

std::map<std::string, Entry>& Map() {
    if (!_map) _map = new std::map<std::string, Entry>;
    return *_map;
}

// Under the lock: the entry for name (created if there is room), nullptr = refused.
Entry* Find(const char* name, bool count) {
    auto& m = Map();
    auto it = m.find(name);
    if (it != m.end()) return &it->second;
    if (!ValidName(name) || m.size() >= kMaxNames) {
        ++_dropped;
        return nullptr;
    }
    Entry& e = m[name];
    e.count = count;
    return &e;
}

std::string FormatNumber(double v) {
    char buf[48];
    if (std::isfinite(v) && std::fabs(v - std::round(v)) < 1e-9 && std::fabs(v) < 1e15)
        std::snprintf(buf, sizeof buf, "%.0f", v);
    else if (std::isfinite(v))
        std::snprintf(buf, sizeof buf, "%.2f", v);
    else
        std::snprintf(buf, sizeof buf, "nan");
    return buf;
}

}  // namespace

bool ValidName(const char* name) {
    if (!name || !*name) return false;
    size_t n = 0;
    for (const char* c = name; *c; ++c, ++n) {
        const unsigned char u = static_cast<unsigned char>(*c);
        if (u <= ' ' || u == '=' || u >= 0x7F || n >= 48) return false;
    }
    return true;
}

void Accumulate(uint64_t& last, uint64_t& sum, uint64_t& delta, uint64_t reading) {
    const uint64_t add = reading >= last ? reading - last : reading;  // went back: its owner started from 0 again
    sum += add;
    delta += add;
    last = reading;
}

void Set(const char* name, double value) {
    AcquireSRWLockExclusive(&_lock);
    if (Entry* e = Find(name, false)) e->level = value;
    ReleaseSRWLockExclusive(&_lock);
}

void Total(const char* name, uint64_t value) {
    AcquireSRWLockExclusive(&_lock);
    if (Entry* e = Find(name, true)) Accumulate(e->last, e->sum, e->delta, value);
    ReleaseSRWLockExclusive(&_lock);
}

void Add(const char* name, uint64_t n) {
    AcquireSRWLockExclusive(&_lock);
    if (Entry* e = Find(name, true)) {
        e->sum += n;
        e->delta += n;
        e->last += n;
    }
    ReleaseSRWLockExclusive(&_lock);
}

void Reset() {
    AcquireSRWLockExclusive(&_lock);
    Map().clear();
    _dropped = 0;
    ReleaseSRWLockExclusive(&_lock);
}

namespace {
std::vector<Value> Collect(bool take) {
    std::vector<Value> out;
    AcquireSRWLockExclusive(&_lock);
    for (auto& [name, e] : Map()) {
        Value v;
        v.name = name;
        v.count = e.count;
        v.level = e.level;
        v.sum = e.sum;
        v.delta = e.delta;
        if (take) e.delta = 0;
        out.push_back(std::move(v));
    }
    ReleaseSRWLockExclusive(&_lock);
    return out;
}
}  // namespace

std::vector<Value> Snapshot() { return Collect(false); }
std::vector<Value> TakeDeltas() { return Collect(true); }

uint64_t Dropped() {
    AcquireSRWLockShared(&_lock);
    const uint64_t d = _dropped;
    ReleaseSRWLockShared(&_lock);
    return d;
}

std::vector<std::string> Format(const std::vector<Value>& values, size_t maxChars, bool withDelta) {
    std::vector<std::string> lines;
    std::string cur;
    for (const Value& v : values) {
        std::string item = v.name + "=";
        if (!v.count)
            item += FormatNumber(v.level);
        else if (withDelta)
            item += std::to_string(v.delta) + "/" + std::to_string(v.sum);
        else
            item += std::to_string(v.sum);
        if (!cur.empty() && cur.size() + 1 + item.size() > maxChars) {
            lines.push_back(std::move(cur));
            cur.clear();
        }
        if (!cur.empty()) cur += ' ';
        cur += item;
    }
    if (!cur.empty()) lines.push_back(std::move(cur));
    return lines;
}

}  // namespace mc::core::diag
