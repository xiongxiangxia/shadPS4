// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "input/dmc_charge_trace.h"

#if defined(ENABLE_PADTRACE) && defined(_WIN64)
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <windows.h>
#include "common/elf_info.h"
#include "common/logging/log.h"
#include "common/memory_patcher.h"
#include "common/types.h"
#include "input/pad_trace.h"

namespace Input::DmcChargeTrace {
namespace {
using Native = void(PS4_SYSV_ABI*)(void*, void*, void*);
using HeldNative = u32(PS4_SYSV_ABI*)(void*, u32);
using ClearNative = u32(PS4_SYSV_ABI*)(void*, void*);
using AggregateEvent = u32(PS4_SYSV_ABI*)(void*, void*, u32);
using AggregateOperation = void(PS4_SYSV_ABI*)(void*);
using BufferEvent = u32(PS4_SYSV_ABI*)(void*, void*, void*, void*, void*);
using GraphEvent = u32(PS4_SYSV_ABI*)(void*, void*);
using OwnerSwitch = void(PS4_SYSV_ABI*)(void*, void*);
using SelectLink = void*(PS4_SYSV_ABI*)(void*, void*);
using ConditionDispatch = void(PS4_SYSV_ABI*)(void*, void*, void*, void*);
using NodeExit = void(PS4_SYSV_ABI*)(void*, void*, void*);
using TimelineTick = void(PS4_SYSV_ABI*)(void*, float);
HeldNative native_held = nullptr;
ClearNative native_clear = nullptr;
AggregateEvent native_aggregate_event = nullptr;
AggregateOperation native_aggregate_clear = nullptr;
AggregateOperation native_aggregate_flush = nullptr;
BufferEvent native_buffer_event = nullptr;
GraphEvent native_graph_event = nullptr;
OwnerSwitch native_active_switch = nullptr;
OwnerSwitch native_buffer_switch = nullptr;
SelectLink native_select_link = nullptr;
GraphEvent native_match_event = nullptr;
ConditionDispatch native_condition_dispatch = nullptr;
NodeExit native_node_exit = nullptr;
AggregateOperation native_fire_start = nullptr;
TimelineTick native_timeline_tick = nullptr;
OwnerSwitch native_timeline_item = nullptr;
Native native_awake = nullptr;
bool discover_topology = false;
std::atomic<Native> original;
std::atomic<Native> original_jump;
std::atomic<u64> charge_property{0};
std::atomic<u64> capture_generation{0};
u64 guest_base = 0;

bool Read(u64 address, void* output, std::size_t size) {
    if (address == 0 || size > std::numeric_limits<u64>::max() - address) {
        return false;
    }
    const u64 end = address + size;
    for (u64 cursor = address; cursor < end;) {
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(reinterpret_cast<void*>(cursor), &info, sizeof(info)) == 0 ||
            info.State != MEM_COMMIT || (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0 ||
            (info.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                             PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) == 0) {
            return false;
        }
        const u64 region_end = reinterpret_cast<u64>(info.BaseAddress) + info.RegionSize;
        if (region_end <= cursor) {
            return false;
        }
        cursor = region_end;
    }
    std::memcpy(output, reinterpret_cast<const void*>(address), size);
    return true;
}

u64 Word(u64 address) {
    u64 value = 0;
    Read(address, &value, sizeof(value));
    return value;
}

struct Assignment {
    u64 owner_property = 0;
    u64 property = 0;
    u32 value = 0;
};

bool Decode(u64 code, Assignment& assignment) {
    std::array<u8, 35> bytes{};
    if (!Read(code, bytes.data(), bytes.size()) || bytes[0] != 0x19 || bytes[1] != 0x01 ||
        bytes[20] != 0 || bytes[21] != 0x01 || bytes[30] != 0x1e) {
        return false;
    }
    u64 result_property = 0;
    std::memcpy(&assignment.owner_property, bytes.data() + 2, 8);
    std::memcpy(&result_property, bytes.data() + 12, 8);
    std::memcpy(&assignment.property, bytes.data() + 22, 8);
    std::memcpy(&assignment.value, bytes.data() + 31, 4);
    return assignment.property != 0 && assignment.property == result_property &&
           (assignment.value == 0 || assignment.value == 0x3f800000);
}

bool IsCleanupCandidate(u64 code, const Assignment& first) {
    std::array<u8, 11> prefix{};
    Assignment second, third;
    return first.value == 0 && code >= prefix.size() &&
           Read(code - prefix.size(), prefix.data(), prefix.size()) && prefix[0] == 0x1c &&
           prefix[9] == 0x16 && prefix[10] == 0x0f && Decode(code + 36, second) &&
           Decode(code + 72, third) && second.value == 0 && third.value == 0 &&
           first.owner_property == second.owner_property &&
           first.owner_property == third.owner_property && first.property != second.property &&
           first.property != third.property && second.property != third.property;
}

u64 LiveWord(u64 address) {
    u64 value = 0;
    std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(value));
    return value;
}

void FrameSite(u64 request, void* frame) {
    const u64 address = reinterpret_cast<u64>(frame);
    const u64 node = LiveWord(address + 0x18);
    const u64 outer = node != 0 ? Word(node + 0x40) : 0;
    PadTrace::Record("DMC_FRAME",
                     {static_cast<s64>(request), 0, static_cast<s64>(address),
                      static_cast<s64>(node), static_cast<s64>(LiveWord(address + 0x20)),
                      static_cast<s64>(LiveWord(address + 0x28)),
                      static_cast<s64>(LiveWord(address + 0x38)),
                      static_cast<s64>(node != 0 ? Word(node + 0x48) : 0),
                      static_cast<s64>(outer != 0 ? Word(outer + 0x48) : 0)});
}

struct CachedAssignment {
    u64 code = 0;
    Assignment assignment{};
    bool matched = false;
    bool cleanup = false;
    bool reported = false;
    bool action_candidate = false;
};

struct WatchedAction {
    u64 object = 0;
    u64 start_name = 0;
    u32 probes = 0;
    u32 records = 0;
};

struct ProbeCache {
    u64 generation = 0;
    u32 discoveries = 0;
    u32 records = 0;
    bool watch_limit_reported = false;
    std::array<WatchedAction, 16> actions{};
    std::array<CachedAssignment, 4096> sites{};
};

thread_local ProbeCache probe_cache;

void ResetCaptureCache() {
    const u64 generation = capture_generation.load(std::memory_order_relaxed);
    if (probe_cache.generation != generation) {
        probe_cache = {};
        probe_cache.generation = generation;
    }
}

WatchedAction* FindAction(void* object) {
    if (object == nullptr) {
        return nullptr;
    }
    for (auto& action : probe_cache.actions) {
        if (action.object == reinterpret_cast<u64>(object)) {
            return &action;
        }
    }
    return nullptr;
}

void WatchAction(void* object, void* frame, const Assignment& assignment, bool cleanup) {
    if (object == nullptr || FindAction(object) != nullptr) {
        return;
    }
    for (auto& action : probe_cache.actions) {
        if (action.object == 0) {
            const u64 node = LiveWord(reinterpret_cast<u64>(frame) + 0x18);
            action.object = reinterpret_cast<u64>(object);
            action.start_name = !cleanup && node != 0 ? Word(node + 0x48) : 0;
            PadTrace::Record("DMC_ACTION_WATCH",
                             {reinterpret_cast<s64>(object), static_cast<s64>(node),
                              static_cast<s64>(assignment.owner_property),
                              static_cast<s64>(assignment.property), cleanup,
                              static_cast<s64>(action.start_name)});
            return;
        }
    }
    if (!probe_cache.watch_limit_reported) {
        probe_cache.watch_limit_reported = true;
        PadTrace::Record("DMC_WATCH_LIMIT", {16});
    }
}

u64 Operand(void* frame) {
    u64 code = 0;
    std::memcpy(&code, static_cast<const u8*>(frame) + 0x28, sizeof(code));
    return code;
}

u32 LiveDword(u64 address) {
    u32 value = 0;
    std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(value));
    return value;
}

struct HeldSample {
    u64 object = 0;
    u64 caller = 0;
    u32 button = 0;
    u32 mask = 0;
    u32 result = 0;
    u32 since = 0;
    u64 calls = 0;
    std::chrono::steady_clock::time_point recorded{};
};

struct PendingItem {
    u64 event = 0;
    u64 descriptor = 0;
    u64 event_class = 0;
    u32 state = 0;
    u32 flags = 0;
    bool operator==(const PendingItem&) const = default;
};

struct PendingSnapshot {
    u64 buffer = 0;
    u64 owner = 0;
    u64 items = 0;
    u32 count = 0;
    u32 policy = 0;
    std::array<PendingItem, 8> entries{};
    bool operator==(const PendingSnapshot&) const = default;
};

struct GraphSnapshot {
    u64 graph = 0;
    u64 root = 0;
    u64 node = 0;
    u64 first_set = 0;
    u64 second_set = 0;
    u32 flags = 0;
    u32 node_state = 0;
    u32 node_flags = 0;
    bool operator==(const GraphSnapshot&) const = default;
};

struct GraphSample {
    u64 graph = 0;
    u64 descriptor = 0;
    u32 type = 0;
    u32 result = 0;
    GraphSnapshot before{};
    GraphSnapshot after{};
};

struct ConditionResult {
    u64 object = 0;
    u64 function = 0;
    u64 context = 0;
    u64 pawn = 0;
    u64 caller = 0;
    u32 result = 0;
    bool operator==(const ConditionResult&) const = default;
};

struct MatchResult {
    u64 definition = 0;
    u32 result = 0;
    bool operator==(const MatchResult&) const = default;
};

struct GateInput {
    bool valid = false;
    u32 combo = 0;
    u32 flags = 0;
    u32 type = 0;
    u32 motion = 0;
    u32 mode = 0;
    u32 focus = 0;
    u32 held = 0;
    u64 upgrades = 0;
    bool operator==(const GateInput& other) const {
        return valid == other.valid && combo == other.combo && flags == other.flags &&
               type == other.type && mode == other.mode && focus == other.focus &&
               held == other.held && upgrades == other.upgrades &&
               (std::bit_cast<float>(motion) > 0) == (std::bit_cast<float>(other.motion) > 0);
    }
};

bool IsInputContext(u64 event) {
    const u64 input_class = Word(guest_base + 0x3b193f8);
    for (u64 parent = Word(event + 0x50), depth = 0; input_class != 0 && parent != 0 && depth < 16;
         ++depth) {
        if (parent == input_class) {
            return true;
        }
        const u64 next = Word(parent + 0x68);
        if (next == parent) {
            break;
        }
        parent = next;
    }
    return false;
}

