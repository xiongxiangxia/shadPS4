// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#ifndef ENABLE_DMC_CHARGE_COMPAT
namespace LightTrace {
constexpr u32 record_budget = 12000;
std::atomic<u32> records{0};
std::atomic<u64> ready_generation{0};
std::atomic<u64> shared_world{0};
std::atomic<u64> shared_input{0};
std::atomic<u64> shared_aggregator{0};
std::atomic<u32> timeline_count{0};
std::array<std::atomic<u64>, 16> timelines{};
bool selected = true;
u32 hooks = 0;
Native held_time = nullptr;

struct State {
    u64 generation = 0;
    u64 world = 0;
    u64 input = 0;
    u64 aggregator = 0;
    u64 parent = 0;
    u64 phases = 0;
    u64 cost_ns = 0;
    u64 max_cost_ns = 0;
};
thread_local State state;

bool Active() {
    if (!PadTrace::Enabled() || records.load(std::memory_order_relaxed) >= record_budget) {
        return false;
    }
    const u64 generation = capture_generation.load(std::memory_order_relaxed);
    if (generation == 0 || ready_generation.load(std::memory_order_acquire) != generation) {
        return false;
    }
    if (state.generation != generation) {
        state = {};
        state.generation = generation;
    }
    state.world = shared_world.load(std::memory_order_relaxed);
    state.input = shared_input.load(std::memory_order_relaxed);
    state.aggregator = shared_aggregator.load(std::memory_order_relaxed);
    return true;
}

template <typename T>
bool Copy(u64 address, T& output) {
    if (address == 0 || sizeof(T) > std::numeric_limits<u64>::max() - address) {
        return false;
    }
    SIZE_T count = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(address), &output,
                             sizeof(T), &count) &&
           count == sizeof(T);
}

u64 Pointer(u64 address) {
    u64 value = 0;
    Copy(address, value);
    return value;
}

u32 Bits(u64 address) {
    u32 value = 0;
    Copy(address, value);
    return value;
}

bool Emit(const char* kind, std::initializer_list<s64> fields) {
    const u32 index = records.fetch_add(1, std::memory_order_relaxed);
    if (index >= record_budget) {
        return false;
    }
    PadTrace::Record(kind, fields);
    if (index + 1 == record_budget) {
        PadTrace::Record("DMC_LIGHT_LIMIT", {record_budget});
    }
    return true;
}

struct Cost {
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    ~Cost() {
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - start)
                            .count();
        state.cost_ns += ns;
        state.max_cost_ns = std::max(state.max_cost_ns, static_cast<u64>(ns));
        if (++state.phases % 128 == 0) {
            Emit("DMC_LIGHT_COST", {static_cast<s64>(state.phases), static_cast<s64>(state.cost_ns),
                                    static_cast<s64>(state.max_cost_ns)});
        }
    }
};

u32 WorldTime() {
    return state.world != 0 ? Bits(state.world + 0x690) : 0;
}

void World() {
    const u64 engine = Pointer(guest_base + 0x3a548e8);
    const u64 contexts = engine != 0 ? Pointer(engine + 0x80) : 0;
    const u64 current = contexts != 0 ? Pointer(contexts + 0x60) : 0;
    state.world = current != 0 ? Pointer(current) : 0;
    shared_world.store(state.world, std::memory_order_relaxed);
}

void Queue(u64 request, u32 phase, u64 aggregator) {
    std::array<u64, 12> words{};
    const bool valid = aggregator != 0 && Copy(aggregator + 0x78, words);
    Emit("DMC_LIGHT_QUEUE",
         {static_cast<s64>(request), phase, static_cast<s64>(aggregator), valid,
          static_cast<s64>(words[0]), static_cast<u32>(words[1]), static_cast<s64>(words[5]),
          static_cast<s64>(words[6]), static_cast<u32>(words[7]), static_cast<u32>(words[9]),
          static_cast<u32>(words[11]), WorldTime()});
}

struct Parent {
    u64 previous = state.parent;
    explicit Parent(u64 request) {
        state.parent = request;
    }
    ~Parent() {
        state.parent = previous;
    }
};

