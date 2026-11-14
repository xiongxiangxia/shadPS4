// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#ifdef ENABLE_DMC_EVADE_TRACE
namespace EvadeTrace {
using TokenOperation = void(PS4_SYSV_ABI*)(void*, u64, u64);

GraphEvent graph_event = nullptr;
TokenOperation create_token = nullptr;
TokenOperation delete_token = nullptr;
bool installed = false;

struct AngelEvadeBuffer {
    u64 manager = 0;
    u64 token = 0;
    u64 owner = 0;
};

thread_local AngelEvadeBuffer angel_buffer;
thread_local u64 pending_gate_event = 0;
thread_local u8* pending_gate_flag = nullptr;
thread_local u32 diagnostic_lines = 0;

void RestoreBufferingGate() {
    if (pending_gate_flag != nullptr) {
        *pending_gate_flag |= 1;
        pending_gate_flag = nullptr;
    }
}

template <typename... Args>
void Record(fmt::format_string<Args...> format, Args&&... args) {
    if (diagnostic_lines < 48) {
        ++diagnostic_lines;
        LOG_INFO(Input, "[DMC EVADE] {}", fmt::format(format, std::forward<Args>(args)...));
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

bool IsEvadeRoot(u64 graph) {
    const u64 conditions = CompatWord(graph + 0x68);
    if (conditions == 0 || LiveDword(conditions + 0x70) == 0) {
        return false;
    }
    const u64 list = CompatWord(conditions + 0x68);
    const u64 first = list != 0 ? CompatWord(list) : 0;
    return first != 0 && CompatName(CompatWord(first + 0x40), "EvadeGraphRoot") &&
           CompatNameIndex(static_cast<u32>(CompatWord(first + 0x60)), "EvadesDenied");
}

const char* BlockedAerialEvade(u64 aggregator, u64 event) {
    if (angel_buffer.token == 0 || angel_buffer.manager != CompatWord(compat.pawn + 0x9c0) ||
        TokenCount(angel_buffer.manager, angel_buffer.token) != 1) {
        return "no sole angel buffer";
    }
    if (LiveDword(aggregator + 0x80) != 0) {
        return "input already buffered";
    }
    u8 mode = 255;
    u8 event_mode = 255;
    if (compat.mode_offset == 0 ||
        !CompatRead(compat.pawn + compat.mode_offset, &mode, sizeof(mode)) ||
        !CompatRead(event + 0x90, &event_mode, sizeof(event_mode)) || mode > 1 ||
        event_mode != mode) {
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
    const auto caller = reinterpret_cast<u64>(__builtin_return_address(0)) - guest_base;
    const auto addr = reinterpret_cast<u64>(graph);
    const auto ev = reinterpret_cast<u64>(event);
    if (!installed || caller != 0xa1fb69 || ev == 0 || compat.pawn == 0 ||
        pending_gate_flag != nullptr || (LiveDword(addr + 0x90) & 1) == 0 ||
        CompatWord(addr + 0x88) != 0) {
        return graph_event(graph, event);
    }
    const u64 aggregator = CompatWord(addr + 0x78);
    u8 type = 255;
    if (aggregator == 0 || aggregator != CompatWord(compat.pawn + 0x930) ||
        !CompatName(CompatWord(ev + 0x60), "DVButton_Evade") ||
        !CompatRead(ev + 0x78, &type, sizeof(type)) || type != 0 || !IsInput(ev) ||
        !IsEvadeRoot(addr)) {
        return graph_event(graph, event);
    }
    if (const char* blocked = BlockedAerialEvade(aggregator, ev)) {
        Record("early_aerial_evade skipped: {}", blocked);
        return graph_event(graph, event);
    }

    pending_gate_event = ev;
    pending_gate_flag = reinterpret_cast<u8*>(addr + 0x90);
    *pending_gate_flag &= 0xfe;
    const auto start = std::chrono::steady_clock::now();
    const u32 accepted = graph_event(graph, event);
    const bool checked_conditions = pending_gate_flag == nullptr;
    RestoreBufferingGate();
    pending_gate_event = 0;
    if (diagnostic_lines < 48) {
        const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - start)
                            .count();
        Record("early_aerial_evade accepted={} native_root_checked={} elapsed_us={}", accepted,
               checked_conditions, us);
    }
    return accepted;
}

void PS4_SYSV_ABI Create(void* manager, u64 token, u64 owner) {
    create_token(manager, token, owner);
    const auto address = reinterpret_cast<u64>(manager);
    if (!installed || compat.pawn == 0 ||
        !CompatNameIndex(static_cast<u32>(token), "EvadesBuffer") ||
        address != CompatWord(compat.pawn + 0x9c0)) {
        return;
    }
    const auto owner_name = static_cast<u32>(owner);
    if (CompatNameIndex(owner_name, "Action_AngelEvade_Ground") ||
        CompatNameIndex(owner_name, "Action_AngelEvade_Ground_3rd") ||
        CompatNameIndex(owner_name, "Action_AngelEvade_Air") ||
        CompatNameIndex(owner_name, "Action_AngelEvade_Air_3rd")) {
        angel_buffer = {address, token, owner};
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

void Install(u64 base);
} // namespace EvadeTrace
#endif
