// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "input/dmc_play.h"

#if defined(ENABLE_DMC_PLAY) && defined(_WIN64)
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <unordered_map>
#include <windows.h>
#include "common/elf_info.h"
#include "common/logging/log.h"
#include "common/memory_patcher.h"
#include "common/types.h"

namespace Input::DmcPlay {
namespace {
using Native = void(PS4_SYSV_ABI*)(void*, void*, void*);
using AggregateEvent = u32(PS4_SYSV_ABI*)(void*, void*, u32);
using BufferEvent = u32(PS4_SYSV_ABI*)(void*, void*, void*, void*, void*);
using GraphEvent = u32(PS4_SYSV_ABI*)(void*, void*);
using SelectLink = void*(PS4_SYSV_ABI*)(void*, void*);
using ConditionDispatch = void(PS4_SYSV_ABI*)(void*, void*, void*, void*);
Native compat_bool = nullptr;
std::atomic<Native> original;
std::atomic<bool> compat_enabled{false};
std::atomic<u64> compat_consumed_property{0};
std::atomic<u64> compat_player_tick{0};
std::atomic<u64> compat_shot_site{0};
AggregateEvent native_aggregate_event = nullptr;
BufferEvent native_buffer_event = nullptr;
GraphEvent native_match_event = nullptr;
SelectLink native_select_link = nullptr;
ConditionDispatch native_condition_dispatch = nullptr;
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

u64 LiveWord(u64 address) {
    u64 value = 0;
    std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(value));
    return value;
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

enum class CompatGun { None, Pistols, Shotgun };

struct CompatState {
    u64 controller = 0;
    u64 pawn = 0;
    u64 input = 0;
    u64 candidate = 0;
    u64 aggregator = 0;
    u64 gun_descriptor = 0;
    u64 generation = 0;
    u32 charge_offset = 0;
    u32 pawn_offset = 0;
    u32 input_offset = 0;
    u32 health_offset = 0;
    u32 mode_offset = 0;
    u32 weapon_offset = 0;
    u64 controller_class = 0;
    std::chrono::steady_clock::time_point last_retry{};
    CompatGun gun = CompatGun::None;
    bool selecting = false;
    bool mode_held = false;
};

thread_local CompatState compat;

struct CompatQueueScope;
thread_local CompatQueueScope* compat_queue = nullptr;

struct CompatQueueScope {
    CompatQueueScope* previous = compat_queue;
    u64 event = 0;
    u64 descriptor = 0;
    u64 aggregator = 0;
    u64 gun_descriptor = 0;
    u64 generation = 0;
    u8 type = 255;
    bool attack = false;

    CompatQueueScope() {
        compat_queue = nullptr;
    }
    ~CompatQueueScope() {
        compat_queue = previous;
    }
};

bool CompatRead(u64 address, void* output, std::size_t size);
u64 CompatWord(u64 address);

bool CompatCheckedCopy(u64 address, void* output, std::size_t size) {
    if (address == 0 || size > std::numeric_limits<u64>::max() - address) {
        return false;
    }
    SIZE_T copied = 0;
    if (ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address), output, size,
                          &copied) != 0 &&
        copied == size) {
        return true;
    }
    std::memset(output, 0, size);
    return false;
}

struct CompatLookup {
    u64 value = 0;
    const char* name = nullptr;
    bool operator==(const CompatLookup&) const = default;
};

struct CompatLookupHash {
    std::size_t operator()(const CompatLookup& key) const {
        return std::hash<u64>{}(key.value) ^ (reinterpret_cast<std::size_t>(key.name) >> 3);
    }
};

bool CompatNameIndex(u32 index, const char* name) {
    thread_local std::unordered_map<CompatLookup, bool, CompatLookupHash> names;
    const CompatLookup key{index, name};
    if (const auto found = names.find(key); found != names.end()) {
        return found->second;
    }
    const bool matches = NameIs(static_cast<u32>(key.value), name);
    names.emplace(key, matches);
    return matches;
}

bool CompatName(u64 object, const char* name) {
    return object != 0 && CompatNameIndex(static_cast<u32>(CompatWord(object + 0x48)), name);
}

bool CompatFunction(u64 function, const char* name) {
    return function != 0 && CompatNameIndex(static_cast<u32>(LiveWord(function + 0x48)), name);
}

