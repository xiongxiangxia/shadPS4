// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#ifdef ENABLE_DMC_CAMERA_SPEED
namespace CameraSpeed {
template <std::size_t Size>
u8* Prepare(u64 address, const std::array<u8, Size>& expected, std::size_t split,
            std::array<u8, 8> multiply) {
    constexpr std::size_t constant_offset = Size + 22;
    constexpr std::size_t allocation_size = constant_offset + sizeof(float);
    auto* code = static_cast<u8*>(
        VirtualAlloc(nullptr, allocation_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (code == nullptr) {
        return nullptr;
    }
    const s32 displacement = static_cast<s32>(constant_offset - split - multiply.size());
    std::memcpy(multiply.data() + 4, &displacement, sizeof(displacement));
    std::memcpy(code, expected.data(), split);
    std::memcpy(code + split, multiply.data(), multiply.size());
    std::memcpy(code + split + multiply.size(), expected.data() + split, Size - split);
    AbsoluteJump(code + Size + multiply.size(), address + Size);
    constexpr float factor = 2.5f;
    std::memcpy(code + constant_offset, &factor, sizeof(factor));
    DWORD previous = 0;
    if (!VirtualProtect(code, allocation_size, PAGE_EXECUTE_READ, &previous)) {
        VirtualFree(code, 0, MEM_RELEASE);
        return nullptr;
    }
    FlushInstructionCache(GetCurrentProcess(), code, allocation_size);
    return code;
}

void Install(u64 base) {
    static bool attempted = false;
    if (attempted) {
        return;
    }
    attempted = true;
    const char* setting = std::getenv("SHADPS4_DMC_CAMERA_SPEED");
    if (setting != nullptr && std::strcmp(setting, "0") == 0) {
        LOG_INFO(Input, "[DMC CAMERA] Original camera speed requested");
        return;
    }
    constexpr u64 yaw_offset = 0x9b5eac;
    constexpr u64 pitch_offset = 0x9ae354;
    constexpr std::array<u8, 21> yaw{0xc5, 0xda, 0x59, 0xc9, 0xc5, 0xf2, 0x59,
                                     0xca, 0xc5, 0xf2, 0x59, 0x51, 0x7c, 0xc5,
                                     0xf8, 0x2e, 0x9f, 0x30, 0x03, 0x00, 0x00};
    constexpr std::array<u8, 17> pitch{0xc5, 0xe2, 0x59, 0xde, 0xc5, 0xe2, 0x59, 0xc0, 0xc5,
                                       0xfa, 0x10, 0x5d, 0xe8, 0xc5, 0xe2, 0x58, 0xc0};
    std::array<u8, yaw.size()> actual_yaw{};
    std::array<u8, pitch.size()> actual_pitch{};
    if (!Read(base + yaw_offset, actual_yaw.data(), actual_yaw.size()) || actual_yaw != yaw ||
        !Read(base + pitch_offset, actual_pitch.data(), actual_pitch.size()) ||
        actual_pitch != pitch) {
        LOG_ERROR(Input, "[DMC CAMERA] Camera code differs; speed patch skipped");
        return;
    }
    auto* yaw_code =
        Prepare(base + yaw_offset, yaw, 13, std::array<u8, 8>{0xc5, 0xea, 0x59, 0x15, 0, 0, 0, 0});
    auto* pitch_code = Prepare(base + pitch_offset, pitch, 8,
                               std::array<u8, 8>{0xc5, 0xfa, 0x59, 0x05, 0, 0, 0, 0});
    const auto release = [&] {
        if (yaw_code != nullptr) {
            VirtualFree(yaw_code, 0, MEM_RELEASE);
        }
        if (pitch_code != nullptr) {
            VirtualFree(pitch_code, 0, MEM_RELEASE);
        }
    };
    if (yaw_code == nullptr || pitch_code == nullptr) {
        release();
        LOG_ERROR(Input, "[DMC CAMERA] Could not allocate both camera patches; speed unchanged");
        return;
    }
    auto* yaw_site = reinterpret_cast<void*>(base + yaw_offset);
    auto* pitch_site = reinterpret_cast<void*>(base + pitch_offset);
    DWORD yaw_protect = 0;
    DWORD pitch_protect = 0;
    if (!VirtualProtect(yaw_site, yaw.size(), PAGE_EXECUTE_READWRITE, &yaw_protect)) {
        release();
        LOG_ERROR(Input, "[DMC CAMERA] Camera yaw page is not writable; speed unchanged");
        return;
    }
    if (!VirtualProtect(pitch_site, pitch.size(), PAGE_EXECUTE_READWRITE, &pitch_protect)) {
        DWORD unused = 0;
        if (!VirtualProtect(yaw_site, yaw.size(), yaw_protect, &unused)) {
            LOG_WARNING(Input, "[DMC CAMERA] Could not restore camera yaw page protection");
        }
        release();
        LOG_ERROR(Input, "[DMC CAMERA] Camera pitch page is not writable; speed unchanged");
        return;
    }
    std::array<u8, yaw.size()> yaw_patch{};
    std::array<u8, pitch.size()> pitch_patch{};
    yaw_patch.fill(0x90);
    pitch_patch.fill(0x90);
    AbsoluteJump(yaw_patch.data(), reinterpret_cast<u64>(yaw_code));
    AbsoluteJump(pitch_patch.data(), reinterpret_cast<u64>(pitch_code));
    std::memcpy(yaw_site, yaw_patch.data(), yaw_patch.size());
    std::memcpy(pitch_site, pitch_patch.data(), pitch_patch.size());
    DWORD unused = 0;
    const bool yaw_restored = VirtualProtect(yaw_site, yaw.size(), yaw_protect, &unused);
    const bool pitch_restored = VirtualProtect(pitch_site, pitch.size(), pitch_protect, &unused);
    if (!yaw_restored || !pitch_restored) {
        LOG_WARNING(Input,
                    "[DMC CAMERA] Could not restore camera page protection: yaw={}, pitch={}",
                    yaw_restored, pitch_restored);
    }
    FlushInstructionCache(GetCurrentProcess(), yaw_site, yaw.size());
    FlushInstructionCache(GetCurrentProcess(), pitch_site, pitch.size());
    LOG_INFO(Input,
             "[DMC CAMERA] Manual yaw/pitch speed x2.5 installed; original limits and game time "
             "preserved; SHADPS4_DMC_CAMERA_SPEED=0 restores default on next launch");
}
} // namespace CameraSpeed
#endif
