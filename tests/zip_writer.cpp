// zip_writer: CRC-32, the streaming deflate and the zip container (src/core/zip_writer.h), checked with a small
// inflate of its own (RFC 1951: stored, fixed and dynamic blocks) below: what was written must come back byte for
// byte, from any write sizes, for text, random data, long runs and empty input; a zip's central directory points at
// its entries and their CRCs match. Exits non-zero on any failure.
//
// zip_writer.exe <out.zip> <file>...   instead writes those files into a zip and prints the speed and the ratio
// (measurements; not part of the automated run).
#include <windows.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "core/zip_writer.h"
#include "support/zip_read.h"

namespace {

using namespace mc::testzip;

int g_failures = 0;

void Check(bool ok, const char* name, const std::string& detail = {}) {
    std::printf("%s  %-60s %s\n", ok ? "PASS" : "FAIL", name, detail.c_str());
    std::fflush(stdout);
    if (!ok) ++g_failures;
}

std::vector<uint8_t> Deflate(const std::vector<uint8_t>& data, size_t writeSize) {
    std::vector<uint8_t> out;
    mc::core::zip::Deflater d([&](const uint8_t* p, size_t n) {
        out.insert(out.end(), p, p + n);
        return true;
    });
    for (size_t at = 0; at < data.size(); at += writeSize)
        d.Write(data.data() + at, std::min(writeSize, data.size() - at));
    d.Finish();
    return out;
}

std::vector<uint8_t> TraceLike(size_t bytes, uint32_t seed) {
    std::mt19937 rng(seed);
    std::string s;
    double t = 1234.5;
    while (s.size() < bytes) {
        char buf[256];
        t += 0.016 + (rng() % 100) / 100000.0;
        const int kind = static_cast<int>(rng() % 4);
        if (kind == 0)
            std::snprintf(buf, sizeof buf, "%.4f TX sT=%u p=%.3f,%.3f,%.3f h=%.3f v=%.2f m=%u f=%u hp=100 sh=0\r\n", t,
                          static_cast<unsigned>(t * 1000), -100 + (rng() % 20000) / 100.0, 50 + (rng() % 2000) / 100.0,
                          3.0 + (rng() % 100) / 100.0, (rng() % 628) / 100.0, (rng() % 600) / 100.0, rng() % 4,
                          rng() % 8);
        else if (kind == 1)
            std::snprintf(buf, sizeof buf, "%.4f PUP id=1 rT=%u tgt=%.3f,%.3f,%.3f err=%.3f g=%u fix=%u\r\n", t,
                          static_cast<unsigned>(t * 1000 - 80), (rng() % 20000) / 100.0, (rng() % 2000) / 100.0, 3.1,
                          (rng() % 300) / 1000.0, rng() % 4, rng() % 3);
        else if (kind == 2)
            std::snprintf(buf, sizeof buf, "%.4f TRC net=%u rT=%.1f cp=%.3f,%.3f,%.3f err=%.3f pin=0 pins=0 f=8\r\n", t,
                          rng() % 300, t * 1000 - 100, (rng() % 20000) / 100.0, (rng() % 2000) / 100.0, 3.0,
                          (rng() % 2000) / 1000.0);
        else
            std::snprintf(buf, sizeof buf, "%.4f 2026-10-20 14:36:%02u.%03u [12345] INF net: player 1 rtt %u ms\r\n", t,
                          rng() % 60, rng() % 1000, rng() % 40);
        s += buf;
    }
    s.resize(bytes);
    return {s.begin(), s.end()};
}

bool RoundTrip(const std::vector<uint8_t>& data, size_t writeSize, double* ratio = nullptr) {
    const auto comp = Deflate(data, writeSize);
    std::vector<uint8_t> back;
    const bool ok = InflateAll(comp, back) && back == data;
    if (ratio) *ratio = comp.empty() ? 0 : static_cast<double>(data.size()) / static_cast<double>(comp.size());
    return ok;
}

int Measure(int argc, wchar_t** argv) {
    mc::core::zip::ZipWriter z;
    if (!z.Open(argv[1])) {
        std::printf("cannot create %ls\n", argv[1]);
        return 1;
    }
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 2; i < argc; ++i) {
        std::wstring p = argv[i];
        const size_t slash = p.find_last_of(L"\\/");
        std::wstring base = slash == std::wstring::npos ? p : p.substr(slash + 1);
        std::string name;
        for (wchar_t c : base) name += c < 128 ? static_cast<char>(c) : '_';
        z.AddFile(p, name);
    }
    const bool ok = z.Close();
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    uint64_t in = 0, out = 0;
    for (const auto& e : z.Info()) {
        std::printf("  %-50s %10llu -> %10llu bytes (%.2fx)%s%s\n", e.name.c_str(),
                    static_cast<unsigned long long>(e.in), static_cast<unsigned long long>(e.out),
                    e.out ? static_cast<double>(e.in) / static_cast<double>(e.out) : 0.0, e.note.empty() ? "" : "  ",
                    e.note.c_str());
        in += e.in;
        out += e.out;
    }
    std::printf("%s: %.1f MB in, %.1f MB out, ratio %.2fx, %.2f s, %.1f MB/s\n", ok ? "ok" : "FAILED", in / 1e6,
                out / 1e6, out ? static_cast<double>(in) / static_cast<double>(out) : 0.0, s, s > 0 ? in / 1e6 / s : 0.0);
    return ok ? 0 : 1;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc >= 3) return Measure(argc, argv);
    using mc::core::zip::Crc32;
    const char* nine = "123456789";
    Check(Crc32(0, nine, 9) == 0xCBF43926u, "crc32: the check value of \"123456789\"");
    Check(Crc32(Crc32(0, nine, 4), nine + 4, 5) == 0xCBF43926u, "crc32: in two parts");
    Check(Crc32(0, nullptr, 0) == 0, "crc32: empty");

    Check(RoundTrip({}, 1), "deflate: empty input");
    const std::string hello = "hello, hello, hello world";
    Check(RoundTrip({hello.begin(), hello.end()}, 1), "deflate: a short text, written byte by byte");
    std::vector<uint8_t> zeros(300000, 0);
    double r = 0;
    bool ok = RoundTrip(zeros, 65536, &r);
    Check(ok && r > 100, "deflate: 300 KB of zeros (long matches)", std::to_string(r) + "x");
    std::mt19937 rng(7);
    std::vector<uint8_t> random(200000);
    for (auto& b : random) b = static_cast<uint8_t>(rng());
    ok = RoundTrip(random, 7777, &r);
    Check(ok && r > 0.95, "deflate: 200 KB random bytes (no matches)", std::to_string(r) + "x");
    const auto trace = TraceLike(3u << 20, 1);
    ok = RoundTrip(trace, 1 << 20, &r);
    Check(ok, "deflate: 3 MB of trace-like text comes back");
    Check(r > 3.0, "deflate: ... and is at least 3x smaller (random digits; real traces more)", std::to_string(r) + "x");
    Check(RoundTrip(trace, 333), "deflate: the same text in 333-byte writes");
    std::vector<uint8_t> mixed;
    for (int i = 0; i < 40; ++i) {
        const auto& part = (i % 3 == 0) ? random : trace;
        mixed.insert(mixed.end(), part.begin(), part.begin() + 50000 + i * 1000);
    }
    Check(RoundTrip(mixed, 4096), "deflate: text and random data mixed (many blocks)");
    std::vector<uint8_t> period;
    for (int i = 0; i < 100000; ++i) period.push_back(static_cast<uint8_t>("abcabcabd"[i % 9]));
    Check(RoundTrip(period, 100), "deflate: a short period (overlapping matches)");
    std::vector<uint8_t> edge;  // matches 32767 bytes back
    std::vector<uint8_t> block(32767);
    for (auto& b : block) b = static_cast<uint8_t>(rng());
    for (int i = 0; i < 4; ++i) edge.insert(edge.end(), block.begin(), block.end());
    Check(RoundTrip(edge, 50000), "deflate: repeats at the window's edge");

    // the container
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    const std::wstring dir = std::wstring(tmp) + L"mc-zip-test";
    CreateDirectoryW(dir.c_str(), nullptr);
    const std::wstring src = dir + L"\\a.trace", zipPath = dir + L"\\t.zip";
    {
        std::ofstream f(src, std::ios::binary);
        f.write(reinterpret_cast<const char*>(trace.data()), static_cast<std::streamsize>(trace.size()));
    }
    mc::core::zip::ZipWriter z;
    Check(z.Open(zipPath), "zip: opens");
    Check(z.AddFile(src, "trace/a.trace"), "zip: a file from disk");
    Check(!z.AddFile(dir + L"\\missing.txt", "missing.txt"), "zip: a missing file is left out");
    Check(z.AddFile(src, "cut.trace", 1000), "zip: a file cut to 1000 bytes");
    const std::string manifest = "manifest\r\nline two\r\n";
    Check(z.AddData("manifest.txt", manifest.data(), manifest.size()), "zip: data from memory");
    Check(z.AddData("empty.txt", "", 0), "zip: an empty entry");
    Check(z.Close(), "zip: closes");
    bool notes = z.Info().size() == 5 && !z.Info()[1].written && !z.Info()[1].note.empty() &&
                 z.Info()[2].note.find("cut") != std::string::npos;
    Check(notes, "zip: the entries' notes (missing, cut)");
    std::vector<std::pair<std::string, std::vector<uint8_t>>> entries;
    const bool read = ReadZip(ReadFileBytes(zipPath), entries);
    Check(read && entries.size() == 4, "zip: central directory, local headers and CRCs",
          std::to_string(entries.size()) + " entries");
    if (read && entries.size() == 4) {
        Check(entries[0].first == "trace/a.trace" && entries[0].second == trace, "zip: the file comes back");
        Check(entries[1].second.size() == 1000 && std::equal(entries[1].second.begin(), entries[1].second.end(),
                                                             trace.begin()),
              "zip: the cut file is its first 1000 bytes");
        Check(std::string(entries[2].second.begin(), entries[2].second.end()) == manifest, "zip: the memory entry");
        Check(entries[3].second.empty(), "zip: the empty entry");
    }
    DeleteFileW(src.c_str());
    DeleteFileW(zipPath.c_str());
    RemoveDirectoryW(dir.c_str());
    std::printf("%s (%d failure%s)\n", g_failures ? "FAILED" : "ALL PASSED", g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