bool CompatClass(u64 object, const char* name) {
    if (object == 0) {
        return false;
    }
    thread_local std::unordered_map<CompatLookup, bool, CompatLookupHash> classes;
    const CompatLookup key{CompatWord(object + 0x50), name};
    if (const auto found = classes.find(key); found != classes.end()) {
        return found->second;
    }
    for (u64 type = key.value, depth = 0; type != 0 && depth < 32;
         type = CompatWord(type + 0x68), ++depth) {
        if (CompatName(type, name)) {
            classes.emplace(key, true);
            return true;
        }
    }
    classes.emplace(key, false);
    return false;
}

struct CompatMetadataReader {
    struct Region {
        u64 begin = 0;
        u64 end = 0;
    };
    std::array<Region, 64> regions{};
    std::size_t next = 0;
    bool metadata_scan = false;

    bool Copy(u64 address, void* output, std::size_t size) {
        if (address == 0 || size > std::numeric_limits<u64>::max() - address) {
            return false;
        }
        if (!metadata_scan) {
            const bool copied = CompatCheckedCopy(address, output, size);
            return copied;
        }
        const u64 end = address + size;
        for (u64 cursor = address; cursor < end;) {
            u64 region_end = 0;
            for (const auto& region : regions) {
                if (cursor >= region.begin && cursor < region.end) {
                    region_end = region.end;
                    break;
                }
            }
            if (region_end == 0) {
                MEMORY_BASIC_INFORMATION info{};
                const auto queried =
                    VirtualQuery(reinterpret_cast<void*>(cursor), &info, sizeof(info));
                if (queried == 0 || info.State != MEM_COMMIT ||
                    (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0 ||
                    (info.Protect &
                     (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                      PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) == 0) {
                    return false;
                }
                region_end = reinterpret_cast<u64>(info.BaseAddress) + info.RegionSize;
                if (region_end <= cursor) {
                    return false;
                }
                regions[next++ % regions.size()] = {reinterpret_cast<u64>(info.BaseAddress),
                                                    region_end};
            }
            cursor = region_end;
        }
        std::memcpy(output, reinterpret_cast<const void*>(address), size);
        return true;
    }

    u64 Pointer(u64 address) {
        u64 value = 0;
        Copy(address, &value, sizeof(value));
        return value;
    }

    bool Named(u64 field, u64 table, u32 count, const char* name) {
        const u32 index = static_cast<u32>(Pointer(field + 0x48));
        if (index == 0 || index >= count) {
            return false;
        }
        const u64 entry = Pointer(table + static_cast<u64>(index) * 8);
        u32 flags = 0;
        if (entry == 0 || !Copy(entry, &flags, sizeof(flags))) {
            return false;
        }
        const bool wide = (flags & 1) != 0;
        const auto length = std::strlen(name);
        const auto stride = wide ? 2 : 1;
        std::array<u8, 64> bytes{};
        const auto size = (length + 1) * stride;
        if (size > bytes.size() || !Copy(entry + 0x10, bytes.data(), size)) {
            return false;
        }
        for (std::size_t i = 0; i <= length; ++i) {
            if (bytes[i * stride] != static_cast<u8>(name[i]) ||
                (wide && bytes[i * stride + 1] != 0)) {
                return false;
            }
        }
        return true;
    }
};

bool CompatRead(u64 address, void* output, std::size_t size) {
    return CompatCheckedCopy(address, output, size);
}

u64 CompatWord(u64 address) {
    u64 value = 0;
    CompatRead(address, &value, sizeof(value));
    return value;
}

u32 CompatOffset(u64 object, const char* name) {
    if (object == 0) {
        return 0;
    }
    thread_local std::unordered_map<CompatLookup, u32, CompatLookupHash> offsets;
    const CompatLookup key{CompatWord(object + 0x50), name};
    if (const auto found = offsets.find(key); found != offsets.end()) {
        return found->second;
    }
    CompatMetadataReader metadata;
    metadata.metadata_scan = true;
    const u64 table = metadata.Pointer(guest_base + 0x3984330);
    u32 names = 0;
    if (table == 0 || !metadata.Copy(guest_base + 0x3984338, &names, sizeof(names)) || names == 0 ||
        names > 2000000) {
        return 0;
    }
    u32 visited = 0;
    for (u64 type = key.value, depth = 0; type != 0 && depth < 32;
         type = metadata.Pointer(type + 0x68), ++depth) {
        for (u64 field = metadata.Pointer(type + 0x70); field != 0 && visited < 4096;
             field = metadata.Pointer(field + 0x60), ++visited) {
            if (metadata.Named(field, table, names, name)) {
                u32 offset = 0;
                if (metadata.Copy(field + 0x90, &offset, sizeof(offset)) && offset >= 0x58 &&
                    offset < 0x10000) {
                    offsets.emplace(key, offset);
                    return offset;
                }
                offsets.emplace(key, 0);
                return 0;
            }
        }
    }
    offsets.emplace(key, 0);
    return 0;
}

u64 CompatObject(u64 object, const char* name) {
    const u32 offset = CompatOffset(object, name);
    return offset != 0 ? CompatWord(object + offset) : 0;
}

bool CompatHeld() {
    u32 buttons = 0;
    return compat.input != 0 && CompatRead(compat.input + 0x220, &buttons, sizeof(buttons)) &&
           (buttons & (1u << 6)) != 0;
}

void CompatController(u64 object) {
    const u64 type = CompatWord(object + 0x50);
    if (compat.controller_class != type) {
        compat.controller_class = type;
        compat.pawn_offset = CompatOffset(object, "m_NTPawn");
        compat.input_offset = CompatOffset(object, "m_NTInput");
    }
    const u64 pawn = compat.pawn_offset != 0 ? CompatWord(object + compat.pawn_offset) : 0;
    const u64 input = compat.input_offset != 0 ? CompatWord(object + compat.input_offset) : 0;
    if (compat.controller != object || compat.pawn != pawn || compat.input != input) {
        compat_shot_site.store(0, std::memory_order_relaxed);
        ++compat.generation;
        compat.gun = CompatGun::None;
        compat.aggregator = 0;
        compat.mode_held = false;

        compat.charge_offset = CompatOffset(pawn, "m_fcpGunCharge");
        compat.health_offset = CompatOffset(pawn, "m_Health");
        compat.mode_offset = CompatOffset(pawn, "m_Playmode");
        compat.weapon_offset = CompatOffset(pawn, "m_ActiveRangedWeapon");
    }
    compat.controller = object;
    compat.pawn = pawn;
    compat.input = input;
    if (!CompatHeld()) {
        compat.gun = CompatGun::None;
        compat.aggregator = 0;
        compat.mode_held = false;
    }
}

void CompatModeIntent() {
    if (compat.gun != CompatGun::None || compat.pawn == 0 || compat.mode_offset == 0 ||
        compat.weapon_offset == 0 || !CompatHeld()) {
        return;
    }
    u8 mode = 0xff;
    if (!CompatRead(compat.pawn + compat.mode_offset, &mode, sizeof(mode))) {
        return;
    }
    if (mode == 1 || mode == 2) {
        compat.mode_held = true;
        return;
    }
    if (mode != 0) {
        return;
    }
    compat.mode_held = false;
    u8 weapon = 0xff;
    if (!CompatRead(compat.pawn + compat.weapon_offset, &weapon, sizeof(weapon))) {
        return;
    }
    const auto gun = weapon == 0   ? CompatGun::Pistols
                     : weapon == 1 ? CompatGun::Shotgun
                                   : CompatGun::None;
    if (gun == CompatGun::None) {
        return;
    }
    float health = 0.0f;
    if (compat.health_offset == 0 ||
        !CompatRead(compat.pawn + compat.health_offset, &health, sizeof(health)) ||
        !(health > 0.0f)) {
        return;
    }
    compat.gun = gun;
    ++compat.generation;
}

bool CompatPending() {
    if (compat.gun == CompatGun::None) {
        return false;
    }
    u32 charge = 0;
    if (compat.pawn == 0 || compat.charge_offset == 0 ||
        !CompatRead(compat.pawn + compat.charge_offset, &charge, sizeof(charge))) {
        return false;
    }
    if (charge != 0) {

        return false;
    }
    if (!CompatHeld()) {
        compat.gun = CompatGun::None;
        return false;
    }
    float health = 0.0f;
    if (compat.health_offset != 0 &&
        CompatRead(compat.pawn + compat.health_offset, &health, sizeof(health)) && health <= 0.0f) {
        compat.gun = CompatGun::None;
        return false;
    }
    return true;
}

CompatGun CompatTarget(u64 definition) {
    if (definition == 0) {
        return CompatGun::None;
    }
    struct Target {
        u64 name = 0;
        u64 type = 0;
        u64 generation = 0;
        CompatGun gun = CompatGun::None;
    };
    thread_local std::unordered_map<u64, Target> targets;
    const u64 name = LiveWord(definition + 0x48);
    const u64 type = LiveWord(definition + 0x50);
    if (const auto found = targets.find(definition);
        found != targets.end() && found->second.name == name && found->second.type == type &&
        found->second.generation == compat.generation) {
        return found->second.gun;
    }
    const u64 target = CompatObject(definition, "TargetNode");
    auto gun = CompatGun::None;
    if (CompatName(target, "Node_Dante_Shotgun_Bomb_Charge")) {
        gun = CompatGun::Shotgun;
    } else if (CompatName(target, "Node_Dante_EnI_Ricoshot_Charge")) {
        gun = CompatGun::Pistols;
    }
    targets.insert_or_assign(definition, Target{name, type, compat.generation, gun});
    return gun;
}

bool CompatBufferedGun(u32& count, bool& held_gun) {
    if (compat.aggregator == 0 || compat.gun_descriptor == 0 ||
        !CompatRead(compat.aggregator + 0x80, &count, sizeof(count)) || count > 64) {
        return false;
    }
    held_gun = false;
    if (count == 0) {
        return true;
    }
    std::array<u64, 64> events{};
    const u64 array = CompatWord(compat.aggregator + 0x78);
    if (array == 0 || !CompatRead(array, events.data(), count * sizeof(events[0]))) {
        return false;
    }
    for (u32 i = 0; i < count; ++i) {
        if (events[i] == 0) {
            continue;
        }
        u64 descriptor = 0;
        if (!CompatRead(events[i] + 0x60, &descriptor, sizeof(descriptor))) {
            return false;
        }
        if (descriptor == compat.gun_descriptor) {
            u8 type = 0;
            if (!CompatRead(events[i] + 0x78, &type, sizeof(type))) {
                return false;
            }
            if (type == 2) {
                held_gun = true;
                return true;
            }
        }
    }
    return true;
}

u32 PS4_SYSV_ABI CompatBuffer(void* buffer, void* owner, void* event, void* first, void* second) {
    if (!compat_enabled || compat_queue == nullptr ||
        compat_queue->event != reinterpret_cast<u64>(event) ||
        compat_queue->generation != compat.generation ||
        compat_queue->aggregator != compat.aggregator ||
        reinterpret_cast<u64>(buffer) != compat_queue->aggregator + 0x78) {
        return native_buffer_event(buffer, owner, event, first, second);
    }
    const u64 address = reinterpret_cast<u64>(buffer);
    u32 count = 0;
    u32 capacity = 0;
    if (!CompatRead(address + 8, &count, sizeof(count)) || count == 0 || count > 64 ||
        !CompatRead(address + 12, &capacity, sizeof(capacity)) || capacity < count) {
        return native_buffer_event(buffer, owner, event, first, second);
    }
    std::array<u64, 64> events{};
    const u64 array = CompatWord(address);
    if (array == 0 || !CompatRead(array, events.data(), count * sizeof(events[0]))) {
        return native_buffer_event(buffer, owner, event, first, second);
    }
    bool only_held_gun = true;
    for (u32 i = 0; i < count; ++i) {
        u64 descriptor = 0;
        u8 type = 255;
        if (events[i] == 0 || !CompatRead(events[i] + 0x60, &descriptor, sizeof(descriptor)) ||
            descriptor == 0 || !CompatRead(events[i] + 0x78, &type, sizeof(type))) {
            return native_buffer_event(buffer, owner, event, first, second);
        }
        only_held_gun &= descriptor == compat_queue->gun_descriptor && type == 2;
    }
    u8 policy = 255;
    if (!CompatRead(address + 0x10, &policy, sizeof(policy)) || policy > 2) {
        return native_buffer_event(buffer, owner, event, first, second);
    }
    if (!compat_queue->attack && !only_held_gun) {

        return 0;
    }
    if (compat_queue->attack && only_held_gun && policy != 0) {
        struct PolicyScope {
            u8& value;
            u8 previous;
            explicit PolicyScope(u8& target) : value{target}, previous{target} {
                value = 0;
            }
            ~PolicyScope() {
                value = previous;
            }
        };
        u32 accepted = 0;
        {
            PolicyScope policy_scope{*reinterpret_cast<u8*>(address + 0x10)};
            accepted = native_buffer_event(buffer, owner, event, first, second);
        }

        return accepted;
    }
    return native_buffer_event(buffer, owner, event, first, second);
}

void PS4_SYSV_ABI CompatBool(void* object, void* frame, void* result) {
    if (!compat_enabled) {
        return compat_bool(object, frame, result);
    }
    thread_local bool interpreter_checked = false;
    if (!interpreter_checked) {
        const u64 slot = guest_base + 0x3960040 + 0x2d * 16;
        const u64 target = Word(slot);
        const u64 adjustment = Word(slot + 8);
        if (target != guest_base + 0x55b90 || adjustment != 0) {
            compat_enabled = false;
            LOG_ERROR(Input,
                      "[DMC PLAY] Interpreter layout changed; charge compatibility disabled");

            return compat_bool(object, frame, result);
        }
        interpreter_checked = true;
    }
    const u64 code = Operand(frame);
    const bool instance = *reinterpret_cast<const u8*>(code) == 0x01;
    const u64 property = instance ? LiveWord(code + 1) : 0;
    if (property == 0 || result == nullptr) {
        return compat_bool(object, frame, result);
    }
    u64 consumed_property = compat_consumed_property.load(std::memory_order_acquire);
    if (consumed_property == 0) {
        const u64 function = LiveWord(reinterpret_cast<u64>(frame) + 0x18);
        if (!CompatFunction(function, "PlayerTick") ||
            !CompatClass(reinterpret_cast<u64>(object), "DVPlayerController_Dante") ||
            !CompatName(property, "m_ConsumedHeldInput_RangedAttack")) {
            return compat_bool(object, frame, result);
        }
        compat_player_tick.store(function, std::memory_order_relaxed);
        compat_consumed_property.store(property, std::memory_order_release);
        consumed_property = property;
    }
    if (property != consumed_property ||
        LiveWord(reinterpret_cast<u64>(frame) + 0x18) !=
            compat_player_tick.load(std::memory_order_relaxed) ||
        (reinterpret_cast<u64>(object) != compat.controller &&
         !CompatClass(reinterpret_cast<u64>(object), "DVPlayerController_Dante"))) {
        return compat_bool(object, frame, result);
    }
    compat_bool(object, frame, result);
    CompatController(reinterpret_cast<u64>(object));
    CompatModeIntent();

    if (compat.gun == CompatGun::None) {
        return;
    }

    u32 buffered = 0;
    const auto now = std::chrono::steady_clock::now();
    const bool pending = CompatPending();

    bool buffered_gun = false;
    const bool readable = pending && CompatBufferedGun(buffered, buffered_gun);

    if (readable && !buffered_gun && now - compat.last_retry >= std::chrono::milliseconds{100}) {
        u32 consumed = 0;
        std::memcpy(&consumed, result, sizeof(consumed));
        if (consumed == 0) {
            return;
        }
        compat.last_retry = now;
        const u32 retry = 0;
        std::memcpy(result, &retry, sizeof(retry));
    }
}

void CompatDispatch(void* object, void* function, void* parameters, void* result, u64 caller) {
    const u64 address = reinterpret_cast<u64>(object);
    native_condition_dispatch(object, function, parameters, result);

    const bool graph_condition = caller == guest_base + 0xa1fee9;
    const bool selection_condition =
        compat.selecting && (caller == guest_base + 0xa2090c || caller == guest_base + 0xa235b9);
    if ((!graph_condition && !selection_condition) || parameters == nullptr ||
        compat.gun == CompatGun::None) {
        return;
    }
    if (!CompatPending() || CompatWord(reinterpret_cast<u64>(parameters) + 16) != compat.pawn) {
        return;
    }
    const u64 event = CompatWord(reinterpret_cast<u64>(parameters));
    u8 type = 0;
    if (event == 0 || compat.gun_descriptor == 0 ||
        CompatWord(event + 0x60) != compat.gun_descriptor ||
        !CompatRead(event + 0x78, &type, sizeof(type)) || type != 2) {
        return;
    }
    const u64 outer = CompatWord(address + 0x40);
    const bool root_token =
        CompatName(outer, "RangeAttacksRoot") &&
        CompatClass(address, "DVActionGraph_ConditionDef_Token") &&
        CompatNameIndex(static_cast<u32>(CompatWord(address + 0x60)), "RangedDenied");
    const bool original_weapon =
        selection_condition && outer == compat.candidate && outer != 0 &&
        CompatClass(address, "DVActionGraph_ConditionDef_SelectedRangedWeapon");
    u8 requested_mode = 255;
    u8 current_mode = 255;
    bool original_mode = false;
    if (selection_condition && caller == guest_base + 0xa235b9 && outer == compat.candidate &&
        outer != 0 && CompatClass(address, "DVActionGraph_ConditionDef_PlayMode")) {
        const u32 offset = CompatOffset(address, "PlayMode");
        original_mode =
            offset != 0 && compat.mode_offset != 0 &&
            CompatRead(address + offset, &requested_mode, sizeof(requested_mode)) &&
            requested_mode == 0 &&
            CompatRead(compat.pawn + compat.mode_offset, &current_mode, sizeof(current_mode)) &&
            (current_mode == 1 || current_mode == 2);
    }
    if (root_token || original_weapon || original_mode) {
        const u32 accepted = 1;
        std::memcpy(static_cast<u8*>(parameters) + 24, &accepted, sizeof(accepted));
    }
}

void PS4_SYSV_ABI CompatLet(void* object, void* frame, void* result) {
    if (!compat_enabled) {
        return original.load(std::memory_order_acquire)(object, frame, result);
    }
    const u64 code = Operand(frame);
    const u64 shot_site = compat_shot_site.load(std::memory_order_relaxed);
    if (shot_site != 0 && code != shot_site) {
        return original.load(std::memory_order_acquire)(object, frame, result);
    }
    if (shot_site == 0) {
        const u64 function = LiveWord(reinterpret_cast<u64>(frame) + 0x18);
        if (!CompatFunction(function, "OnStart") ||
            !CompatName(CompatWord(function + 0x40), "DVAction_Dante_RangedAttack")) {
            return original.load(std::memory_order_acquire)(object, frame, result);
        }
    }
    const u64 address = reinterpret_cast<u64>(object);
    const auto gun = CompatClass(address, "DVAction_Dante_ShotgunShot")      ? CompatGun::Shotgun
                     : CompatClass(address, "DVAction_Dante_EbonyIvoryShot") ? CompatGun::Pistols
                                                                             : CompatGun::None;
    if (gun == CompatGun::None) {
        return original.load(std::memory_order_acquire)(object, frame, result);
    }
    const auto* bytes = reinterpret_cast<const u8*>(code);
    Assignment assignment;
    const bool shot = bytes[0] == 0x19 && bytes[1] == 0x01 && Decode(code, assignment) &&
                      assignment.value == 0x3f800000 &&
                      CompatName(assignment.property, "m_fcpGunOn");
    original.load(std::memory_order_acquire)(object, frame, result);
    if (!shot || !compat_enabled) {
        return;
    }
    u32 owner_offset = 0;
    const bool owner_valid =
        CompatName(assignment.owner_property, "m_Dante") &&
        CompatRead(assignment.owner_property + 0x90, &owner_offset, sizeof(owner_offset)) &&
        owner_offset >= 0x58 && owner_offset < 0x10000;
    const u64 pawn = owner_valid ? CompatWord(address + owner_offset) : 0;
    const u64 controller = compat.controller;

    if (controller != 0 && pawn != 0 && pawn == compat.pawn) {
        CompatController(controller);

        if (pawn == compat.pawn && CompatHeld()) {

            compat.gun = gun;
            compat.mode_held = false;
            ++compat.generation;
        }
    }
    compat_shot_site.store(code, std::memory_order_relaxed);
}

u32 CompatMatch(void* definition, void* event) {
    if (!compat.selecting) {
        return native_match_event(definition, event);
    }
    const u64 address = reinterpret_cast<u64>(definition);
    const auto target = CompatTarget(address);
    if (target == CompatGun::None || target != compat.gun) {
        compat.candidate = 0;
        return 0;
    }
    auto* mode = static_cast<u8*>(event) + 0x90;
    const u8 previous_mode = *mode;
    *mode = 0;
    const u32 accepted = native_match_event(definition, event);
    *mode = previous_mode;
    compat.candidate = accepted != 0 ? address : 0;

    return accepted;
}

void* CompatSelect(void* set, void* event) {
    struct SelectionScope {
        bool previous = compat.selecting;
        u64 candidate = compat.candidate;
        SelectionScope() {
            compat.selecting = false;
            compat.candidate = 0;
        }
        ~SelectionScope() {
            compat.selecting = previous;
            compat.candidate = candidate;
        }
    } scope;
    const u64 address = reinterpret_cast<u64>(event);
    if (event == nullptr || compat.gun == CompatGun::None || compat.gun_descriptor == 0 ||
        LiveWord(address + 0x60) != compat.gun_descriptor ||
        *reinterpret_cast<const u8*>(address + 0x78) != 2) {
        void* selected = native_select_link(set, event);

        return selected;
    }
    compat.selecting = CompatPending();
    if (compat.selecting) {
        const u64 set_address = reinterpret_cast<u64>(set);
        const u64 links = LiveWord(set_address + 0x60);
        const u32 count = LiveDword(set_address + 0x68);
        bool charge_entry = false;
        if (count <= 256) {
            for (u32 i = 0; links != 0 && i < count; ++i) {
                const u64 link = LiveWord(links + i * 8);
                if (link != 0 && CompatTarget(LiveWord(link + 0x60)) == compat.gun) {
                    charge_entry = true;
                    break;
                }
            }
        }
        compat.selecting = charge_entry;
    }
    compat.candidate = 0;
    void* selected = native_select_link(set, event);

    return selected;
}

u32 CompatAggregate(void* object, void* event, u32 input) {
    CompatQueueScope queue_scope;
    const u64 address = reinterpret_cast<u64>(event);
    if (input != 0 && event != nullptr && (compat.gun != CompatGun::None || compat.mode_held) &&
        *reinterpret_cast<const u8*>(address + 0x78) == 2) {
        const u64 descriptor = LiveWord(address + 0x60);
        if (descriptor != 0 &&
            (descriptor == compat.gun_descriptor ||
             CompatName(descriptor, "DVButton_RangedAttack")) &&
            CompatHeld()) {
            compat.gun_descriptor = descriptor;
            compat.aggregator = reinterpret_cast<u64>(object);
        }
    }
    if (event != nullptr && compat.gun != CompatGun::None && compat.gun_descriptor != 0 &&
        reinterpret_cast<u64>(object) == compat.aggregator) {
        if (compat.aggregator == CompatWord(compat.pawn + 0x930)) {
            const u64 incoming = LiveWord(address + 0x60);
            const u8 type = *reinterpret_cast<const u8*>(address + 0x78);
            const bool held = type == 2 && incoming == compat.gun_descriptor;
            const bool attack = type == 0 && (CompatName(incoming, "DVButton_GunSpecial") ||
                                              CompatName(incoming, "DVButton_LightMelee") ||
                                              CompatName(incoming, "DVButton_StrongMelee"));
            if ((held || attack) && CompatPending()) {
                queue_scope.event = address;
                queue_scope.descriptor = incoming;
                queue_scope.aggregator = compat.aggregator;
                queue_scope.gun_descriptor = compat.gun_descriptor;
                queue_scope.generation = compat.generation;
                queue_scope.type = type;
                queue_scope.attack = attack;
                compat_queue = &queue_scope;
            }
        }
    }

    return native_aggregate_event(object, event, input);
}

void PS4_SYSV_ABI Dispatch(void* object, void* function, void* parameters, void* result) {
    if (!compat_enabled) {
        return native_condition_dispatch(object, function, parameters, result);
    }
    CompatDispatch(object, function, parameters, result,
                   reinterpret_cast<u64>(__builtin_return_address(0)));
}
u32 PS4_SYSV_ABI Match(void* definition, void* event) {
    return compat_enabled ? CompatMatch(definition, event) : native_match_event(definition, event);
}
void* PS4_SYSV_ABI Select(void* set, void* event) {
    return compat_enabled ? CompatSelect(set, event) : native_select_link(set, event);
}
u32 PS4_SYSV_ABI Aggregate(void* object, void* event, u32 input) {
    return compat_enabled ? CompatAggregate(object, event, input)
                          : native_aggregate_event(object, event, input);
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
        LOG_WARNING(Input, "[DMC PLAY] Compatibility hook page protection could not be restored");
    }
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(address), Size);
    return trampoline;
}

