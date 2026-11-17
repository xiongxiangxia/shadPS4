// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#ifdef ENABLE_DMC_EVADE_TRACE
namespace EvadeTrace {
using TokenOperation = void(PS4_SYSV_ABI*)(void*, u64, u64);
using BufferCallback = void(PS4_SYSV_ABI*)(void*, void*, void*);

GraphEvent graph_event = nullptr;
TokenOperation create_token = nullptr;
TokenOperation delete_token = nullptr;
BufferCallback set_buffer = nullptr;
bool installed = false;

struct AngelEvadeBuffer {
    u64 manager = 0;
    u64 token = 0;
    u64 owner = 0;
    u64 graph = 0;
};

thread_local AngelEvadeBuffer angel_buffer;
thread_local AngelEvadeBuffer* creating_buffer = nullptr;

struct EvadePress {
    u64 aggregator;
    u64 event;
    u8 mode;
};

thread_local EvadePress* evade_press = nullptr;
thread_local u64 pending_gate_event = 0;
thread_local u8* pending_gate_flag = nullptr;

void RestoreBufferingGate() {
    if (pending_gate_flag != nullptr) {
        *pending_gate_flag |= 1;
        pending_gate_flag = nullptr;
    }
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

const char* BlockedAerialEvade(const EvadePress& press) {
    if (angel_buffer.token == 0 || angel_buffer.manager != CompatWord(compat.pawn + 0x9c0) ||
        TokenCount(angel_buffer.manager, angel_buffer.token) != 1) {
        return "no sole angel buffer";
    }
    if (LiveDword(press.aggregator + 0x80) != 0) {
        return "input already buffered";
    }
    u8 mode = 255;
    if (compat.mode_offset == 0 ||
        !CompatRead(compat.pawn + compat.mode_offset, &mode, sizeof(mode)) || mode != press.mode) {
        return "play mode";
    }
    u32 ground_mask = 0;
    const u32 ground_offset = CompatOffset(compat.pawn, "m_bOnGround", &ground_mask);
    u32 ground_flags = 0;
    if (ground_offset == 0 || ground_mask == 0 || (ground_mask & (ground_mask - 1)) != 0 ||
        !CompatRead(compat.pawn + ground_offset, &ground_flags, sizeof(ground_flags))) {
        return "ground state unavailable";
    }
    return (ground_flags & ground_mask) != 0 ? "grounded" : nullptr;
}

u32 PS4_SYSV_ABI Graph(void* graph, void* event) {
    const auto addr = reinterpret_cast<u64>(graph);
    const auto ev = reinterpret_cast<u64>(event);
    if (evade_press == nullptr || evade_press->event != ev || addr != angel_buffer.graph ||
        pending_gate_event != 0 ||
        reinterpret_cast<u64>(__builtin_return_address(0)) - guest_base != 0xa1fb69 ||
        LiveWord(addr + 0x78) != evade_press->aggregator) {
        return graph_event(graph, event);
    }
    if ((LiveDword(addr + 0x90) & 1) == 0 || LiveWord(addr + 0x88) != 0) {
        return graph_event(graph, event);
    }
    if (BlockedAerialEvade(*evade_press) != nullptr) {
        return graph_event(graph, event);
    }

    pending_gate_event = ev;
    pending_gate_flag = reinterpret_cast<u8*>(addr + 0x90);
    *pending_gate_flag &= 0xfe;
    const u32 accepted = graph_event(graph, event);
    RestoreBufferingGate();
    pending_gate_event = 0;
    return accepted;
}

bool DispatchRootCondition(void* object, void* function, void* parameters, void* result,
                           u64 caller) {
    if (pending_gate_event == 0 || parameters == nullptr ||
        (caller != guest_base + 0xa1fee9 && caller != guest_base + 0xa2090c) ||
        LiveWord(reinterpret_cast<u64>(parameters)) != pending_gate_event) {
        return false;
    }
    if (caller == guest_base + 0xa1fee9) {
        RestoreBufferingGate();
    }
    const auto address = reinterpret_cast<u64>(object);
    if (!CompatName(LiveWord(address + 0x40), "EvadeGraphRoot") ||
        !CompatClass(address, "DVActionGraph_ConditionDef_Token") ||
        !CompatNameIndex(static_cast<u32>(LiveWord(address + 0x60)), "EvadesDenied")) {
        return false;
    }
    const u32 offset = CompatOffset(address, "m_AdditionalTokenChecks");
    if (offset == 0 || LiveDword(address + offset + 8) != 1) {
        return false;
    }
    const u64 checks = LiveWord(address + offset);
    u64 token = 0;
    u32 has_token = 1;
    if (checks == 0 || !CompatRead(checks, &token, sizeof(token)) || token != angel_buffer.token ||
        !CompatRead(checks + 8, &has_token, sizeof(has_token)) || has_token != 0) {
        return false;
    }

    // The singleton array only requires EvadesBuffer to be absent. Preserve the native
    // EvadesDenied check and leave the live token and its callbacks in place.
    const u32 empty = 0;
    const u32 count = 1;
    auto* count_field = reinterpret_cast<void*>(address + offset + 8);
    std::memcpy(count_field, &empty, sizeof(empty));
    native_condition_dispatch(object, function, parameters, result);
    std::memcpy(count_field, &count, sizeof(count));
    return true;
}

void BeforeSelect(void* event, u64 caller) {
    if (pending_gate_event != 0 && reinterpret_cast<u64>(event) == pending_gate_event &&
        caller == guest_base + 0xa1ffcb) {
        RestoreBufferingGate();
    }
}

bool IsAngelEvadeOwner(u64 owner) {
    const auto name = static_cast<u32>(owner);
    return CompatNameIndex(name, "Action_AngelEvade_Ground") ||
           CompatNameIndex(name, "Action_AngelEvade_Ground_3rd") ||
           CompatNameIndex(name, "Action_AngelEvade_Air") ||
           CompatNameIndex(name, "Action_AngelEvade_Air_3rd");
}

void PS4_SYSV_ABI SetBuffer(void* graph, void* frame, void* result) {
    set_buffer(graph, frame, result);
    if (creating_buffer != nullptr && compat.pawn != 0) {
        const auto address = reinterpret_cast<u64>(graph);
        if (LiveWord(address + 0x78) == CompatWord(compat.pawn + 0x930)) {
            creating_buffer->graph = address;
        }
    }
}

void PS4_SYSV_ABI Create(void* manager, u64 token, u64 owner) {
    const auto address = reinterpret_cast<u64>(manager);
    if (!installed || compat.pawn == 0 ||
        !CompatNameIndex(static_cast<u32>(token), "EvadesBuffer") ||
        address != CompatWord(compat.pawn + 0x9c0) || !IsAngelEvadeOwner(owner)) {
        return create_token(manager, token, owner);
    }
    AngelEvadeBuffer buffer{address, token, owner};
    auto* previous = creating_buffer;
    creating_buffer = &buffer;
    create_token(manager, token, owner);
    creating_buffer = previous;
    if (buffer.graph != 0) {
        angel_buffer = buffer;
    }
}

void PS4_SYSV_ABI Delete(void* manager, u64 token, u64 owner) {
    const auto address = reinterpret_cast<u64>(manager);
    if (address == angel_buffer.manager && token == angel_buffer.token &&
        owner == angel_buffer.owner) {
        angel_buffer = {};
    }
    delete_token(manager, token, owner);
}

u32 Aggregate(void* object, void* input_event, u32 input, u32 (*original)(void*, void*, u32)) {
    const auto aggregator = reinterpret_cast<u64>(object);
    const auto event = reinterpret_cast<u64>(input_event);
    u8 mode = 255;
    if (!installed || input == 0 || event == 0 || compat.pawn == 0 ||
        *reinterpret_cast<const u8*>(event + 0x78) != 0 ||
        !CompatName(LiveWord(event + 0x60), "DVButton_Evade") || !IsInput(event) ||
        aggregator != CompatWord(compat.pawn + 0x930) ||
        !CompatRead(event + 0x90, &mode, sizeof(mode)) || mode > 1) {
        return original(object, input_event, input);
    }
    EvadePress press{aggregator, event, mode};
    auto* previous = evade_press;
    evade_press = &press;
    const u32 accepted = original(object, input_event, input);
    evade_press = previous;
    return accepted;
}

void Install(u64 base);
} // namespace EvadeTrace
#endif