struct GateResults {
    GateInput input{};
    u32 condition_count = 0;
    u32 match_count = 0;
    u32 clipped = 0;
    std::array<ConditionResult, 32> conditions{};
    std::array<MatchResult, 32> matches{};
    bool operator==(const GateResults&) const = default;
};

struct GateTrace {
    u64 descriptor = 0;
    GateResults results{};
};

struct GateSample {
    u64 set = 0;
    u64 descriptor = 0;
    u64 selected = 0;
    u32 type = 0;
    u32 epoch = 0;
    GateResults results{};
};

thread_local GateTrace* active_gate = nullptr;

struct NativeCache {
    u64 generation = 0;
    u32 held_records = 0;
    u32 clear_records = 0;
    std::array<u32, 4> aggregate_records{};
    u64 aggregate_request = 0;
    u64 aggregate_event = 0;
    u32 aggregate_type = 256;
    u32 aggregate_held = 0;
    std::array<u32, 2> handoff_records{};
    std::array<u64, 256> identities{};
    std::array<u32, 512> names{};
    u32 identity_records = 0;
    u32 owner_records = 0;
    u32 exit_records = 0;
    u32 gate_records = 0;
    u32 gun_since = 0;
    u32 gun_epoch = 0;
    u64 gun_descriptor = 0;
    u64 player_aggregator = 0;
    u64 fire_action = 0;
    u64 fire_node = 0;
    u64 fire_timeline = 0;
    u64 holster_name = 0;
    u32 fire_serial = 0;
    u32 fire_records = 0;
    u32 timeline_records = 0;
    u32 item_records = 0;
    u64 timeline_item_request = 0;
    u32 timeline_delta = 0;
    bool fire_pending = false;
    std::chrono::steady_clock::time_point timeline_recorded{};
    std::array<GateSample, 128> gates{};
    std::array<std::array<u64, 2>, 64> topologies{};
    std::array<GraphSample, 128> graphs{};
    std::array<HeldSample, 128> held{};
};

thread_local NativeCache native_cache;

void ResetNativeCache() {
    const u64 generation = capture_generation.load(std::memory_order_relaxed);
    if (native_cache.generation != generation) {
        native_cache = {};
        native_cache.generation = generation;
    }
}

void GameClock(u64 request, u64 input = 0, u32 button = 0) {
    const u64 engine = Word(guest_base + 0x3a548e8);
    const u64 contexts = engine != 0 ? Word(engine + 0x80) : 0;
    const u64 current = contexts != 0 ? Word(contexts + 0x60) : 0;
    const u64 world = current != 0 ? Word(current) : 0;
    std::array<u32, 4> times{};
    const bool readable = world != 0 && Read(world + 0x690, times.data(), sizeof(times));
    PadTrace::Record("DMC_GAME_CLOCK",
                     {static_cast<s64>(request), static_cast<s64>(world), readable, times[0],
                      times[1], times[2], times[3], static_cast<s64>(Word(guest_base + 0x3940be0)),
                      static_cast<s64>(Word(guest_base + 0x3f58870)), static_cast<s64>(input),
                      button, input != 0 ? LiveDword(input + 0x220) : 0,
                      input != 0 && button < 32 ? LiveDword(input + 0x228 + button * 4) : 0});
}

u32 PS4_SYSV_ABI ObserveHeld(void* object, u32 button) {
    const u32 result = native_held(object, button);
    if (!PadTrace::Enabled()) {
        return result;
    }
    ResetNativeCache();
    if (native_cache.held_records >= 4096) {
        return result;
    }
    const u64 address = reinterpret_cast<u64>(object);
    const u64 caller = reinterpret_cast<u64>(__builtin_return_address(0));
    const u32 mask = LiveDword(address + 0x220);
    const u32 since = button < 32 ? LiveDword(address + 0x228 + button * 4) : 0;
    if (button == 6 && result != 0 && native_cache.gun_since != since) {
        native_cache.gun_since = since;
        ++native_cache.gun_epoch;
    }
    auto& sample =
        native_cache.held[((address >> 4) ^ (caller >> 4) ^ button) % native_cache.held.size()];
    const bool first = sample.calls == 0 || sample.object != address || sample.caller != caller ||
                       sample.button != button;
    if (first) {
        sample = {};
        sample.object = address;
        sample.caller = caller;
        sample.button = button;
    }
    ++sample.calls;
    const auto now = std::chrono::steady_clock::now();
    if (first || sample.mask != mask || sample.result != result || sample.since != since ||
        now - sample.recorded >= std::chrono::milliseconds{100}) {
        ++native_cache.held_records;
        PadTrace::Record("DMC_NATIVE_HELD",
                         {static_cast<s64>(address), button, result, mask, since,
                          static_cast<s64>(caller), static_cast<s64>(sample.calls)});
        if (button == 6) {
            GameClock(0, address, button);
        }
        sample.mask = mask;
        sample.result = result;
        sample.since = since;
        sample.recorded = now;
        if (native_cache.held_records == 4096) {
            PadTrace::Record("DMC_NATIVE_LIMIT", {0, 4096});
        }
    }
    return result;
}

u32 PS4_SYSV_ABI ObserveClear(void* object, void* event) {
    if (!PadTrace::Enabled()) {
        return native_clear(object, event);
    }
    ResetNativeCache();
    if (native_cache.clear_records >= 1024) {
        return native_clear(object, event);
    }
    ++native_cache.clear_records;
    const u64 request = PadTrace::NextId();
    const u64 address = reinterpret_cast<u64>(object);
    PadTrace::Record("DMC_NATIVE_CLEAR_BEGIN",
                     {static_cast<s64>(request), static_cast<s64>(address),
                      reinterpret_cast<s64>(event), static_cast<s64>(LiveWord(address + 0x70)),
                      LiveDword(address + 0xd0), LiveDword(address + 0xb4),
                      reinterpret_cast<s64>(__builtin_return_address(0))});
    const u32 result = native_clear(object, event);
    PadTrace::Record(
        "DMC_NATIVE_CLEAR_END",
        {static_cast<s64>(request), result, LiveDword(address + 0xd0), LiveDword(address + 0xb4)});
    if (native_cache.clear_records == 1024) {
        PadTrace::Record("DMC_NATIVE_LIMIT", {1, 1024});
    }
    return result;
}

bool AggregateBudget(u32 kind) {
    ResetNativeCache();
    constexpr std::array<u32, 4> limits{512, 1024, 256, 256};
    auto& count = native_cache.aggregate_records[kind];
    if (count >= limits[kind]) {
        return false;
    }
    ++count;
    if (count == limits[kind]) {
        PadTrace::Record("DMC_NATIVE_LIMIT", {kind + 2, count});
    }
    return true;
}

void AggregateState(const char* kind, u64 request, void* object, void* event, u32 mode, u64 caller,
                    u32 result = 0) {
    const u64 address = reinterpret_cast<u64>(object);
    PadTrace::Record(
        kind, {static_cast<s64>(request), static_cast<s64>(address), reinterpret_cast<s64>(event),
               mode, LiveDword(address + 0x80), static_cast<s64>(LiveWord(address + 0xa0)),
               static_cast<s64>(LiveWord(address + 0xa8)), LiveDword(address + 0xb0),
               LiveDword(address + 0xc0), LiveDword(address + 0xd0), LiveDword(address + 0xd8),
               static_cast<s64>(caller), result, static_cast<s64>(native_cache.aggregate_request)});
}

class AggregateScope {
public:
    explicit AggregateScope(u64 request, u32 type = 256, u32 held = 0, u64 event = 0)
        : previous{native_cache.aggregate_request}, previous_type{native_cache.aggregate_type},
          previous_held{native_cache.aggregate_held}, previous_event{native_cache.aggregate_event} {
        native_cache.aggregate_request = request;
        native_cache.aggregate_type = type;
        native_cache.aggregate_held = held;
        native_cache.aggregate_event = event;
    }
    ~AggregateScope() {
        native_cache.aggregate_request = previous;
        native_cache.aggregate_type = previous_type;
        native_cache.aggregate_held = previous_held;
        native_cache.aggregate_event = previous_event;
    }

private:
    u64 previous;
    u32 previous_type;
    u32 previous_held;
    u64 previous_event;
};

bool HandoffBudget(u32 kind) {
    ResetNativeCache();
    if (native_cache.aggregate_request == 0) {
        return false;
    }
    constexpr std::array<u32, 2> limits{256, 512};
    auto& count = native_cache.handoff_records[kind];
    if (count >= limits[kind]) {
        return false;
    }
    if (++count == limits[kind]) {
        PadTrace::Record("DMC_NATIVE_LIMIT", {kind + 6, count});
    }
    return true;
}

void NameSite(u32 index) {
    if (index == 0 || native_cache.names[index % native_cache.names.size()] == index) {
        return;
    }
    native_cache.names[index % native_cache.names.size()] = index;
    const u64 table = Word(guest_base + 0x3984330);
    u32 count = 0;
    if (table == 0 || !Read(guest_base + 0x3984338, &count, sizeof(count)) || index >= count ||
        count > 2000000) {
        return;
    }
    const u64 entry = Word(table + static_cast<u64>(index) * 8);
    u32 flags = 0;
    if (!Read(entry, &flags, sizeof(flags))) {
        return;
    }
    const bool wide = (flags & 1) != 0;
    for (u32 offset = 0; offset < 192; offset += 96) {
        std::array<u64, 12> words{};
        if (!Read(entry + 0x10 + offset, words.data(), sizeof(words))) {
            break;
        }
        u32 length = 0;
        const auto* bytes = reinterpret_cast<const u8*>(words.data());
        while (length < sizeof(words) && (bytes[length] != 0 || (wide && bytes[length + 1] != 0))) {
            length += wide ? 2 : 1;
        }
        PadTrace::Record("DMC_NAME", {index, wide, offset, length, static_cast<s64>(words[0]),
                                      static_cast<s64>(words[1]), static_cast<s64>(words[2]),
                                      static_cast<s64>(words[3]), static_cast<s64>(words[4]),
                                      static_cast<s64>(words[5]), static_cast<s64>(words[6]),
                                      static_cast<s64>(words[7]), static_cast<s64>(words[8]),
                                      static_cast<s64>(words[9]), static_cast<s64>(words[10]),
                                      static_cast<s64>(words[11])});
        if (length < sizeof(words)) {
            break;
        }
    }
}