bool Tracked(u64 timeline) {
    if (timeline == 0) {
        return false;
    }
    return std::any_of(timelines.begin(), timelines.end(), [timeline](const auto& entry) {
        return entry.load(std::memory_order_relaxed) == timeline;
    });
}

void PS4_SYSV_ABI HeldTime(void* object, void* frame, void* result) {
    if (!Active()) {
        return held_time(object, frame, result);
    }
    u64 request = 0;
    u64 code = 0;
    u64 function = 0;
    const u64 input = reinterpret_cast<u64>(object);
    {
        const Cost cost;
        const u32 mask = Bits(input + 0x220);
        if ((mask & 0x40) != 0) {
            state.input = input;
            shared_input.store(input, std::memory_order_relaxed);
            World();
            request = PadTrace::NextId();
            const u64 address = reinterpret_cast<u64>(frame);
            code = Pointer(address + 0x28);
            function = Pointer(address + 0x18);
            Emit("DMC_LIGHT_HELD_BEGIN",
                 {static_cast<s64>(request), static_cast<s64>(state.parent),
                  static_cast<s64>(input), static_cast<s64>(function), static_cast<s64>(code), mask,
                  Bits(input + 0x240), static_cast<s64>(state.world), WorldTime()});
        }
    }
    held_time(object, frame, result);
    if (request != 0) {
        const Cost cost;
        Emit("DMC_LIGHT_HELD_END",
             {static_cast<s64>(request), Bits(reinterpret_cast<u64>(result)), WorldTime()});
    }
}

u32 PS4_SYSV_ABI Aggregate(void* object, void* event, u32 input) {
    if (!Active()) {
        return native_aggregate_event(object, event, input);
    }
    const u64 aggregator = reinterpret_cast<u64>(object);
    u64 request = 0;
    {
        const Cost cost;
        std::array<u8, 0x48> bytes{};
        const bool valid = event != nullptr && Copy(reinterpret_cast<u64>(event) + 0x60, bytes);
        u64 descriptor = 0;
        u32 held = 0;
        std::memcpy(&descriptor, bytes.data(), sizeof(descriptor));
        std::memcpy(&held, bytes.data() + 0x40, sizeof(held));
        const u32 type = bytes[0x18];
        if (valid && held == 8 && type <= 2) {
            state.aggregator = aggregator;
            shared_aggregator.store(aggregator, std::memory_order_relaxed);
        }
        if (state.aggregator == aggregator && aggregator != 0) {
            World();
            request = PadTrace::NextId();
            Emit("DMC_LIGHT_EVENT",
                 {static_cast<s64>(request), static_cast<s64>(state.parent),
                  static_cast<s64>(aggregator), reinterpret_cast<s64>(event), input, valid,
                  static_cast<s64>(descriptor), type, held, bytes[0x30],
                  static_cast<s64>(Pointer(descriptor + 0x48)),
                  reinterpret_cast<s64>(__builtin_return_address(0)), WorldTime()});
            Queue(request, 0, aggregator);
        }
    }
    u32 value = 0;
    {
        const Parent parent{request != 0 ? request : state.parent};
        value = native_aggregate_event(object, event, input);
    }
    if (request != 0) {
        const Cost cost;
        Queue(request, 1, aggregator);
        Emit("DMC_LIGHT_EVENT_END", {static_cast<s64>(request), value, WorldTime()});
    }
    return value;
}

u32 PS4_SYSV_ABI Buffer(void* object, void* event, void* first, void* second, void* flags) {
    if (!Active() || state.aggregator == 0) {
        return native_buffer_event(object, event, first, second, flags);
    }
    const u64 request = PadTrace::NextId();
    {
        const Cost cost;
        Emit("DMC_LIGHT_BUFFER", {static_cast<s64>(request), static_cast<s64>(state.parent),
                                  reinterpret_cast<s64>(object), reinterpret_cast<s64>(event),
                                  reinterpret_cast<s64>(first), reinterpret_cast<s64>(second),
                                  reinterpret_cast<s64>(flags), WorldTime()});
        Queue(request, 0, state.aggregator);
    }
    u32 value = 0;
    {
        const Parent parent{request};
        value = native_buffer_event(object, event, first, second, flags);
    }
    {
        const Cost cost;
        Queue(request, 1, state.aggregator);
        Emit("DMC_LIGHT_BUFFER_END", {static_cast<s64>(request), value});
    }
    return value;
}

