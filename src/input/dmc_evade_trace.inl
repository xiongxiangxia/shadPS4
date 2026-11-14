// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#ifdef ENABLE_DMC_EVADE_TRACE
namespace EvadeTrace {
using Clock = std::chrono::steady_clock;
using TokenOperation = void(PS4_SYSV_ABI*)(void*, u64, u64);
using ActionStart = void(PS4_SYSV_ABI*)(void*);

GraphEvent graph_event = nullptr;
TokenOperation create_token = nullptr;
TokenOperation delete_token = nullptr;
ActionStart action_start = nullptr;
bool installed = false;

struct Capture {
    Clock::time_point start{};
    u64 aggregator = 0;
    u64 event = 0;
    u64 sequence = 0;
    u32 lines = 0;
};
thread_local Capture capture;

bool Active() {
    return installed && capture.start != Clock::time_point{} && capture.lines < 384 &&
           Clock::now() - capture.start < std::chrono::seconds{5};
}

template <typename... Args>
void Record(fmt::format_string<Args...> format, Args&&... args) {
    if (capture.lines >= 384) {
        return;
    }
    ++capture.lines;
    const auto us =
        std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - capture.start).count();
    LOG_INFO(Input, "[DMC EVADE] +{}us seq={} {}", us, capture.sequence,
             fmt::format(format, std::forward<Args>(args)...));
}

std::array<char, 96> Name(u64 value) {
    std::array<char, 96> text{};
    const u32 index = static_cast<u32>(value);
    const u64 table = CompatWord(guest_base + 0x3984330);
    u32 count = 0;
    if (index == 0 || table == 0 || !CompatRead(guest_base + 0x3984338, &count, sizeof(count)) ||
        index >= count || count > 2000000) {
        return text;
    }
    const u64 entry = CompatWord(table + static_cast<u64>(index) * 8);
    u32 flags = 0;
    if (entry == 0 || !CompatRead(entry, &flags, sizeof(flags))) {
        return text;
    }
    if ((flags & 1) == 0) {
        CompatRead(entry + 0x10, text.data(), text.size() - 1);
    } else {
        std::array<u16, 95> wide{};
        if (CompatRead(entry + 0x10, wide.data(), sizeof(wide))) {
            for (std::size_t i = 0; i < wide.size() && wide[i] != 0; ++i) {
                text[i] = wide[i] < 128 ? static_cast<char>(wide[i]) : '?';
            }
        }
    }
    return text;
}

auto ObjectName(u64 object) {
    return Name(object != 0 ? CompatWord(object + 0x48) : 0);
}

bool TrackedToken(u64 token) {
    const u32 index = static_cast<u32>(token);
    return CompatNameIndex(index, "EvadesBuffer") || CompatNameIndex(index, "EvadesDenied") ||
           CompatNameIndex(index, "JumpsDenied");
}

s32 TokenCount(u64 manager, u64 token) {
    std::array<u64, 10> header{};
    if (manager == 0 || !CompatRead(manager + 0x60, header.data(), sizeof(header))) {
        return -1;
    }
    const u32 buckets = static_cast<u32>(header[9]);
    if (buckets == 0) {
        return 0;
    }
    if (buckets > 65536 || (buckets & (buckets - 1)) != 0 || (header[8] == 0 && buckets > 2)) {
        return -1;
    }
    const u64 hash = header[8] != 0 ? header[8] : manager + 0x98;
    s32 index = -1;
    if (!CompatRead(hash + (static_cast<u32>(token) & (buckets - 1)) * 4, &index, sizeof(index))) {
        return -1;
    }
    for (u32 steps = 0; index != -1 && steps < 16; ++steps) {
        std::array<u64, 10> entry{};
        if (index < 0 || index >= 4096 || header[0] == 0 ||
            !CompatRead(header[0] + static_cast<u64>(index) * 0x50, entry.data(), sizeof(entry))) {
            return -1;
        }
        if (entry[0] == token) {
            return static_cast<s32>(entry[2]);
        }
        index = static_cast<s32>(entry[9]);
    }
    return index == -1 ? 0 : -1;
}

const char* Button(u64 event) {
    const u64 descriptor = CompatWord(event + 0x60);
    if (CompatName(descriptor, "DVButton_Evade")) {
        return "Evade";
    }
    if (CompatName(descriptor, "DVButton_Jump")) {
        return "Jump";
    }
    return nullptr;
}

bool IsInput(u64 event) {
    const u64 input_class = CompatWord(guest_base + 0x3b193f8);
    for (u64 type = CompatWord(event + 0x50), depth = 0;
         input_class != 0 && type != 0 && depth < 16; ++depth) {
        if (type == input_class) {
            return true;
        }
        type = CompatWord(type + 0x68);
    }
    return false;
}

void Input(u64 aggregator, u64 event, u32 input) {
    if (!installed || input == 0 || event == 0 || compat.pawn == 0 ||
        aggregator != CompatWord(compat.pawn + 0x930)) {
        return;
    }
    u8 type = 255;
    if (!CompatRead(event + 0x78, &type, sizeof(type)) || type != 0 || !IsInput(event)) {
        return;
    }
    const char* button = Button(event);
    if (button == nullptr) {
        return;
    }
    if (!Active()) {
        if (std::strcmp(button, "Evade") != 0 || capture.lines >= 384) {
            return;
        }
        capture.start = Clock::now();
    }
    capture.aggregator = aggregator;
    ++capture.sequence;
    u8 mode = 255;
    if (compat.mode_offset != 0) {
        CompatRead(compat.pawn + compat.mode_offset, &mode, sizeof(mode));
    }
    u8 event_mode = 255;
    CompatRead(event + 0x90, &event_mode, sizeof(event_mode));
    Record("input button={} event={:#x} pawn_mode={} event_mode={}", button, event, mode,
           event_mode);
}