void Identity(u64 request, u64 object) {
    if (object == 0 || native_cache.identity_records >= native_cache.identities.size()) {
        return;
    }
    for (u32 i = 0; i < native_cache.identity_records; ++i) {
        if (native_cache.identities[i] == object) {
            return;
        }
    }
    native_cache.identities[native_cache.identity_records++] = object;
    std::array<u64, 11> words{};
    if (!Read(object, words.data(), sizeof(words))) {
        return;
    }
    PadTrace::Record("DMC_OBJECT", {static_cast<s64>(request), static_cast<s64>(object),
                                    static_cast<s64>(words[9]), static_cast<s64>(words[10]),
                                    static_cast<s64>(words[8]), static_cast<s64>(words[0])});
    NameSite(static_cast<u32>(words[9]));
    if (words[10] != object) {
        Identity(request, words[10]);
    }
}

PendingSnapshot SnapshotBuffer(u64 buffer) {
    PendingSnapshot snapshot{};
    snapshot.buffer = buffer;
    snapshot.count = LiveDword(buffer + 8);
    snapshot.owner = LiveWord(buffer + 0x28);
    snapshot.items = LiveWord(buffer);
    snapshot.policy = LiveDword(buffer + 0x10);
    for (u32 i = 0; snapshot.items != 0 && i < snapshot.count && i < snapshot.entries.size(); ++i) {
        auto& item = snapshot.entries[i];
        item.event = LiveWord(snapshot.items + i * 8);
        if (item.event != 0) {
            item.descriptor = LiveWord(item.event + 0x60);
            item.state = LiveDword(item.event + 0x68);
            item.flags = LiveDword(item.event + 0x6c);
            item.event_class = LiveWord(item.event + 0x50);
        }
    }
    return snapshot;
}

void BufferState(u64 request, u32 phase, const PendingSnapshot& snapshot) {
    PadTrace::Record("DMC_BUFFER",
                     {static_cast<s64>(request), phase, static_cast<s64>(snapshot.buffer),
                      snapshot.count, snapshot.policy, static_cast<s64>(snapshot.owner),
                      static_cast<s64>(snapshot.items)});
    Identity(request, snapshot.owner);
    for (u32 i = 0; i < snapshot.count && i < snapshot.entries.size(); ++i) {
        const auto& item = snapshot.entries[i];
        if (item.event == 0) {
            continue;
        }
        PadTrace::Record("DMC_BUFFER_ITEM",
                         {static_cast<s64>(request), phase, i, static_cast<s64>(item.event),
                          static_cast<s64>(item.descriptor), item.state, item.flags,
                          static_cast<s64>(item.event_class)});
        Identity(request, item.descriptor);
    }
}

GraphSnapshot SnapshotGraph(u64 graph) {
    GraphSnapshot snapshot{};
    snapshot.graph = graph;
    snapshot.root = LiveWord(graph + 0x60);
    snapshot.node = LiveWord(graph + 0x88);
    snapshot.flags = LiveDword(graph + 0x90);
    if (snapshot.node != 0) {
        snapshot.node_state = LiveDword(snapshot.node + 0xd0);
        snapshot.node_flags = LiveDword(snapshot.node + 0xb4);
        snapshot.first_set = LiveWord(snapshot.node + 0x80);
        snapshot.second_set = LiveWord(snapshot.node + 0x88);
    }
    return snapshot;
}

void GraphState(u64 request, u32 phase, const GraphSnapshot& snapshot) {
    PadTrace::Record("DMC_GRAPH",
                     {static_cast<s64>(request), phase, static_cast<s64>(snapshot.graph),
                      static_cast<s64>(snapshot.root), static_cast<s64>(snapshot.node),
                      snapshot.flags, snapshot.node_state, snapshot.node_flags,
                      static_cast<s64>(snapshot.first_set), static_cast<s64>(snapshot.second_set)});
    Identity(request, snapshot.graph);
    Identity(request, snapshot.node);
}

u32 FloatWord(float value) {
    u32 word = 0;
    std::memcpy(&word, &value, sizeof(word));
    return word;
}

bool NameIs(u32 index, const char* expected) {
    const u64 table = Word(guest_base + 0x3984330);
    u32 count = 0;
    if (index == 0 || table == 0 || !Read(guest_base + 0x3984338, &count, sizeof(count)) ||
        index >= count || count > 2000000) {
        return false;
    }
    const u64 entry = Word(table + static_cast<u64>(index) * 8);
    u32 flags = 0;
    if (entry == 0 || !Read(entry, &flags, sizeof(flags))) {
        return false;
    }
    const auto length = std::strlen(expected);
    const bool wide = (flags & 1) != 0;
    std::array<u8, 64> bytes{};
    const auto size = (length + 1) * (wide ? 2 : 1);
    if (size > bytes.size() || !Read(entry + 0x10, bytes.data(), size)) {
        return false;
    }
    for (std::size_t i = 0; i <= length; ++i) {
        if (bytes[i * (wide ? 2 : 1)] != static_cast<u8>(expected[i]) ||
            (wide && bytes[i * 2 + 1] != 0)) {
            return false;
        }
    }
    return true;
}

void FireState(const char* kind, u64 request, u64 action, u64 node, u64 timeline,
               u64 script_name = 0) {
    const u64 graph = node != 0 ? Word(node + 0x70) : 0;
    const u64 aggregator = graph != 0 ? Word(graph + 0x78) : 0;
    PadTrace::Record(
        kind,
        {static_cast<s64>(request), native_cache.fire_serial, static_cast<s64>(action),
         static_cast<s64>(node), static_cast<s64>(timeline),
         timeline != 0 ? LiveDword(timeline + 0x8c) : 0, node != 0 ? LiveDword(node + 0xd0) : 0,
         node != 0 ? LiveDword(node + 0xb4) : 0, aggregator != 0 ? LiveDword(aggregator + 0x80) : 0,
         aggregator != 0 ? static_cast<s64>(LiveWord(aggregator + 0xa0)) : 0,
         static_cast<s64>(script_name), action != 0 ? LiveDword(action + 0x13c) : 0,
         action != 0 ? LiveDword(action + 0x140) : 0});
}

void PS4_SYSV_ABI ObserveFireStart(void* action) {
    if (!PadTrace::Enabled()) {
        return native_fire_start(action);
    }
    ResetNativeCache();
    if (native_cache.fire_records >= 128 || native_cache.player_aggregator == 0) {
        return native_fire_start(action);
    }
    const u64 address = reinterpret_cast<u64>(action);
    const u64 node = Word(address + 0x68);
    const u64 graph = node != 0 ? Word(node + 0x70) : 0;
    if (graph == 0 || Word(graph + 0x78) != native_cache.player_aggregator) {
        return native_fire_start(action);
    }
    struct Scheduled {
        u64 callback = 0;
        u64 name = 0;
        u32 seconds = 0;
        u32 flags = 0;
    };
    std::array<Scheduled, 64> items{};
    std::array<std::array<u64, 3>, 64> payloads{};
    std::array<std::array<u32, 3>, 64> controls{};
    const u64 array = Word(address + 0x80);
    const u32 count = LiveDword(address + 0x88);
    u64 holster = 0;
    for (u32 i = 0; array != 0 && i < count && i < items.size(); ++i) {
        const u64 entry = array + i * 0x48;
        items[i] = {Word(entry), Word(entry + 8), LiveDword(entry + 0x30),
                    LiveDword(entry + 0x40) & 0xff};
        payloads[i] = {Word(entry + 0x10), Word(entry + 0x18), Word(entry + 0x28)};
        controls[i] = {LiveDword(entry + 0x3c) & 0xff, LiveDword(entry + 0x34),
                       LiveDword(entry + 0x38)};
        if (native_cache.holster_name != 0
                ? items[i].name == native_cache.holster_name
                : NameIs(static_cast<u32>(items[i].name), "HolsterWeapon")) {
            holster = items[i].name;
        }
    }
    if (holster == 0) {
        return native_fire_start(action);
    }
    const u64 timeline = Word(address + 0x60);
    const u32 time_before = timeline != 0 ? LiveDword(timeline + 0x8c) : 0;
    native_cache.fire_action = address;
    native_cache.fire_node = node;
    native_cache.fire_timeline = timeline;
    native_cache.holster_name = holster;
    native_cache.fire_pending = true;
    native_cache.timeline_records = 0;
    native_cache.item_records = 0;
    native_cache.timeline_recorded = {};
    ++native_cache.fire_serial;
    native_fire_start(action);
    const u64 request = PadTrace::NextId();
    ++native_cache.fire_records;
    for (u32 i = 0; i < count && i < items.size(); ++i) {
        const auto& item = items[i];
        PadTrace::Record("DMC_FIRE_SCHEDULE", {static_cast<s64>(request),
                                               native_cache.fire_serial,
                                               static_cast<s64>(address),
                                               static_cast<s64>(node),
                                               static_cast<s64>(timeline),
                                               static_cast<s64>(item.callback),
                                               static_cast<s64>(item.name),
                                               item.seconds,
                                               item.name == holster,
                                               i,
                                               count,
                                               static_cast<s64>(Word(address + 0x78)),
                                               static_cast<s64>(native_cache.aggregate_request),
                                               time_before,
                                               timeline != 0 ? LiveDword(timeline + 0x8c) : 0,
                                               item.flags,
                                               count > items.size(),
                                               controls[i][0],
                                               static_cast<s64>(payloads[i][0]),
                                               static_cast<s64>(payloads[i][1]),
                                               static_cast<s64>(payloads[i][2]),
                                               controls[i][1],
                                               controls[i][2]});
        NameSite(static_cast<u32>(item.name));
        if (controls[i][0] == 2) {
            Identity(request, payloads[i][0]);
        }
    }
    Identity(request, address);
    Identity(request, node);
    FireState("DMC_FIRE_START", request, address, node, timeline);
    GameClock(request);
    if (native_cache.fire_records == 128) {
        PadTrace::Record("DMC_NATIVE_LIMIT", {14, 128});
    }
}

