// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "input/dmc_charge_trace.h"

#if defined(ENABLE_PADTRACE) && defined(_WIN64)
#include <array>
#include <atomic>
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
HeldNative native_held = nullptr;
ClearNative native_clear = nullptr;
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

struct NativeCache {
    u64 generation = 0;
    u32 held_records = 0;
    u32 clear_records = 0;
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
    if (native_held != nullptr || native_clear != nullptr ||
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
    LOG_INFO(Input, "[DMC TRACE] Native probes before guest execution: held={}, node_clear={}",
             native_held != nullptr, native_clear != nullptr);
}

void Install() {
    capture_generation.fetch_add(1, std::memory_order_relaxed);
    PadTrace::Record("DMC_NATIVE_HOOK", {native_held != nullptr, native_clear != nullptr});
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
