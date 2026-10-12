// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "input/dmc_charge_trace.h"

#if defined(ENABLE_PADTRACE) && defined(_WIN64)
#include <array>
#include <atomic>
#include <cstring>
#include <limits>
#include <mutex>
#include <windows.h>
#include "common/elf_info.h"
#include "common/logging/log.h"
#include "common/memory_patcher.h"
#include "common/types.h"
#include "input/pad_trace.h"

namespace Input::DmcChargeTrace {
namespace {
using Native = void(PS4_SYSV_ABI*)(void*, void*, void*);
std::atomic<Native> original;
std::atomic<Native> original_final;
std::atomic<Native> original_virtual;
std::atomic<u64> charge_property{0};
std::mutex nodes_mutex;
struct NodeEntry {
    u64 address = 0;
    u64 name = 0;
};
std::array<NodeEntry, 64> nodes{};
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

void RememberNode(u64 node) {
    if (node == 0) {
        return;
    }
    std::lock_guard lock{nodes_mutex};
    for (auto& entry : nodes) {
        if (entry.address == node) {
            return;
        }
        if (entry.address == 0) {
            entry = {node, Word(node + 0x48)};
            return;
        }
    }
}

bool Watched(u64 value, bool name) {
    std::lock_guard lock{nodes_mutex};
    for (const auto& node : nodes) {
        if (node.address != 0 && (name ? node.name == value : node.address == value)) {
            return true;
        }
    }
    return false;
}

void Frames(u64 request, u64 frame, bool remember) {
    for (int depth = 0; depth < 8 && frame != 0; ++depth) {
        std::array<u64, 9> words{};
        if (!Read(frame, words.data(), sizeof(words))) {
            break;
        }
        if (remember) {
            RememberNode(words[3]);
        }
        PadTrace::Record("DMC_FRAME", {static_cast<s64>(request), depth, static_cast<s64>(frame),
                                       static_cast<s64>(words[3]), static_cast<s64>(words[4]),
                                       static_cast<s64>(words[5]), static_cast<s64>(words[7]),
                                       static_cast<s64>(Word(words[3] + 0x48)),
                                       static_cast<s64>(Word(Word(words[3] + 0x40) + 0x48))});
        if (words[7] == frame) {
            break;
        }
        frame = words[7];
    }
}

void ObserveCall(Native handler, bool name, void* object, void* frame, void* result) {
    const u64 code = PadTrace::Enabled() ? Word(reinterpret_cast<u64>(frame) + 0x28) : 0;
    const u64 target = code != 0 ? Word(code) : 0;
    const bool matched = code != 0 && Watched(target, name);
    const u64 request = matched ? PadTrace::NextId() : 0;
    if (matched) {
        PadTrace::Record("DMC_CALL_BEGIN", {static_cast<s64>(request), name,
                                            static_cast<s64>(target), reinterpret_cast<s64>(object),
                                            reinterpret_cast<s64>(frame), static_cast<s64>(code)});
        Frames(request, reinterpret_cast<u64>(frame), true);
    }
    handler(object, frame, result);
    if (matched) {
        PadTrace::Record("DMC_CALL_END",
                         {static_cast<s64>(request), name, static_cast<s64>(target)});
    }
}

void PS4_SYSV_ABI ObserveFinal(void* object, void* frame, void* result) {
    const auto handler = original_final.load(std::memory_order_acquire);
    if (!PadTrace::Enabled()) {
        return handler(object, frame, result);
    }
    ObserveCall(handler, false, object, frame, result);
}

void PS4_SYSV_ABI ObserveVirtual(void* object, void* frame, void* result) {
    const auto handler = original_virtual.load(std::memory_order_acquire);
    if (!PadTrace::Enabled()) {
        return handler(object, frame, result);
    }
    ObserveCall(handler, true, object, frame, result);
}

void PS4_SYSV_ABI ObserveLet(void* object, void* frame, void* result) {
    const auto handler = original.load(std::memory_order_acquire);
    Assignment assignment;
    const u64 code = PadTrace::Enabled() ? Word(reinterpret_cast<u64>(frame) + 0x28) : 0;
    const bool matched = code != 0 && Decode(code, assignment);
    const bool cleanup = matched && IsCleanupCandidate(code, assignment);
    const u64 request = matched ? PadTrace::NextId() : 0;
    if (cleanup) {
        charge_property.store(assignment.property, std::memory_order_relaxed);
    }
    if (matched) {
        PadTrace::Record("DMC_LET_BEGIN",
                         {static_cast<s64>(request), reinterpret_cast<s64>(object),
                          reinterpret_cast<s64>(frame), static_cast<s64>(code),
                          static_cast<s64>(assignment.property),
                          static_cast<s64>(assignment.owner_property), assignment.value, cleanup,
                          assignment.property == charge_property.load()});
        Frames(request, reinterpret_cast<u64>(frame),
               cleanup || assignment.property == charge_property.load());
    }
    handler(object, frame, result);
    if (matched) {
        const u64 destination = Word(guest_base + 0x3940bc0);
        const u64 property = Word(guest_base + 0x3940b90);
        u32 actual = 0;
        const bool readable = Read(destination, &actual, sizeof(actual));
        PadTrace::Record("DMC_LET_END",
                         {static_cast<s64>(request), static_cast<s64>(destination),
                          static_cast<s64>(property), actual, readable,
                          property == assignment.property,
                          static_cast<s64>(Word(reinterpret_cast<u64>(frame) + 0x28))});
    }
}
} // namespace

void Install() {
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
    const auto install_call = [base](u32 token, Native observer, std::atomic<Native>& saved) {
        const u64 entry = base + 0x3960040 + token * 16;
        const u64 target = Word(entry);
        if (target < base || target >= base + 0x2f7b360 || (target & 1) != 0 ||
            Word(entry + 8) != 0) {
            PadTrace::Record("DMC_HOOK_CALL", {token, 0, static_cast<s64>(target)});
            return;
        }
        saved.store(reinterpret_cast<Native>(target), std::memory_order_release);
        const auto prior = InterlockedCompareExchange64(reinterpret_cast<volatile LONG64*>(entry),
                                                        reinterpret_cast<LONG64>(observer),
                                                        static_cast<LONG64>(target));
        PadTrace::Record("DMC_HOOK_CALL",
                         {token, static_cast<u64>(prior) == target, static_cast<s64>(target)});
    };
    install_call(0x1c, &ObserveFinal, original_final);
    install_call(0x1b, &ObserveVirtual, original_virtual);
    LOG_INFO(
        Input,
        "[DMC TRACE] Script assignment observer installed; cleanup identification is provisional");
}
} // namespace Input::DmcChargeTrace
#else
namespace Input::DmcChargeTrace {
void Install() {}
} // namespace Input::DmcChargeTrace
#endif