void PS4_SYSV_ABI ObserveTimelineTick(void* object, float delta) {
    const u64 address = reinterpret_cast<u64>(object);
    if (!PadTrace::Enabled()) {
        return native_timeline_tick(object, delta);
    }
    ResetNativeCache();
    if (address != native_cache.fire_timeline || !native_cache.fire_pending ||
        native_cache.timeline_records >= 128) {
        return native_timeline_tick(object, delta);
    }
    const u32 before = LiveDword(address + 0x8c);
    const u32 serial = native_cache.fire_serial;
    const u32 previous_delta = native_cache.timeline_delta;
    native_cache.timeline_delta = FloatWord(delta);
    native_timeline_tick(object, delta);
    native_cache.timeline_delta = previous_delta;
    const auto now = std::chrono::steady_clock::now();
    if (!native_cache.fire_pending ||
        now - native_cache.timeline_recorded >= std::chrono::milliseconds{50}) {
        ++native_cache.timeline_records;
        native_cache.timeline_recorded = now;
        PadTrace::Record("DMC_FIRE_TICK", {serial, static_cast<s64>(address), FloatWord(delta),
                                           before, LiveDword(address + 0x8c),
                                           native_cache.fire_pending, LiveDword(address + 0x98)});
        if (native_cache.timeline_records == 128) {
            PadTrace::Record("DMC_NATIVE_LIMIT", {15, 128, serial});
        }
    }
}

void PS4_SYSV_ABI ObserveTimelineItem(void* timeline, void* item) {
    if (!PadTrace::Enabled()) {
        return native_timeline_item(timeline, item);
    }
    ResetNativeCache();
    const u64 address = reinterpret_cast<u64>(timeline);
    if (address != native_cache.fire_timeline || !native_cache.fire_pending ||
        native_cache.item_records >= 64) {
        return native_timeline_item(timeline, item);
    }
    const u64 entry = reinterpret_cast<u64>(item);
    const u64 request = PadTrace::NextId();
    const u64 previous = native_cache.timeline_item_request;
    native_cache.timeline_item_request = request;
    ++native_cache.item_records;
    PadTrace::Record(
        "DMC_FIRE_ITEM_BEGIN",
        {static_cast<s64>(request), native_cache.fire_serial, static_cast<s64>(address),
         static_cast<s64>(entry), LiveDword(address + 0x8c), static_cast<s64>(Word(entry)),
         static_cast<s64>(Word(entry + 8)), static_cast<s64>(Word(entry + 0x10)),
         static_cast<s64>(Word(entry + 0x18)), static_cast<s64>(Word(entry + 0x20)),
         static_cast<s64>(Word(entry + 0x28)), LiveDword(entry + 0x30), LiveDword(entry + 0x34),
         LiveDword(entry + 0x38), LiveDword(entry + 0x3c) & 0xff, LiveDword(entry + 0x40) & 0xff,
         static_cast<s64>(native_cache.aggregate_request), native_cache.timeline_delta});
    native_timeline_item(timeline, item);
    FireState("DMC_FIRE_ITEM_END", request, native_cache.fire_action, native_cache.fire_node,
              address);
    native_cache.timeline_item_request = previous;
    if (native_cache.item_records == 64) {
        PadTrace::Record("DMC_NATIVE_LIMIT", {16, 64, native_cache.fire_serial});
    }
}

void PS4_SYSV_ABI ObserveAwake(void* node, void* frame, void* result) {
    const u64 address = reinterpret_cast<u64>(node);
    if (!PadTrace::Enabled()) {
        return native_awake(node, frame, result);
    }
    ResetNativeCache();
    if (address != native_cache.fire_node || native_cache.fire_records >= 128) {
        return native_awake(node, frame, result);
    }
    const u64 function = LiveWord(reinterpret_cast<u64>(frame) + 0x18);
    const u64 name = function != 0 ? Word(function + 0x48) : 0;
    const u64 request = PadTrace::NextId();
    const u64 action = native_cache.fire_action;
    const u64 timeline = native_cache.fire_timeline;
    ++native_cache.fire_records;
    const u64 frame_address = reinterpret_cast<u64>(frame);
    const u64 context = LiveWord(frame_address + 0x20);
    const u64 outer = function != 0 ? Word(function + 0x40) : 0;
    PadTrace::Record("DMC_FIRE_AWAKE_ORIGIN",
                     {static_cast<s64>(request), native_cache.fire_serial,
                      static_cast<s64>(function), static_cast<s64>(outer),
                      static_cast<s64>(context), static_cast<s64>(LiveWord(frame_address + 0x28)),
                      static_cast<s64>(native_cache.timeline_item_request),
                      static_cast<s64>(native_cache.aggregate_request),
                      native_cache.timeline_delta});
    FireState("DMC_FIRE_AWAKE_BEGIN", request, action, address, timeline, name);
    native_awake(node, frame, result);
    FireState("DMC_FIRE_AWAKE_END", request, action, address, timeline, name);
    NameSite(static_cast<u32>(name));
    Identity(request, outer);
    Identity(request, context);
    native_cache.fire_pending = false;
    if (native_cache.fire_records == 128) {
        PadTrace::Record("DMC_NATIVE_LIMIT", {14, 128});
    }
}

void GateConditionDetails(u64 request, u64 condition) {
    Identity(request, condition);
    const u64 table = condition != 0 ? Word(condition) : 0;
    if (table != 0) {
        PadTrace::Record("DMC_GATE_DEFINITION",
                         {static_cast<s64>(request), static_cast<s64>(condition),
                          static_cast<s64>(Word(table + 0x248)),
                          static_cast<s64>(Word(table + 0x250))});
    }
    if (table == 0 || Word(table + 0x250) != guest_base + 0xb72300) {
        return;
    }
    const u64 token = Word(condition + 0x60);
    u32 expected = 0;
    u32 count = 0;
    Read(condition + 0x68, &expected, sizeof(expected));
    Read(condition + 0x78, &count, sizeof(count));
    PadTrace::Record("DMC_GATE_TOKEN", {static_cast<s64>(request), static_cast<s64>(condition), 0,
                                        static_cast<s64>(token), expected & 1, count});
    NameSite(static_cast<u32>(token));
    const u64 extra = Word(condition + 0x70);
    for (u32 i = 0; extra != 0 && i < count && i < 8; ++i) {
        std::array<u32, 3> item{};
        if (!Read(extra + i * 12, item.data(), sizeof(item))) {
            break;
        }
        const u64 name = item[0] | (static_cast<u64>(item[1]) << 32);
        PadTrace::Record("DMC_GATE_TOKEN", {static_cast<s64>(request), static_cast<s64>(condition),
                                            i + 1, static_cast<s64>(name), item[2] & 1, count});
        NameSite(item[0]);
    }
}

void GateEventDetails(u64 request, u64 definition) {
    const u64 array = Word(definition + 0x70);
    const u32 count = LiveDword(definition + 0x78);
    for (u32 i = 0; array != 0 && i < count && i < 8; ++i) {
        const u64 condition = Word(array + i * 8);
        const u64 table = condition != 0 ? Word(condition) : 0;
        const u64 handler = table != 0 ? Word(table + 0x248) : 0;
        const u32 kind = handler == guest_base + 0xa232a0   ? 1
                         : handler == guest_base + 0xb73310 ? 2
                         : handler == guest_base + 0xa22f70 ? 3
                                                            : 0;
        PadTrace::Record("DMC_GATE_EVENT_CONFIG",
                         {static_cast<s64>(request), static_cast<s64>(definition), i,
                          static_cast<s64>(condition), static_cast<s64>(handler), kind,
                          kind != 0 ? static_cast<s64>(Word(condition + 0x60)) : 0,
                          kind == 2 ? LiveDword(condition + 0x68) : 0,
                          kind == 2 ? static_cast<s64>(Word(condition + 0x70)) : 0,
                          kind == 2 ? LiveDword(condition + 0x78) : 0, count, count > 8});
        Identity(request, condition);
    }
}

void GateRelations(u64 request, u64 set, u64 link, u32 kind, u64 container) {
    if (container == 0) {
        return;
    }
    const u64 array = Word(container + (kind == 1 ? 0x70 : 0x68));
    u32 count = 0;
    Read(container + (kind == 1 ? 0x78 : 0x70), &count, sizeof(count));
    PadTrace::Record("DMC_GATE_RELATION_SET",
                     {static_cast<s64>(request), static_cast<s64>(set), static_cast<s64>(link),
                      kind, static_cast<s64>(container), count, count > 32});
    for (u32 i = 0; array != 0 && i < count && i < 32; ++i) {
        const u64 condition = Word(array + i * 8);
        PadTrace::Record("DMC_GATE_RELATION",
                         {static_cast<s64>(request), static_cast<s64>(set), static_cast<s64>(link),
                          kind, i, static_cast<s64>(condition), static_cast<s64>(container)});
        GateConditionDetails(request, condition);
    }
}

void GateTopology(u64 request, u64 set, u64 descriptor) {
    for (auto& entry : native_cache.topologies) {
        if (entry[0] == set && entry[1] == descriptor) {
            return;
        }
        if (entry[0] == 0) {
            entry = {set, descriptor};
            GateRelations(request, set, 0, 0, Word(set + 0x70));
            const u64 array = Word(set + 0x60);
            u32 count = 0;
            Read(set + 0x68, &count, sizeof(count));
            PadTrace::Record("DMC_GATE_TOPOLOGY",
                             {static_cast<s64>(request), static_cast<s64>(set),
                              static_cast<s64>(descriptor), count, count > 64});
            for (u32 i = 0; array != 0 && i < count && i < 64; ++i) {
                const u64 link = Word(array + i * 8);
                const u64 definition = link != 0 ? Word(link + 0x60) : 0;
                if (definition == 0 || Word(definition + 0x68) != descriptor) {
                    continue;
                }
                const u64 target = Word(link + 0x70);
                const u64 conditions = Word(link + 0x78);
                PadTrace::Record("DMC_GATE_LINK",
                                 {static_cast<s64>(request), static_cast<s64>(set),
                                  static_cast<s64>(link), static_cast<s64>(definition),
                                  static_cast<s64>(target), static_cast<s64>(Word(link + 0x68)),
                                  static_cast<s64>(conditions)});
                Identity(request, link);
                Identity(request, definition);
                Identity(request, target);
                GateRelations(request, set, link, 1, definition);
                GateRelations(request, set, link, 2, conditions);
            }
            return;
        }
    }
    PadTrace::Record("DMC_GATE_TOPOLOGY_LIMIT", {64, static_cast<s64>(set)});
}