#include "input/dmc_camera_speed.inl"

} // namespace

void Install() {
    if (native_aggregate_event != nullptr ||
        Common::ElfInfo::Instance().GameSerial() != "CUSA01022" ||
        Common::ElfInfo::Instance().AppVer() != "01.03") {
        return;
    }
    const u64 base = MemoryPatcher::g_eboot_address;
    if (base == 0) {
        return;
    }
    guest_base = base;
    CameraSpeed::Install(base);
    constexpr std::array<u8, 16> bool_bytes{0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56,
                                            0x53, 0x50, 0x49, 0x89, 0xd6, 0x48, 0x89, 0xfb};
    constexpr std::array<u8, 17> compat_dispatch_bytes{0x55, 0x48, 0x89, 0xe5, 0x41, 0x57,
                                                       0x41, 0x56, 0x41, 0x55, 0x41, 0x54,
                                                       0x53, 0x48, 0x83, 0xec, 0x58};
    auto compat_select_bytes = compat_dispatch_bytes;
    compat_select_bytes.back() = 0x38;
    auto compat_let_bytes = compat_dispatch_bytes;
    compat_let_bytes.back() = 0x18;
    constexpr std::array<u8, 14> compat_match_bytes{0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41,
                                                    0x56, 0x41, 0x54, 0x53, 0x49, 0x89, 0xf7};
    constexpr std::array<u8, 10> compat_link_layout{0x41, 0x39, 0x5f, 0x68, 0x7e,
                                                    0x26, 0x49, 0x8b, 0x47, 0x60};
    const auto matches = [](u64 address, const auto& expected) {
        auto actual = expected;
        return Read(address, actual.data(), actual.size()) && actual == expected;
    };
    if (!matches(base + 0x55b90, bool_bytes) || !matches(base + 0x567e0, compat_let_bytes) ||
        !matches(base + 0xa1f980, compat_let_bytes) ||
        !matches(base + 0x6d1e0, compat_dispatch_bytes) ||
        !matches(base + 0xa20850, compat_select_bytes) ||
        !matches(base + 0xa26420, compat_match_bytes) ||
        !matches(base + 0xa1fe10, compat_select_bytes) ||
        !matches(base + 0xa1c3c0, compat_let_bytes) ||
        !matches(base + 0xa20973, compat_link_layout)) {
        LOG_ERROR(Input, "[DMC PLAY] Executable validation failed; charge compatibility disabled");
        return;
    }
    native_condition_dispatch = reinterpret_cast<ConditionDispatch>(
        NativeEntry(base + 0x6d1e0, reinterpret_cast<u64>(&Dispatch), compat_dispatch_bytes));
    native_match_event = reinterpret_cast<GraphEvent>(
        NativeEntry(base + 0xa26420, reinterpret_cast<u64>(&Match), compat_match_bytes));
    native_select_link = reinterpret_cast<SelectLink>(
        NativeEntry(base + 0xa20850, reinterpret_cast<u64>(&Select), compat_select_bytes));

    native_aggregate_event = reinterpret_cast<AggregateEvent>(
        NativeEntry(base + 0xa1f980, reinterpret_cast<u64>(&Aggregate), compat_let_bytes));
    native_buffer_event = reinterpret_cast<BufferEvent>(
        NativeEntry(base + 0xa1c3c0, reinterpret_cast<u64>(&CompatBuffer), compat_let_bytes));
    compat_bool = reinterpret_cast<Native>(
        NativeEntry(base + 0x55b90, reinterpret_cast<u64>(&CompatBool), bool_bytes));
    original.store(reinterpret_cast<Native>(NativeEntry(
                       base + 0x567e0, reinterpret_cast<u64>(&CompatLet), compat_let_bytes)),
                   std::memory_order_release);
    compat_enabled = native_condition_dispatch != nullptr && native_match_event != nullptr &&
                     native_select_link != nullptr && compat_bool != nullptr &&
                     native_aggregate_event != nullptr && native_buffer_event != nullptr &&
                     original.load(std::memory_order_acquire) != nullptr;

    LOG_INFO(Input,
             "[DMC PLAY] Charge compatibility installed={}; no input recorder or diagnostic probes",
             compat_enabled.load());
}
} // namespace Input::DmcPlay
#else
namespace Input::DmcPlay {
void Install() {}
} // namespace Input::DmcPlay
#endif
