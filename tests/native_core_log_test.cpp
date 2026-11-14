#include "oreslang/runtime/CoreLog.hpp"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdint>
#include <limits>
#include <string>
#include <thread>
#include <vector>

static std::string dump(const oreslang::CoreLog& log) {
    std::FILE* out = std::tmpfile();
    assert(out != nullptr);
    log.dump(out);
    std::rewind(out);
    std::string text;
    char bytes[4096];
    for (;;) {
        const auto n = std::fread(bytes, 1, sizeof(bytes), out);
        if (n == 0) break;
        text.append(bytes, n);
    }
    std::fclose(out);
    return text;
}

static std::size_t count_substr(const std::string& haystack,
                                const std::string& needle) {
    std::size_t count = 0, offset = 0;
    while ((offset = haystack.find(needle, offset)) != std::string::npos) {
        ++count;
        offset += needle.size();
    }
    return count;
}

int main() {
    using Log = oreslang::CoreLog;
    Log::Level result = Log::Level::Off;
    for (const auto name : {"OFF", "error", "Warn", "info", "debug", "TRACE"})
        assert(Log::parse_level(name, result));
    for (const auto name : {"", "none", "info ", "fatal", "notice", "trace\n", "é"})
        assert(!Log::parse_level(name, result));

    // Disabled by default: no diagnostics are emitted.
    Log off;
    off.record(Log::Level::Error, Log::EventId::CompileFailed, 17);
    assert(!off.enabled(Log::Level::Error));
    assert(dump(off).empty());

    Log warn(Log::Level::Warn);
    warn.record(Log::Level::Info, Log::EventId::CompileFailed, 111);
    warn.record(Log::Level::Warn, Log::EventId::CompileFailed,
                std::numeric_limits<std::int64_t>::min());
    warn.record(Log::Level::Error, Log::EventId::InputRejected,
                std::numeric_limits<std::int64_t>::max());
    warn.record(Log::Level::Warn, static_cast<Log::EventId>(999), 333);
    const auto text = dump(warn);
    assert(text.find("\"event\":\"compiler.failed\"") != std::string::npos);
    assert(text.find("\"event\":\"compiler.input.rejected\"") != std::string::npos);
    assert(text.find("\"severity\":\"warn\"") != std::string::npos);
    assert(text.find("\"severity\":\"error\"") != std::string::npos);
    assert(text.find(std::to_string(std::numeric_limits<std::int64_t>::min()))
           != std::string::npos);
    assert(text.find(std::to_string(std::numeric_limits<std::int64_t>::max()))
           != std::string::npos);
    assert(text.find("\"value\":111") == std::string::npos);
    assert(text.find("\"value\":333") == std::string::npos);
    assert(text.find("\"recorded\":2,\"reserved\":2,\"unpublished\":0,\"saturated\":false")
           != std::string::npos);

    // 6 concurrent native threads race for 8192 slots; all finished rows
    // must be complete, with no user-controlled string representation.
    Log parallel(Log::Level::Trace);
    std::vector<std::thread> workers;
    for (int t = 0; t < 6; ++t) {
        workers.emplace_back([&parallel] {
            for (int i = 0; i < 5000; ++i)
                parallel.record(Log::Level::Error, Log::EventId::CompileFailed, 23);
        });
    }
    for (auto& worker : workers) worker.join();
    const auto events = dump(parallel);
    assert(count_substr(events, "\"kind\":\"event\"") == Log::capacity);
    assert(events.find("\"recorded\":8192,\"reserved\":8192,\"unpublished\":0,\"saturated\":true")
           != std::string::npos);
    assert(count_substr(events, "\"value\":23") == Log::capacity);
    assert(events.find("private-source-path") == std::string::npos);
    std::puts("NATIVE_CORE_LOG_FIXED_RECORDS_AND_CONCURRENCY_PASSED");
}