void PS4_SYSV_ABI ObserveConditionDispatch(void* object, void* function, void* parameters,
                                           void* result) {
    const u64 caller = reinterpret_cast<u64>(__builtin_return_address(0));
    if (caller == guest_base + 0xa2a85c && parameters != nullptr && function != nullptr &&
        PadTrace::Enabled()) {
        ResetNativeCache();
        if (native_cache.fire_pending && native_cache.fire_records < 128 &&
            LiveWord(reinterpret_cast<u64>(parameters)) == Word(native_cache.fire_action + 0x70)) {
            const u64 request = PadTrace::NextId();
            const u64 address = reinterpret_cast<u64>(object);
            const u64 script = reinterpret_cast<u64>(function);
            ++native_cache.fire_records;
            PadTrace::Record(
                "DMC_FIRE_COMMAND",
                {static_cast<s64>(request), native_cache.fire_serial, static_cast<s64>(address),
                 static_cast<s64>(script), static_cast<s64>(Word(script + 0x40)),
                 static_cast<s64>(caller), static_cast<s64>(native_cache.timeline_item_request),
                 static_cast<s64>(native_cache.aggregate_request),
                 LiveDword(native_cache.fire_timeline + 0x8c), native_cache.timeline_delta});
            native_condition_dispatch(object, function, parameters, result);
            Identity(request, address);
            Identity(request, Word(script + 0x40));
            return;
        }
    }
    if (reinterpret_cast<u64>(object) == native_cache.fire_action && function != nullptr &&
        native_cache.fire_records < 128 && PadTrace::Enabled()) {
        ResetNativeCache();
        if (reinterpret_cast<u64>(object) != native_cache.fire_action) {
            return native_condition_dispatch(object, function, parameters, result);
        }
        const u64 name = LiveWord(reinterpret_cast<u64>(function) + 0x48);
        const bool holster = name == native_cache.holster_name;
        const bool lifecycle = caller >= guest_base + 0xa18780 && caller < guest_base + 0xa18a00;
        if (holster || lifecycle) {
            const u64 request = PadTrace::NextId();
            const u64 action = native_cache.fire_action;
            const u64 node = native_cache.fire_node;
            const u64 timeline = native_cache.fire_timeline;
            ++native_cache.fire_records;
            FireState("DMC_FIRE_CALLBACK_BEGIN", request, action, node, timeline, name);
            native_condition_dispatch(object, function, parameters, result);
            FireState("DMC_FIRE_CALLBACK_END", request, action, node, timeline, name);
            NameSite(static_cast<u32>(name));
            if (holster ||
                (NameIs(static_cast<u32>(name), "CommonCleanup") &&
                 (LiveDword(action + 0x13c) & 1) == 0) ||
                NameIs(static_cast<u32>(name), "OnInterrupt") ||
                NameIs(static_cast<u32>(name), "OnCleanup")) {
                native_cache.fire_pending = false;
            }
            if (native_cache.fire_records == 128) {
                PadTrace::Record("DMC_NATIVE_LIMIT", {14, 128});
            }
            return;
        }
    }
    auto* trace = active_gate;
    if (trace == nullptr || parameters == nullptr ||
        (caller != guest_base + 0xa2090c && caller != guest_base + 0xa235b9)) {
        return native_condition_dispatch(object, function, parameters, result);
    }
    const u64 address = reinterpret_cast<u64>(parameters);
    ConditionResult observed{};
    observed.object = reinterpret_cast<u64>(object);
    observed.function = reinterpret_cast<u64>(function);
    observed.context = LiveWord(address + 8);
    observed.pawn = LiveWord(address + 16);
    observed.caller = caller;
    native_condition_dispatch(object, function, parameters, result);
    observed.result = LiveDword(address + 24);
    auto& results = trace->results;
    if (results.condition_count < results.conditions.size()) {
        results.conditions[results.condition_count++] = observed;
    } else {
        results.clipped |= 1;
    }
}

u32 PS4_SYSV_ABI ObserveGateMatch(void* definition, void* event) {
    auto* trace = active_gate;
    if (trace == nullptr || definition == nullptr ||
        LiveWord(reinterpret_cast<u64>(definition) + 0x68) != trace->descriptor) {
        return native_match_event(definition, event);
    }
    const u32 result = native_match_event(definition, event);
    auto& results = trace->results;
    if (results.match_count < results.matches.size()) {
        results.matches[results.match_count++] = {reinterpret_cast<u64>(definition), result};
    } else {
        results.clipped |= 2;
    }
    return result;
}

void* PS4_SYSV_ABI ObserveSelectLink(void* set, void* event) {
    if (!PadTrace::Enabled() || event == nullptr) {
        return native_select_link(set, event);
    }
    ResetNativeCache();
    if (native_cache.gate_records >= 256 || native_cache.gun_descriptor == 0) {
        return native_select_link(set, event);
    }
    const u64 event_address = reinterpret_cast<u64>(event);
    const u64 descriptor = LiveWord(event_address + 0x60);
    if (descriptor != native_cache.gun_descriptor &&
        !(native_cache.aggregate_event == event_address && native_cache.aggregate_type == 0 &&
          (native_cache.aggregate_held & 8) != 0)) {
        return native_select_link(set, event);
    }
    const u32 type = *reinterpret_cast<const u8*>(event_address + 0x78);
    GateTrace trace{descriptor};
    auto& input = trace.results.input;
    input.valid = IsInputContext(event_address);
    if (input.valid) {
        input.combo = LiveDword(event_address + 0x68);
        input.flags = LiveDword(event_address + 0x6c);
        input.type = type;
        input.motion = LiveDword(event_address + 0x7c);
        input.mode = *reinterpret_cast<const u8*>(event_address + 0x90);
        input.focus = *reinterpret_cast<const u8*>(event_address + 0x91);
        input.held = LiveDword(event_address + 0xa0);
        input.upgrades = LiveWord(event_address + 0x70);
    }
    auto* previous = active_gate;
    active_gate = &trace;
    void* selected = native_select_link(set, event);
    active_gate = previous;
    if (selected == nullptr && trace.results.condition_count == 0 &&
        trace.results.match_count == 0) {
        return selected;
    }
    const u64 address = reinterpret_cast<u64>(set);
    const u64 selected_address = reinterpret_cast<u64>(selected);
    GateSample* sample = nullptr;
    const auto start = ((address >> 4) ^ (descriptor >> 4) ^ type) % native_cache.gates.size();
    for (std::size_t i = 0; i < native_cache.gates.size(); ++i) {
        auto& candidate = native_cache.gates[(start + i) % native_cache.gates.size()];
        if (candidate.set == 0 || (candidate.set == address && candidate.descriptor == descriptor &&
                                   candidate.type == type)) {
            sample = &candidate;
            break;
        }
    }
    if (sample == nullptr) {
        native_cache.gate_records = 256;
        PadTrace::Record("DMC_NATIVE_LIMIT", {13, 128});
        return selected;
    }
    const bool repeated = sample->set == address && sample->descriptor == descriptor &&
                          sample->type == type && sample->epoch == native_cache.gun_epoch &&
                          sample->selected == selected_address && sample->results == trace.results;
    *sample = {address, descriptor, selected_address, type, native_cache.gun_epoch, trace.results};
    if (repeated) {
        return selected;
    }
    ++native_cache.gate_records;
    const u64 request = PadTrace::NextId();
    PadTrace::Record("DMC_GATE_SELECT",
                     {static_cast<s64>(request), static_cast<s64>(address),
                      static_cast<s64>(descriptor), type, static_cast<s64>(selected_address),
                      trace.results.condition_count, trace.results.match_count,
                      trace.results.clipped, native_cache.gun_epoch,
                      static_cast<s64>(native_cache.aggregate_request),
                      reinterpret_cast<s64>(__builtin_return_address(0))});
    PadTrace::Record("DMC_GATE_INPUT", {static_cast<s64>(request), input.valid, input.combo,
                                        input.flags, input.type, input.motion, input.mode,
                                        input.focus, input.held, static_cast<s64>(input.upgrades)});
    if (discover_topology) {
        GateTopology(request, address, descriptor);
    }
    Identity(request, selected_address);
    for (u32 i = 0; i < trace.results.condition_count; ++i) {
        const auto& condition = trace.results.conditions[i];
        PadTrace::Record("DMC_GATE_CONDITION",
                         {static_cast<s64>(request), i, static_cast<s64>(condition.object),
                          static_cast<s64>(condition.function), static_cast<s64>(condition.context),
                          static_cast<s64>(condition.pawn), condition.result,
                          static_cast<s64>(condition.caller),
                          condition.function != 0 ? LiveDword(condition.function + 0xd0) : 0});
        if (!discover_topology) {
            GateConditionDetails(request, condition.object);
        } else {
            Identity(request, condition.object);
        }
        Identity(request, condition.function);
        Identity(request, condition.context);
    }
    for (u32 i = 0; i < trace.results.match_count; ++i) {
        const auto& match = trace.results.matches[i];
        PadTrace::Record("DMC_GATE_MATCH", {static_cast<s64>(request), i,
                                            static_cast<s64>(match.definition), match.result});
        GateEventDetails(request, match.definition);
    }
    if (native_cache.gate_records == 256) {
        PadTrace::Record("DMC_NATIVE_LIMIT", {11, 256});
    }
    return selected;
}

void PS4_SYSV_ABI ObserveNodeExit(void* object, void* graph, void* node) {
    if (!PadTrace::Enabled()) {
        return native_node_exit(object, graph, node);
    }
    ResetNativeCache();
    const u64 address = reinterpret_cast<u64>(object);
    if (native_cache.exit_records >= 64 || LiveDword(address + 0x80) == 0) {
        return native_node_exit(object, graph, node);
    }
    const auto before = SnapshotBuffer(address + 0x78);
    const u64 node_address = reinterpret_cast<u64>(node);
    const u32 state = node != nullptr ? LiveDword(node_address + 0xd0) : 0;
    const u32 flags = node != nullptr ? LiveDword(node_address + 0xb4) : 0;
    const u32 internal = LiveDword(address + 0xd8);
    native_node_exit(object, graph, node);
    const auto after = SnapshotBuffer(address + 0x78);
    if (before == after) {
        return;
    }
    ++native_cache.exit_records;
    const u64 request = PadTrace::NextId();
    PadTrace::Record("DMC_NODE_EXIT", {static_cast<s64>(request), static_cast<s64>(address),
                                       reinterpret_cast<s64>(graph), static_cast<s64>(node_address),
                                       reinterpret_cast<s64>(__builtin_return_address(0)),
                                       static_cast<s64>(native_cache.aggregate_request),
                                       before.owner == node_address, state, flags, internal});
    BufferState(request, 0, before);
    BufferState(request, 1, after);
    Identity(request, reinterpret_cast<u64>(graph));
    Identity(request, node_address);
    if (native_cache.exit_records == 64) {
        PadTrace::Record("DMC_NATIVE_LIMIT", {12, 64});
    }
}