u32 PS4_SYSV_ABI Graph(void* object, void* event) {
    if (!Active() || state.aggregator == 0) {
        return native_graph_event(object, event);
    }
    const u64 graph = reinterpret_cast<u64>(object);
    const u64 caller = reinterpret_cast<u64>(__builtin_return_address(0));
    u64 before = 0;
    bool tracked = false;
    {
        const Cost cost;
        tracked = graph != 0 && Pointer(graph + 0x78) == state.aggregator;
        if (tracked) {
            before = Pointer(graph + 0x88);
        }
    }
    const u32 value = native_graph_event(object, event);
    if (tracked) {
        const Cost cost;
        const u64 after = Pointer(graph + 0x88);
        const bool replay = caller == guest_base + 0xa1f8e3 || caller == guest_base + 0xa1f913;
        if (value != 0 || before != after || replay) {
            Emit("DMC_LIGHT_GRAPH",
                 {static_cast<s64>(state.parent), static_cast<s64>(graph),
                  reinterpret_cast<s64>(event), static_cast<s64>(before), static_cast<s64>(after),
                  value, static_cast<s64>(caller), replay, WorldTime()});
        }
    }
    return value;
}

u32 PS4_SYSV_ABI Clear(void* object, void* event) {
    if (!Active() || state.aggregator == 0) {
        return native_clear(object, event);
    }
    const u64 node = reinterpret_cast<u64>(object);
    const u64 graph = node != 0 ? Pointer(node + 0x70) : 0;
    if (graph == 0 || Pointer(graph + 0x78) != state.aggregator) {
        return native_clear(object, event);
    }
    const u64 request = PadTrace::NextId();
    {
        const Cost cost;
        Emit("DMC_LIGHT_CLEAR",
             {static_cast<s64>(request), static_cast<s64>(state.parent), static_cast<s64>(node),
              reinterpret_cast<s64>(event), WorldTime()});
        Queue(request, 0, state.aggregator);
    }
    u32 value = 0;
    {
        const Parent parent{request};
        value = native_clear(object, event);
    }
    {
        const Cost cost;
        Queue(request, 1, state.aggregator);
        Emit("DMC_LIGHT_CLEAR_END", {static_cast<s64>(request), value, WorldTime()});
    }
    return value;
}

void PS4_SYSV_ABI Start(void* object) {
    if (!Active() || state.aggregator == 0) {
        return native_fire_start(object);
    }
    const u64 action = reinterpret_cast<u64>(object);
    u64 timeline = 0;
    u64 request = 0;
    {
        const Cost cost;
        const u64 node = Pointer(action + 0x68);
        const u64 graph = node != 0 ? Pointer(node + 0x70) : 0;
        if (graph != 0 && Pointer(graph + 0x78) == state.aggregator) {
            timeline = Pointer(action + 0x60);
            if (timeline != 0 && !Tracked(timeline)) {
                const u32 index = timeline_count.fetch_add(1, std::memory_order_relaxed);
                timelines[index % timelines.size()].store(timeline, std::memory_order_relaxed);
            }
            request = PadTrace::NextId();
            Emit("DMC_LIGHT_START",
                 {static_cast<s64>(request), static_cast<s64>(state.parent),
                  static_cast<s64>(action), static_cast<s64>(node), static_cast<s64>(graph),
                  static_cast<s64>(timeline), static_cast<s64>(Pointer(action + 0x48)),
                  static_cast<s64>(Pointer(node + 0x48)), WorldTime(),
                  timeline_count.load(std::memory_order_relaxed)});
        }
    }
    {
        const Parent parent{request != 0 ? request : state.parent};
        native_fire_start(object);
    }
    if (request != 0) {
        const Cost cost;
        Emit("DMC_LIGHT_START_END",
             {static_cast<s64>(request), timeline != 0 ? Bits(timeline + 0x8c) : 0, WorldTime()});
    }
}