u32 PS4_SYSV_ABI Graph(void* graph, void* event) {
    const auto addr = reinterpret_cast<u64>(graph);
    const auto ev = reinterpret_cast<u64>(event);
    if (!Active() || ev == 0 || CompatWord(addr + 0x78) != capture.aggregator) {
        return graph_event(graph, event);
    }
    const char* button = Button(ev);
    if (button == nullptr || !IsInput(ev)) {
        return graph_event(graph, event);
    }
    u8 type = 255;
    if (!CompatRead(ev + 0x78, &type, sizeof(type)) || type != 0) {
        return graph_event(graph, event);
    }
    const auto caller = reinterpret_cast<u64>(__builtin_return_address(0)) - guest_base;
    const auto before = CompatWord(addr + 0x88);
    const auto previous_event = capture.event;
    capture.event = ev;
    const auto root = ObjectName(CompatWord(addr + 0x60));
    Record("graph_enter button={} event={:#x} graph={:#x} root={} caller={:#x} node={:#x}", button,
           ev, addr, root.data(), caller, before);
    const u32 accepted = graph_event(graph, event);
    Record("graph_exit event={:#x} accepted={} node={:#x} buffered={}", ev, accepted,
           CompatWord(addr + 0x88), LiveDword(capture.aggregator + 0x80));
    capture.event = previous_event;
    return accepted;
}

void Condition(u64 condition, void* parameters, u64 caller) {
    const auto params = reinterpret_cast<u64>(parameters);
    if (capture.event == 0 || !Active() || params == 0 ||
        (caller != guest_base + 0xa1fee9 && caller != guest_base + 0xa2090c &&
         caller != guest_base + 0xa235b9) ||
        CompatWord(params) != capture.event) {
        return;
    }
    const u64 type = CompatWord(condition + 0x50);
    const auto name = ObjectName(type);
    const u64 token = CompatClass(condition, "DVActionGraph_ConditionDef_Token")
                          ? CompatWord(condition + 0x60)
                          : 0;
    const auto token_name = Name(token);
    const auto owner = ObjectName(CompatWord(condition + 0x40));
    const u64 pawn = CompatWord(params + 16);
    const s32 count = token != 0 && pawn != 0 ? TokenCount(CompatWord(pawn + 0x9c0), token) : -1;
    Record("condition event={:#x} class={} owner={} token={} count={} pass={}", capture.event,
           name.data(), owner.data(), token_name.data(), count, LiveDword(params + 24));
}

void Match(u64 definition, u64 event, u32 matched) {
    if (capture.event == 0 || event != capture.event || !Active() || definition == 0 ||
        CompatWord(definition + 0x68) != CompatWord(event + 0x60)) {
        return;
    }
    const u64 target = CompatWord(definition + 0x60);
    const auto owner = ObjectName(CompatWord(definition + 0x40));
    const auto name = ObjectName(target);
    const u64 id = CompatWord(definition + 0x48);
    u8 mode = 255;
    CompatRead(event + 0x90, &mode, sizeof(mode));
    Record("link event={:#x} owner={} id={} target={} match={} mode={} combo={}", event,
           owner.data(), id >> 32, name.data(), matched, mode, LiveDword(event + 0x68));
}

const char* BufferButton(u64 buffer, u64 event) {
    if (!Active() || event == 0 || buffer != capture.aggregator + 0x78) {
        return nullptr;
    }
    u8 type = 255;
    if (CompatRead(event + 0x78, &type, sizeof(type)) && type == 0 && IsInput(event)) {
        return Button(event);
    }
    return nullptr;
}

void Token(const char* operation, u64 manager, u64 token, u64 owner, u64 caller) {
    if (!Active() || compat.pawn == 0 || manager != CompatWord(compat.pawn + 0x9c0) ||
        !TrackedToken(token)) {
        return;
    }
    const auto name = Name(token);
    const auto owner_name = Name(owner);
    Record("token {} name={} owner={} count_after={} caller={:#x}", operation, name.data(),
           owner_name.data(), TokenCount(manager, token), caller - guest_base);
}

void PS4_SYSV_ABI Create(void* manager, u64 token, u64 owner) {
    const u64 caller = reinterpret_cast<u64>(__builtin_return_address(0));
    create_token(manager, token, owner);
    Token("create", reinterpret_cast<u64>(manager), token, owner, caller);
}

void PS4_SYSV_ABI Delete(void* manager, u64 token, u64 owner) {
    const u64 caller = reinterpret_cast<u64>(__builtin_return_address(0));
    delete_token(manager, token, owner);
    Token("delete", reinterpret_cast<u64>(manager), token, owner, caller);
}

void PS4_SYSV_ABI Start(void* action) {
    if (Active()) {
        const auto address = reinterpret_cast<u64>(action);
        const u64 node = CompatWord(address + 0x68);
        const u64 graph = node != 0 ? CompatWord(node + 0x70) : 0;
        if (graph != 0 && CompatWord(graph + 0x78) == capture.aggregator &&
            (capture.event != 0 || CompatClass(address, "DVAction_Dante_AerialEvade") ||
             CompatClass(address, "DVAction_Dante_AngelEvade"))) {
            const auto type = ObjectName(CompatWord(address + 0x50));
            const auto root = ObjectName(CompatWord(graph + 0x60));
            Record("action_start graph={} class={} action={:#x} node={:#x}", root.data(),
                   type.data(), address, node);
        }
    }
    action_start(action);
}

void Install(u64 base);
} // namespace EvadeTrace
#endif