u32 PS4_SYSV_ABI ObserveBufferEvent(void* buffer, void* owner, void* event, void* first,
                                    void* second) {
    if (!PadTrace::Enabled() || native_cache.aggregate_request == 0 ||
        native_cache.handoff_records[0] >= 256) {
        return native_buffer_event(buffer, owner, event, first, second);
    }
    const auto before = SnapshotBuffer(reinterpret_cast<u64>(buffer));
    const u32 result = native_buffer_event(buffer, owner, event, first, second);
    const auto after = SnapshotBuffer(reinterpret_cast<u64>(buffer));
    if (before == after || !HandoffBudget(0)) {
        return result;
    }
    const u64 request = PadTrace::NextId();
    PadTrace::Record("DMC_BUFFER_CALL",
                     {static_cast<s64>(request), static_cast<s64>(native_cache.aggregate_request),
                      reinterpret_cast<s64>(buffer), reinterpret_cast<s64>(owner),
                      reinterpret_cast<s64>(event), reinterpret_cast<s64>(first),
                      reinterpret_cast<s64>(second),
                      reinterpret_cast<s64>(__builtin_return_address(0))});
    BufferState(request, 0, before);
    Identity(request, reinterpret_cast<u64>(owner));
    BufferState(request, 1, after);
    PadTrace::Record("DMC_HANDOFF_END", {static_cast<s64>(request), 0, result});
    return result;
}

u32 PS4_SYSV_ABI ObserveGraphEvent(void* graph, void* event) {
    if (!PadTrace::Enabled() || native_cache.aggregate_request == 0 ||
        native_cache.handoff_records[1] >= 512) {
        return native_graph_event(graph, event);
    }
    const auto before = SnapshotGraph(reinterpret_cast<u64>(graph));
    const u64 descriptor = event != nullptr ? LiveWord(reinterpret_cast<u64>(event) + 0x60) : 0;
    const u32 type = native_cache.aggregate_type;
    const u32 result = native_graph_event(graph, event);
    const auto after = SnapshotGraph(reinterpret_cast<u64>(graph));
    GraphSample* sample = nullptr;
    for (auto& entry : native_cache.graphs) {
        if (entry.graph == before.graph && entry.descriptor == descriptor && entry.type == type) {
            sample = &entry;
            break;
        }
        if (sample == nullptr && entry.graph == 0) {
            sample = &entry;
        }
    }
    const bool repeated =
        sample == nullptr || (sample->graph == before.graph && sample->result == result &&
                              sample->before == before && sample->after == after);
    if (sample != nullptr) {
        *sample = {before.graph, descriptor, type, result, before, after};
    }
    if ((repeated && before == after && result == 0) || !HandoffBudget(1)) {
        return result;
    }
    const u64 request = PadTrace::NextId();
    PadTrace::Record("DMC_GRAPH_CALL",
                     {static_cast<s64>(request), static_cast<s64>(native_cache.aggregate_request),
                      reinterpret_cast<s64>(graph), reinterpret_cast<s64>(event),
                      reinterpret_cast<s64>(__builtin_return_address(0))});
    GraphState(request, 0, before);
    GraphState(request, 1, after);
    PadTrace::Record("DMC_HANDOFF_END", {static_cast<s64>(request), 1, result});
    return result;
}

void OwnerStack(u64 request, u64 frame) {
    std::array<u64, 6> callers{};
    u32 count = 0;
    for (; count < callers.size() && frame != 0; ++count) {
        std::array<u64, 2> words{};
        if (!Read(frame, words.data(), sizeof(words)) || words[1] < guest_base ||
            words[1] >= guest_base + 0x2f7b360) {
            break;
        }
        callers[count] = words[1];
        if (words[0] <= frame) {
            ++count;
            break;
        }
        frame = words[0];
    }
    PadTrace::Record("DMC_OWNER_STACK",
                     {static_cast<s64>(request), count, static_cast<s64>(callers[0]),
                      static_cast<s64>(callers[1]), static_cast<s64>(callers[2]),
                      static_cast<s64>(callers[3]), static_cast<s64>(callers[4]),
                      static_cast<s64>(callers[5])});
}

void ObserveOwnerSwitch(void* object, void* incoming, u32 kind, u64 caller, u64 frame) {
    const auto handler = kind == 0 ? native_active_switch : native_buffer_switch;
    if (!PadTrace::Enabled()) {
        return handler(object, incoming);
    }
    ResetNativeCache();
    const u64 address = reinterpret_cast<u64>(object);
    if (native_cache.owner_records >= 64 || LiveDword(address + 0x80) == 0) {
        return handler(object, incoming);
    }
    const auto before = SnapshotBuffer(address + 0x78);
    const u64 active = LiveWord(address + 0x70);
    const u32 internal = LiveDword(address + 0xd8);
    handler(object, incoming);
    const auto after = SnapshotBuffer(address + 0x78);
    if (before == after) {
        return;
    }
    ++native_cache.owner_records;
    const u64 request = PadTrace::NextId();
    PadTrace::Record("DMC_OWNER_SWITCH",
                     {static_cast<s64>(request), kind, static_cast<s64>(address),
                      reinterpret_cast<s64>(incoming), static_cast<s64>(caller),
                      static_cast<s64>(native_cache.aggregate_request), static_cast<s64>(active),
                      static_cast<s64>(LiveWord(address + 0x70)), internal,
                      LiveDword(address + 0xd8), LiveDword(address + 0xb0)});
    BufferState(request, 0, before);
    BufferState(request, 1, after);
    Identity(request, reinterpret_cast<u64>(incoming));
    OwnerStack(request, frame);
    GameClock(request);
    if (native_cache.owner_records == 64) {
        PadTrace::Record("DMC_NATIVE_LIMIT", {10, 64});
    }
}

void PS4_SYSV_ABI ObserveActiveSwitch(void* object, void* incoming) {
    const u64 frame = LiveWord(reinterpret_cast<u64>(__builtin_frame_address(0)));
    ObserveOwnerSwitch(object, incoming, 0, reinterpret_cast<u64>(__builtin_return_address(0)),
                       frame);
}

void PS4_SYSV_ABI ObserveBufferSwitch(void* object, void* incoming) {
    const u64 frame = LiveWord(reinterpret_cast<u64>(__builtin_frame_address(0)));
    ObserveOwnerSwitch(object, incoming, 1, reinterpret_cast<u64>(__builtin_return_address(0)),
                       frame);
}

u32 AggregateContext(u64 request, void* event) {
    if (event == nullptr || guest_base == 0) {
        return 256;
    }
    const u64 address = reinterpret_cast<u64>(event);
    const u64 input_class = LiveWord(guest_base + 0x3b193f8);
    const u64 event_class = LiveWord(address + 0x50);
    bool input = false;
    for (u64 parent = event_class, depth = 0; input_class != 0 && parent != 0 && depth < 16;
         ++depth) {
        if (parent == input_class) {
            input = true;
            break;
        }
        const u64 next = LiveWord(parent + 0x68);
        if (next == parent) {
            break;
        }
        parent = next;
    }
    const u32 type = input ? *reinterpret_cast<const u8*>(address + 0x78) : 0;
    const u32 held = input ? LiveDword(address + 0xa0) : 0;
    if (input && type == 2 && held == 8 && native_cache.gun_descriptor == 0) {
        native_cache.gun_descriptor = LiveWord(address + 0x60);
    }
    PadTrace::Record("DMC_AGG_CONTEXT",
                     {static_cast<s64>(request), static_cast<s64>(address),
                      static_cast<s64>(event_class), input, type, held,
                      static_cast<s64>(LiveWord(address + 0x60)),
                      static_cast<s64>(LiveWord(address + 0x68)),
                      static_cast<s64>(LiveWord(address + 0x70)), static_cast<s64>(input_class)});
    return input ? type : 256;
}

u32 PS4_SYSV_ABI ObserveAggregateEvent(void* object, void* event, u32 input) {
    if (!PadTrace::Enabled() || !AggregateBudget(input != 0 ? 0 : 1)) {
        return native_aggregate_event(object, event, input);
    }
    const u64 request = PadTrace::NextId();
    const u64 caller = reinterpret_cast<u64>(__builtin_return_address(0));
    AggregateState("DMC_AGG_EVENT_BEGIN", request, object, event, input, caller);
    const u32 type = AggregateContext(request, event);
    const u32 held = type != 256 ? LiveDword(reinterpret_cast<u64>(event) + 0xa0) : 0;
    if (type == 0 && held == 8) {
        native_cache.player_aggregator = reinterpret_cast<u64>(object);
    }
    const auto before = SnapshotBuffer(reinterpret_cast<u64>(object) + 0x78);
    u32 result = 0;
    {
        const AggregateScope scope{request, type, held, reinterpret_cast<u64>(event)};
        result = native_aggregate_event(object, event, input);
    }
    AggregateState("DMC_AGG_EVENT_END", request, object, event, input, caller, result);
    const auto after = SnapshotBuffer(reinterpret_cast<u64>(object) + 0x78);
    if (!(before == after)) {
        BufferState(request, 0, before);
        BufferState(request, 1, after);
    }
    return result;
}

void PS4_SYSV_ABI ObserveAggregateClear(void* object) {
    if (!PadTrace::Enabled() || !AggregateBudget(2)) {
        return native_aggregate_clear(object);
    }
    const u64 request = PadTrace::NextId();
    const u64 caller = reinterpret_cast<u64>(__builtin_return_address(0));
    AggregateState("DMC_AGG_CLEAR_BEGIN", request, object, nullptr, 0, caller);
    {
        const AggregateScope scope{request};
        native_aggregate_clear(object);
    }
    AggregateState("DMC_AGG_CLEAR_END", request, object, nullptr, 0, caller);
}

void PS4_SYSV_ABI ObserveAggregateFlush(void* object) {
    if (!PadTrace::Enabled() || !AggregateBudget(3)) {
        return native_aggregate_flush(object);
    }
    const u64 request = PadTrace::NextId();
    const u64 caller = reinterpret_cast<u64>(__builtin_return_address(0));
    AggregateState("DMC_AGG_FLUSH_BEGIN", request, object, nullptr, 0, caller);
    {
        const AggregateScope scope{request};
        native_aggregate_flush(object);
    }
    AggregateState("DMC_AGG_FLUSH_END", request, object, nullptr, 0, caller);
}

void AbsoluteJump(u8* output, u64 target) {
    constexpr std::array<u8, 6> instruction{0xff, 0x25, 0, 0, 0, 0};
    std::memcpy(output, instruction.data(), instruction.size());
    std::memcpy(output + instruction.size(), &target, sizeof(target));
}