void PS4_SYSV_ABI Tick(void* object, float delta) {
    if (!Active() || !Tracked(reinterpret_cast<u64>(object))) {
        return native_timeline_tick(object, delta);
    }
    const u64 request = PadTrace::NextId();
    const u64 timeline = reinterpret_cast<u64>(object);
    {
        const Cost cost;
        std::array<u32, 4> times{};
        const bool valid = state.world != 0 && Copy(state.world + 0x690, times);
        Emit("DMC_LIGHT_TICK",
             {static_cast<s64>(request), static_cast<s64>(state.parent), static_cast<s64>(timeline),
              std::bit_cast<u32>(delta), Bits(timeline + 0x8c), valid, times[0], times[1], times[2],
              times[3], static_cast<s64>(state.input),
              state.input != 0 ? Bits(state.input + 0x220) : 0,
              state.input != 0 ? Bits(state.input + 0x240) : 0});
    }
    {
        const Parent parent{request};
        native_timeline_tick(object, delta);
    }
    {
        const Cost cost;
        Emit("DMC_LIGHT_TICK_END", {static_cast<s64>(request), Bits(timeline + 0x8c), WorldTime()});
    }
}

void PS4_SYSV_ABI Item(void* object, void* item) {
    if (!Active() || !Tracked(reinterpret_cast<u64>(object))) {
        return native_timeline_item(object, item);
    }
    const u64 request = PadTrace::NextId();
    {
        const Cost cost;
        std::array<u64, 9> words{};
        const bool valid = item != nullptr && Copy(reinterpret_cast<u64>(item), words);
        Emit("DMC_LIGHT_ITEM",
             {static_cast<s64>(request), static_cast<s64>(state.parent),
              reinterpret_cast<s64>(object), reinterpret_cast<s64>(item), valid,
              static_cast<s64>(words[0]), static_cast<s64>(words[1]), static_cast<s64>(words[2]),
              static_cast<s64>(words[3]), static_cast<s64>(words[4]), static_cast<s64>(words[5]),
              static_cast<s64>(words[6]), static_cast<s64>(words[7]), static_cast<s64>(words[8]),
              WorldTime()});
    }
    {
        const Parent parent{request};
        native_timeline_item(object, item);
    }
    {
        const Cost cost;
        Emit("DMC_LIGHT_ITEM_END", {static_cast<s64>(request), WorldTime()});
    }
}

void PS4_SYSV_ABI Awake(void* object, void* frame, void* result) {
    if (!Active() || state.aggregator == 0) {
        return native_awake(object, frame, result);
    }
    const u64 node = reinterpret_cast<u64>(object);
    const u64 graph = Pointer(node + 0x70);
    if (graph == 0 || Pointer(graph + 0x78) != state.aggregator) {
        return native_awake(object, frame, result);
    }
    const u64 request = PadTrace::NextId();
    {
        const Cost cost;
        Emit("DMC_LIGHT_AWAKE",
             {static_cast<s64>(request), static_cast<s64>(state.parent), static_cast<s64>(node),
              static_cast<s64>(graph), Bits(node + 0xd0), WorldTime()});
        Queue(request, 0, state.aggregator);
    }
    {
        const Parent parent{request};
        native_awake(object, frame, result);
    }
    {
        const Cost cost;
        Queue(request, 1, state.aggregator);
        Emit("DMC_LIGHT_AWAKE_END", {static_cast<s64>(request), Bits(node + 0xd0), WorldTime()});
    }
}

void PS4_SYSV_ABI Switch(void* object, void* incoming) {
    if (!Active() || reinterpret_cast<u64>(object) != state.aggregator) {
        return native_active_switch(object, incoming);
    }
    const u64 request = PadTrace::NextId();
    {
        const Cost cost;
        Emit("DMC_LIGHT_SWITCH",
             {static_cast<s64>(request), static_cast<s64>(state.parent),
              reinterpret_cast<s64>(object), reinterpret_cast<s64>(incoming), WorldTime()});
        Queue(request, 0, state.aggregator);
    }
    {
        const Parent parent{request};
        native_active_switch(object, incoming);
    }
    {
        const Cost cost;
        Queue(request, 1, state.aggregator);
        Emit("DMC_LIGHT_SWITCH_END", {static_cast<s64>(request), WorldTime()});
    }
}

