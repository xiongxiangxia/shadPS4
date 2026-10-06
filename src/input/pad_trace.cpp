// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "input/pad_trace.h"

#ifdef ENABLE_PADTRACE
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <fmt/format.h>
#include "common/logging/log.h"
#include "common/path_util.h"
#include "common/scm_rev.h"

namespace Input::PadTrace {
namespace {

constexpr std::size_t buffer_capacity = 65536;
constexpr u64 record_limit = 1000000;

struct Entry {
    u64 sequence;
    u64 monotonic_us;
    u64 thread;
    const char* kind;
    std::array<s64, 24> fields{};
};

struct Recorder {
    std::atomic<bool> available{false};
    std::atomic<bool> enabled{false};
    std::atomic<u64> next_id{1};
    std::mutex mutex;
    std::condition_variable wake;
    std::vector<Entry> pending;
    std::vector<std::string> configuration;
    std::ofstream file;
    std::thread writer;
    u64 sequence = 0;
    u64 dropped = 0;
    bool stopping = false;
};

Recorder& GetRecorder() {
    static auto* recorder = new Recorder;
    return *recorder;
}

thread_local u64 current_event = 0;

void WriteLoop() {
    auto& recorder = GetRecorder();
    std::vector<Entry> entries;
    entries.reserve(buffer_capacity);
    for (;;) {
        u64 dropped;
        u64 sequence;
        bool stopping;
        std::vector<std::string> configuration;
        {
            std::unique_lock lock{recorder.mutex};
            recorder.wake.wait_for(lock, std::chrono::milliseconds{250}, [&] {
                return recorder.stopping || recorder.pending.size() >= buffer_capacity / 2;
            });
            entries.swap(recorder.pending);
            dropped = recorder.dropped;
            sequence = recorder.sequence;
            configuration.swap(recorder.configuration);
            stopping = recorder.stopping;
        }
        std::string output;
        if (entries.empty() && configuration.empty() && !stopping) {
            continue;
        }
        output.reserve(entries.size() * 180);
        for (const auto& line : configuration) {
            fmt::format_to(std::back_inserter(output), "# config={}\n", line);
        }
        for (const auto& entry : entries) {
            fmt::format_to(std::back_inserter(output), "{},{},{},{}", entry.sequence,
                           entry.monotonic_us, entry.thread, entry.kind);
            for (const auto value : entry.fields) {
                fmt::format_to(std::back_inserter(output), ",{}", value);
            }
            output += '\n';
        }
        fmt::format_to(std::back_inserter(output), "# dropped={} limit_reached={}\n", dropped,
                       sequence >= record_limit);
        recorder.file.write(output.data(), static_cast<std::streamsize>(output.size()));
        recorder.file.flush();
        if (!recorder.file) {
            recorder.available = false;
            recorder.enabled = false;
            LOG_ERROR(Input, "[PADTRACE] Trace write failed; recording disabled");
            return;
        }
        entries.clear();
        if (stopping) {
            recorder.file.close();
            return;
        }
    }
}

} // namespace

void Initialize() {
    auto& recorder = GetRecorder();
    static std::once_flag initialized;
    std::call_once(initialized, [&] {
        const char* setting = std::getenv("SHADPS4_PADTRACE");
        if (setting && std::string_view{setting} == "0") {
            return;
        }
        const auto session = std::chrono::duration_cast<std::chrono::microseconds>(
                                 std::chrono::system_clock::now().time_since_epoch())
                                 .count();
        const auto path = Common::FS::GetUserPath(Common::FS::PathType::LogDir) /
                          fmt::format("padtrace-v2-{}.csv", session);
        recorder.file.open(path, std::ios::out | std::ios::binary);
        if (!recorder.file) {
            LOG_ERROR(Input, "[PADTRACE] Cannot open trace file");
            return;
        }
        recorder.file << "# PADTRACE v2 revision=" << Common::g_scm_rev << '\n';
        recorder.file << "seq,monotonic_us,thread,kind";
        for (int i = 0; i < 24; ++i) {
            recorder.file << ",p" << i;
        }
        recorder.file << '\n';
        recorder.pending.reserve(buffer_capacity);
        recorder.available = true;
        recorder.enabled = setting && std::string_view{setting} == "1";
        recorder.writer = std::thread{WriteLoop};
        std::atexit(Shutdown);
        std::at_quick_exit(Shutdown);
        LOG_INFO(Input, "[PADTRACE] Trace file {}; F9 toggles recording (currently {})",
                 Common::FS::PathToUTF8String(path), recorder.enabled.load() ? "ON" : "OFF");
    });
}

void Shutdown() {
    auto& recorder = GetRecorder();
    recorder.available = false;
    recorder.enabled = false;
    {
        std::lock_guard lock{recorder.mutex};
        recorder.stopping = true;
    }
    recorder.wake.notify_one();
    if (recorder.writer.joinable()) {
        recorder.writer.join();
    }
}

bool Enabled() {
    return GetRecorder().enabled.load(std::memory_order_relaxed);
}

bool Toggle() {
    auto& recorder = GetRecorder();
    if (!recorder.available.load(std::memory_order_relaxed)) {
        return false;
    }
    const bool was_enabled = Enabled();
    if (was_enabled) {
        Record("CAPTURE", {0});
    }
    {
        std::lock_guard lock{recorder.mutex};
        if (recorder.stopping || recorder.sequence >= record_limit) {
            LOG_WARNING(Input, "[PADTRACE] Recording limit reached; restart to capture again");
            return true;
        }
        recorder.enabled = !was_enabled;
    }
    if (!was_enabled) {
        Record("CAPTURE", {1});
    }
    recorder.wake.notify_one();
    LOG_INFO(Input, "[PADTRACE] Recording {}", was_enabled ? "OFF" : "ON");
    return true;
}

u64 NextId() {
    return Enabled() ? GetRecorder().next_id.fetch_add(1, std::memory_order_relaxed) : 0;
}

u64 CurrentEvent() {
    return current_event;
}

void Record(const char* kind, std::initializer_list<s64> fields) {
    if (!Enabled()) {
        return;
    }
    auto& recorder = GetRecorder();
    Entry entry{};
    entry.kind = kind;
    entry.monotonic_us = std::chrono::duration_cast<std::chrono::microseconds>(
                             std::chrono::steady_clock::now().time_since_epoch())
                             .count();
    static thread_local const u64 thread_id =
        std::hash<std::thread::id>{}(std::this_thread::get_id());
    entry.thread = thread_id;
    std::copy_n(fields.begin(), std::min(fields.size(), entry.fields.size()), entry.fields.begin());
    std::lock_guard lock{recorder.mutex};
    if (recorder.stopping || !recorder.enabled.load(std::memory_order_relaxed)) {
        return;
    }
    entry.sequence = ++recorder.sequence;
    if (entry.sequence > record_limit) {
        recorder.enabled = false;
        return;
    }
    if (recorder.pending.size() == buffer_capacity) {
        ++recorder.dropped;
        return;
    }
    recorder.pending.push_back(entry);
}

void Configuration(std::string_view text) {
    if (!GetRecorder().available.load(std::memory_order_relaxed)) {
        return;
    }
    auto& recorder = GetRecorder();
    std::lock_guard lock{recorder.mutex};
    if (!recorder.stopping) {
        recorder.configuration.emplace_back(text);
    }
}

void ClockSample(u32 api, u64 value, u64 frequency) {
    if (!Enabled() || api >= 3) {
        return;
    }
    struct Sample {
        u64 value = 0;
        u64 calls = 0;
        std::chrono::steady_clock::time_point recorded{};
    };
    static thread_local std::array<Sample, 3> samples;
    auto& sample = samples[api];
    const auto now = std::chrono::steady_clock::now();
    const bool backwards = sample.calls != 0 && value < sample.value;
    const u64 previous = sample.value;
    sample.value = value;
    ++sample.calls;
    if (backwards || now - sample.recorded >= std::chrono::milliseconds{10}) {
        Record("CLOCK", {api, static_cast<s64>(value), static_cast<s64>(frequency),
                         static_cast<s64>(sample.calls), backwards, static_cast<s64>(previous),
                         static_cast<s64>(CurrentEvent())});
        sample.recorded = now;
    }
}

EventScope::EventScope() : previous{current_event} {
    current_event = NextId();
}

EventScope::~EventScope() {
    current_event = previous;
}

} // namespace Input::PadTrace
#else
namespace Input::PadTrace {
void Initialize() {}
void Shutdown() {}
bool Enabled() {
    return false;
}
bool Toggle() {
    return false;
}
u64 NextId() {
    return 0;
}
u64 CurrentEvent() {
    return 0;
}
void Record(const char*, std::initializer_list<s64>) {}
void Configuration(std::string_view) {}
void ClockSample(u32, u64, u64) {}
EventScope::EventScope() : previous{0} {}
EventScope::~EventScope() {}
} // namespace Input::PadTrace
#endif