template <std::size_t Size>
void* NativeEntry(u64 address, u64 observer, const std::array<u8, Size>& expected) {
    static_assert(Size >= 14);
    std::array<u8, Size> bytes{};
    if (!Read(address, bytes.data(), bytes.size()) || bytes != expected) {
        return nullptr;
    }
    auto* trampoline = static_cast<u8*>(
        VirtualAlloc(nullptr, Size + 14, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (trampoline == nullptr) {
        return nullptr;
    }
    std::memcpy(trampoline, bytes.data(), Size);
    AbsoluteJump(trampoline + Size, address + Size);
    DWORD previous = 0;
    if (!VirtualProtect(trampoline, Size + 14, PAGE_EXECUTE_READ, &previous)) {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        return nullptr;
    }
    FlushInstructionCache(GetCurrentProcess(), trampoline, Size + 14);
    if (!VirtualProtect(reinterpret_cast<void*>(address), Size, PAGE_EXECUTE_READWRITE,
                        &previous)) {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        return nullptr;
    }
    std::array<u8, Size> patch{};
    patch.fill(0x90);
    AbsoluteJump(patch.data(), observer);
    std::memcpy(reinterpret_cast<void*>(address), patch.data(), patch.size());
    DWORD unused = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(address), Size, previous, &unused)) {
        LOG_WARNING(Input, "[DMC TRACE] Native probe page protection could not be restored");
    }
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(address), Size);
    return trampoline;
}

void PS4_SYSV_ABI ObserveLet(void* object, void* frame, void* result) {
    const auto handler = original.load(std::memory_order_acquire);
    if (!PadTrace::Enabled()) {
        return handler(object, frame, result);
    }
    ResetCaptureCache();
    if (probe_cache.records >= 4096) {
        return handler(object, frame, result);
    }
    const u64 code = Operand(frame);
    auto& site = probe_cache.sites[(code >> 2) % probe_cache.sites.size()];
    if (site.code != code) {
        site = {};
        site.code = code;
        const auto* operand = reinterpret_cast<const u8*>(code);
        site.matched =
            code != 0 && operand[0] == 0x19 && operand[1] == 0x01 && Decode(code, site.assignment);
        site.cleanup = site.matched && IsCleanupCandidate(code, site.assignment);
        Assignment next;
        site.action_candidate = site.matched && site.assignment.value == 0x3f800000 &&
                                Decode(code + 36, next) && next.value == 0x3f800000 &&
                                next.owner_property == site.assignment.owner_property &&
                                next.property != site.assignment.property;
    }
    const Assignment assignment = site.assignment;
    const bool cleanup = site.cleanup;
    if (cleanup) {
        charge_property.store(assignment.property, std::memory_order_relaxed);
    }
    if (cleanup || site.action_candidate) {
        WatchAction(object, frame, assignment, cleanup);
    }
    const bool tracked =
        site.matched && assignment.property == charge_property.load(std::memory_order_relaxed);
    const bool discovery = site.matched && !site.reported && probe_cache.discoveries < 128;
    const bool matched = site.matched && (FindAction(object) != nullptr || discovery || tracked) &&
                         probe_cache.records < 4096;
    const u64 request = matched ? PadTrace::NextId() : 0;
    if (matched) {
        ++probe_cache.records;
        site.reported = true;
        if (discovery) {
            ++probe_cache.discoveries;
        }
        PadTrace::Record("DMC_LET_BEGIN", {static_cast<s64>(request), reinterpret_cast<s64>(object),
                                           reinterpret_cast<s64>(frame), static_cast<s64>(code),
                                           static_cast<s64>(assignment.property),
                                           static_cast<s64>(assignment.owner_property),
                                           assignment.value, cleanup, tracked});
        if (discovery) {
            FrameSite(request, frame);
        }
    }
    handler(object, frame, result);
    if (matched) {
        const u64 destination = LiveWord(guest_base + 0x3940bc0);
        const u64 property = LiveWord(guest_base + 0x3940b90);
        u32 actual = 0;
        const bool readable = destination != 0 && property == assignment.property;
        if (readable) {
            std::memcpy(&actual, reinterpret_cast<const void*>(destination), sizeof(actual));
        }
        PadTrace::Record("DMC_LET_END",
                         {static_cast<s64>(request), static_cast<s64>(destination),
                          static_cast<s64>(property), actual, readable,
                          property == assignment.property, static_cast<s64>(Operand(frame))});
        if (probe_cache.records == 4096) {
            PadTrace::Record("DMC_PROBE_LIMIT", {4096});
        }
    }
}

struct BranchSite {
    u64 code = 0;
    u64 object = 0;
    bool matched = false;
    bool reported = false;
    u64 next = 0;
};

struct BranchCache {
    u64 generation = 0;
    std::array<BranchSite, 4096> sites{};
};

thread_local BranchCache branch_cache;

void PS4_SYSV_ABI ObserveJump(void* object, void* frame, void* result) {
    const auto handler = original_jump.load(std::memory_order_acquire);
    if (!PadTrace::Enabled()) {
        return handler(object, frame, result);
    }
    ResetCaptureCache();
    auto* action = FindAction(object);
    if (action == nullptr || action->records >= 512) {
        return handler(object, frame, result);
    }
    const u64 generation = capture_generation.load(std::memory_order_relaxed);
    if (branch_cache.generation != generation) {
        branch_cache = {};
        branch_cache.generation = generation;
    }
    const u64 code = Operand(frame);
    const u64 object_address = reinterpret_cast<u64>(object);
    auto& site = branch_cache.sites[((code >> 2) ^ (code >> 15) ^ (object_address >> 4)) %
                                    branch_cache.sites.size()];
    if ((site.code != code || site.object != object_address) && action->probes < 256) {
        ++action->probes;
        site = {};
        site.code = code;
        site.object = object_address;
        const auto* expression = reinterpret_cast<const u8*>(code + 2);
        site.matched = code != 0 && expression[0] == 0x19 &&
                       (expression[1] == 0x01 || (expression[1] == 0x19 && expression[2] == 0x01));
        if (action->probes == 256) {
            PadTrace::Record("DMC_BRANCH_LIMIT",
                             {action->probes, action->records, static_cast<s64>(object_address)});
        }
    }
    const bool matched = site.code == code && site.object == object_address && site.matched;
    const u64 node = matched ? LiveWord(reinterpret_cast<u64>(frame) + 0x18) : 0;
    handler(object, frame, result);
    if (!matched) {
        return;
    }
    const u64 next = Operand(frame);
    if (site.code != code || site.object != object_address) {
        return;
    }
    const bool start =
        node != 0 && action->start_name != 0 && LiveWord(node + 0x48) == action->start_name;
    if (!site.reported || site.next != next || start) {
        const bool first = !site.reported;
        const bool changed = site.reported && site.next != next;
        site.reported = true;
        site.next = next;
        ++action->records;
        const u64 request = PadTrace::NextId();
        PadTrace::Record("DMC_BRANCH", {static_cast<s64>(request), reinterpret_cast<s64>(object),
                                        reinterpret_cast<s64>(frame), static_cast<s64>(node),
                                        static_cast<s64>(code), static_cast<s64>(next), changed,
                                        first, action->probes, action->records, start});
        if (first && action->records <= 64) {
            FrameSite(request, frame);
            std::array<u64, 4> words{};
            if (Read(code, words.data(), sizeof(words))) {
                PadTrace::Record("DMC_BRANCH_CODE",
                                 {static_cast<s64>(request), static_cast<s64>(words[0]),
                                  static_cast<s64>(words[1]), static_cast<s64>(words[2]),
                                  static_cast<s64>(words[3])});
            }
        }
        if (action->records == 512) {
            PadTrace::Record("DMC_BRANCH_LIMIT",
                             {action->probes, action->records, static_cast<s64>(object_address)});
        }
    }
}
} // namespace