void Install(u64 base) {
    static bool attempted = false;
    if (attempted) {
        return;
    }
    attempted = true;
    constexpr std::array<u8, 17> prologue{0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41,
                                          0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xec, 0x18};
    constexpr std::array<u8, 15> held_bytes{0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56,
                                            0x41, 0x54, 0x53, 0x48, 0x83, 0xec, 0x10};
    constexpr std::array<u8, 16> start_bytes{0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56,
                                             0x53, 0x48, 0x83, 0xec, 0x18, 0x49, 0x89, 0xff};
    constexpr std::array<u8, 16> awake_bytes{0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56,
                                             0x53, 0x48, 0x83, 0xec, 0x18, 0x48, 0x89, 0xf3};
    constexpr std::array<u8, 14> switch_bytes{0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41,
                                              0x56, 0x41, 0x54, 0x53, 0x49, 0x89, 0xf6};
    constexpr std::array<u8, 17> clear_bytes{0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x53, 0x49, 0x89,
                                             0xf6, 0x48, 0x89, 0xfb, 0x48, 0x8b, 0x43, 0x70};
    auto item_bytes = prologue;
    item_bytes.back() = 0x78;
    auto graph_bytes = prologue;
    graph_bytes.back() = 0x38;
    const auto install = [&](auto& target, u64 offset, u64 observer, const auto& expected,
                             u32 bit) {
        target = reinterpret_cast<std::remove_reference_t<decltype(target)>>(
            NativeEntry(base + offset, observer, expected));
        if (target != nullptr) {
            hooks |= bit;
        }
    };
    install(held_time, 0x20dc680, reinterpret_cast<u64>(&HeldTime), held_bytes, 1);
    install(native_aggregate_event, 0xa1f980, reinterpret_cast<u64>(&Aggregate), prologue, 2);
    install(native_buffer_event, 0xa1c3c0, reinterpret_cast<u64>(&Buffer), prologue, 4);
    install(native_fire_start, 0xa17f00, reinterpret_cast<u64>(&Start), start_bytes, 8);
    install(native_timeline_tick, 0xa28dc0, reinterpret_cast<u64>(&Tick), prologue, 16);
    install(native_timeline_item, 0xa28850, reinterpret_cast<u64>(&Item), item_bytes, 32);
    install(native_awake, 0x20b8f20, reinterpret_cast<u64>(&Awake), awake_bytes, 64);
    install(native_active_switch, 0xa1e2c0, reinterpret_cast<u64>(&Switch), switch_bytes, 128);
    install(native_graph_event, 0xa1fe10, reinterpret_cast<u64>(&Graph), graph_bytes, 256);
    install(native_clear, 0xa21c90, reinterpret_cast<u64>(&Clear), clear_bytes, 512);
    LOG_INFO(Input,
             "[DMC LIGHT] Passive timing hooks={:#x}/0x3ff; no compatibility or heavy probes",
             hooks);
}

void Begin() {
    ready_generation.store(0, std::memory_order_release);
    records.store(0, std::memory_order_relaxed);
    shared_world.store(0, std::memory_order_relaxed);
    shared_input.store(0, std::memory_order_relaxed);
    shared_aggregator.store(0, std::memory_order_relaxed);
    timeline_count.store(0, std::memory_order_relaxed);
    for (auto& timeline : timelines) {
        timeline.store(0, std::memory_order_relaxed);
    }
    PadTrace::Record("DMC_LIGHT_CAPTURE",
                     {1, hooks, record_budget, static_cast<s64>(guest_base), 16, 128});
    ready_generation.store(capture_generation.load(std::memory_order_relaxed),
                           std::memory_order_release);
}
} // namespace LightTrace
#endif