void InstallNative() {
    if (native_held != nullptr || native_clear != nullptr || native_aggregate_event != nullptr ||
        native_aggregate_clear != nullptr || native_aggregate_flush != nullptr ||
        Common::ElfInfo::Instance().GameSerial() != "CUSA01022" ||
        Common::ElfInfo::Instance().AppVer() != "01.03") {
        return;
    }
    const char* setting = std::getenv("SHADPS4_DMC_NATIVE");
    if (setting != nullptr && std::strcmp(setting, "0") == 0) {
        return;
    }
    const u64 base = MemoryPatcher::g_eboot_address;
    if (base == 0) {
        return;
    }
    guest_base = base;
    const char* topology_setting = std::getenv("SHADPS4_DMC_TOPOLOGY");
    discover_topology = topology_setting != nullptr && std::strcmp(topology_setting, "1") == 0;
    constexpr char held_name[] = "UNTInputexecIsHeld";
    constexpr char clear_name[] = "UNTActionGraph_NodeexecClearBufferedInput";
    std::array<char, sizeof(held_name)> held_registration{};
    std::array<char, sizeof(clear_name)> clear_registration{};
    if (!Read(base + 0x25cbbd9, held_registration.data(), held_registration.size()) ||
        !Read(base + 0x25c065c, clear_registration.data(), clear_registration.size()) ||
        std::memcmp(held_registration.data(), held_name, sizeof(held_name)) != 0 ||
        std::memcmp(clear_registration.data(), clear_name, sizeof(clear_name)) != 0) {
        LOG_WARNING(Input, "[DMC TRACE] Native registrations differ; entry probes skipped");
        return;
    }
    constexpr std::array<u8, 14> held_bytes{0x8b, 0x87, 0x20, 0x02, 0x00, 0x00, 0x0f,
                                            0xa3, 0xf0, 0x19, 0xc0, 0x83, 0xe0, 0x01};
    constexpr std::array<u8, 17> clear_bytes{0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x53, 0x49, 0x89,
                                             0xf6, 0x48, 0x89, 0xfb, 0x48, 0x8b, 0x43, 0x70};
    native_held = reinterpret_cast<HeldNative>(
        NativeEntry(base + 0x91d750, reinterpret_cast<u64>(&ObserveHeld), held_bytes));
    native_clear = reinterpret_cast<ClearNative>(
        NativeEntry(base + 0xa21c90, reinterpret_cast<u64>(&ObserveClear), clear_bytes));
    constexpr std::array<u8, 17> event_bytes{0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41,
                                             0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xec, 0x18};
    constexpr std::array<u8, 17> aggregate_clear_bytes{0x55, 0x48, 0x89, 0xe5, 0x41, 0x56,
                                                       0x53, 0x49, 0x89, 0xfe, 0x41, 0x8b,
                                                       0x86, 0x80, 0x00, 0x00, 0x00};
    constexpr std::array<u8, 20> flush_bytes{0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41,
                                             0x56, 0x53, 0x50, 0x48, 0x89, 0xfb, 0x83,
                                             0xbb, 0x80, 0x00, 0x00, 0x00, 0x00};
    native_aggregate_event = reinterpret_cast<AggregateEvent>(
        NativeEntry(base + 0xa1f980, reinterpret_cast<u64>(&ObserveAggregateEvent), event_bytes));
    native_aggregate_clear = reinterpret_cast<AggregateOperation>(NativeEntry(
        base + 0xa1f5d0, reinterpret_cast<u64>(&ObserveAggregateClear), aggregate_clear_bytes));
    native_aggregate_flush = reinterpret_cast<AggregateOperation>(
        NativeEntry(base + 0xa1f220, reinterpret_cast<u64>(&ObserveAggregateFlush), flush_bytes));
    auto graph_bytes = event_bytes;
    graph_bytes.back() = 0x38;
    native_buffer_event = reinterpret_cast<BufferEvent>(
        NativeEntry(base + 0xa1c3c0, reinterpret_cast<u64>(&ObserveBufferEvent), event_bytes));
    native_graph_event = reinterpret_cast<GraphEvent>(
        NativeEntry(base + 0xa1fe10, reinterpret_cast<u64>(&ObserveGraphEvent), graph_bytes));
    constexpr std::array<u8, 14> active_bytes{0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41,
                                              0x56, 0x41, 0x54, 0x53, 0x49, 0x89, 0xf6};
    constexpr std::array<u8, 17> buffer_switch_bytes{0x55, 0x48, 0x89, 0xe5, 0x41, 0x56,
                                                     0x53, 0x49, 0x89, 0xfe, 0x41, 0x8b,
                                                     0x86, 0x80, 0x00, 0x00, 0x00};
    native_active_switch = reinterpret_cast<OwnerSwitch>(
        NativeEntry(base + 0xa1e2c0, reinterpret_cast<u64>(&ObserveActiveSwitch), active_bytes));
    native_buffer_switch = reinterpret_cast<OwnerSwitch>(NativeEntry(
        base + 0xa1e510, reinterpret_cast<u64>(&ObserveBufferSwitch), buffer_switch_bytes));
    auto select_bytes = event_bytes;
    select_bytes.back() = 0x38;
    auto dispatch_bytes = event_bytes;
    dispatch_bytes.back() = 0x58;
    constexpr std::array<u8, 14> match_bytes{0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41,
                                             0x56, 0x41, 0x54, 0x53, 0x49, 0x89, 0xf7};
    constexpr std::array<u8, 16> exit_bytes{0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56,
                                            0x53, 0x50, 0x49, 0x89, 0xf6, 0x49, 0x89, 0xff};
    native_match_event = reinterpret_cast<GraphEvent>(
        NativeEntry(base + 0xa26420, reinterpret_cast<u64>(&ObserveGateMatch), match_bytes));
    native_condition_dispatch = reinterpret_cast<ConditionDispatch>(NativeEntry(
        base + 0x6d1e0, reinterpret_cast<u64>(&ObserveConditionDispatch), dispatch_bytes));
    if (native_match_event != nullptr && native_condition_dispatch != nullptr) {
        native_select_link = reinterpret_cast<SelectLink>(
            NativeEntry(base + 0xa20850, reinterpret_cast<u64>(&ObserveSelectLink), select_bytes));
    }
    native_node_exit = reinterpret_cast<NodeExit>(
        NativeEntry(base + 0xa1e650, reinterpret_cast<u64>(&ObserveNodeExit), exit_bytes));
    constexpr std::array<u8, 16> fire_start_bytes{0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56,
                                                  0x53, 0x48, 0x83, 0xec, 0x18, 0x49, 0x89, 0xff};
    constexpr std::array<u8, 16> awake_bytes{0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56,
                                             0x53, 0x48, 0x83, 0xec, 0x18, 0x48, 0x89, 0xf3};
    native_fire_start = reinterpret_cast<AggregateOperation>(
        NativeEntry(base + 0xa17f00, reinterpret_cast<u64>(&ObserveFireStart), fire_start_bytes));
    native_timeline_tick = reinterpret_cast<TimelineTick>(
        NativeEntry(base + 0xa28dc0, reinterpret_cast<u64>(&ObserveTimelineTick), event_bytes));
    auto item_bytes = event_bytes;
    item_bytes.back() = 0x78;
    native_timeline_item = reinterpret_cast<OwnerSwitch>(
        NativeEntry(base + 0xa28850, reinterpret_cast<u64>(&ObserveTimelineItem), item_bytes));
    native_awake = reinterpret_cast<Native>(
        NativeEntry(base + 0x20b8f20, reinterpret_cast<u64>(&ObserveAwake), awake_bytes));
    LOG_INFO(Input, "[DMC TRACE] Native probes before guest execution: held={}, node_clear={}",
             native_held != nullptr, native_clear != nullptr);
    LOG_INFO(Input, "[DMC TRACE] Aggregator probes: event={}, clear={}, flush={}",
             native_aggregate_event != nullptr, native_aggregate_clear != nullptr,
             native_aggregate_flush != nullptr);
    LOG_INFO(Input, "[DMC TRACE] Sparse handoff probes: buffer={}, graph={}",
             native_buffer_event != nullptr, native_graph_event != nullptr);
    LOG_INFO(Input, "[DMC TRACE] Pending ownership probes: active={}, buffer={}",
             native_active_switch != nullptr, native_buffer_switch != nullptr);
    LOG_INFO(Input, "[DMC TRACE] Filtered gate probes: select={}, match={}, condition={}, exit={}",
             native_select_link != nullptr, native_match_event != nullptr,
             native_condition_dispatch != nullptr, native_node_exit != nullptr);
    LOG_INFO(Input,
             "[DMC TRACE] Shot timeline probes: start={}, tick={}, awake={}, item={}; topology={}",
             native_fire_start != nullptr, native_timeline_tick != nullptr, native_awake != nullptr,
             native_timeline_item != nullptr, discover_topology);
}

void Install() {
    capture_generation.fetch_add(1, std::memory_order_relaxed);
    PadTrace::Record("DMC_NATIVE_HOOK", {native_held != nullptr, native_clear != nullptr});
    PadTrace::Record("DMC_AGG_HOOK",
                     {native_aggregate_event != nullptr, native_aggregate_clear != nullptr,
                      native_aggregate_flush != nullptr});
    PadTrace::Record("DMC_HANDOFF_HOOK",
                     {native_buffer_event != nullptr, native_graph_event != nullptr, 0, 0});
    PadTrace::Record("DMC_HANDOFF_SPARSE", {1, 256, 512, 256});
    PadTrace::Record("DMC_OWNER_HOOK",
                     {native_active_switch != nullptr, native_buffer_switch != nullptr});
    PadTrace::Record("DMC_GATE_HOOK", {native_select_link != nullptr, native_match_event != nullptr,
                                       native_condition_dispatch != nullptr,
                                       native_node_exit != nullptr, 256, 32, 32, 64});
    PadTrace::Record("DMC_FIRE_HOOK",
                     {native_fire_start != nullptr, native_timeline_tick != nullptr,
                      native_awake != nullptr, 128, 128, 50, discover_topology,
                      native_timeline_item != nullptr, 64});
    if (original.load() != nullptr || Common::ElfInfo::Instance().GameSerial() != "CUSA01022" ||
        Common::ElfInfo::Instance().AppVer() != "01.03") {
        return;
    }
    const u64 base = MemoryPatcher::g_eboot_address;
    constexpr std::array<u8, 17> expected = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41,
                                             0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xec, 0x18};
    std::array<u8, expected.size()> bytes{};
    constexpr std::array<u8, 7> expected_address = {0x4c, 0x8d, 0x3d, 0xb7, 0xa3, 0x8e, 0x03};
    constexpr std::array<u8, 7> expected_property = {0x48, 0x8d, 0x05, 0xc6, 0xa2, 0x8e, 0x03};
    std::array<u8, 7> address_bytes{}, property_bytes{};
    const u64 slot = base + 0x3960040 + 0x0f * 16;
    MEMORY_BASIC_INFORMATION info{};
    if (base == 0 || !Read(base + 0x567e0, bytes.data(), bytes.size()) || bytes != expected ||
        !Read(base + 0x56802, address_bytes.data(), address_bytes.size()) ||
        address_bytes != expected_address ||
        !Read(base + 0x568c3, property_bytes.data(), property_bytes.size()) ||
        property_bytes != expected_property || Word(slot) != base + 0x567e0 ||
        Word(slot + 8) != 0 ||
        VirtualQuery(reinterpret_cast<void*>(slot), &info, sizeof(info)) == 0 ||
        (info.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) == 0) {
        LOG_WARNING(Input,
                    "[DMC TRACE] Interpreter validation failed; observation hook not installed");
        PadTrace::Record("DMC_HOOK", {0, static_cast<s64>(base), static_cast<s64>(Word(slot))});
        return;
    }
    guest_base = base;
    original.store(reinterpret_cast<Native>(base + 0x567e0), std::memory_order_release);
    const auto before = InterlockedCompareExchange64(reinterpret_cast<volatile LONG64*>(slot),
                                                     reinterpret_cast<LONG64>(&ObserveLet),
                                                     static_cast<LONG64>(base + 0x567e0));
    if (static_cast<u64>(before) != base + 0x567e0) {
        original.store(nullptr);
        LOG_WARNING(Input, "[DMC TRACE] Interpreter entry changed; observation hook not installed");
        return;
    }
    PadTrace::Record("DMC_HOOK", {1, static_cast<s64>(base), static_cast<s64>(slot)});
    const u64 jump_slot = base + 0x3960040 + 0x07 * 16;
    const u64 jump_target = Word(jump_slot);
    if (jump_target >= base && jump_target < base + 0x2f7b360 && Word(jump_slot + 8) == 0) {
        original_jump.store(reinterpret_cast<Native>(jump_target), std::memory_order_release);
        const auto prior = InterlockedCompareExchange64(
            reinterpret_cast<volatile LONG64*>(jump_slot), reinterpret_cast<LONG64>(&ObserveJump),
            static_cast<LONG64>(jump_target));
        PadTrace::Record("DMC_HOOK_BRANCH",
                         {static_cast<u64>(prior) == jump_target, static_cast<s64>(jump_target)});
    }
    LOG_INFO(
        Input,
        "[DMC TRACE] Script assignment observer installed; cleanup identification is provisional");
}
} // namespace Input::DmcChargeTrace
#else
namespace Input::DmcChargeTrace {
void InstallNative() {}
void Install() {}
} // namespace Input::DmcChargeTrace
#endif
