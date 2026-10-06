// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <initializer_list>
#include <string_view>
#include "common/types.h"

namespace Input::PadTrace {

void Initialize();
void Shutdown();
bool Enabled();
u64 NextId();
u64 CurrentEvent();
void Record(const char* kind, std::initializer_list<s64> fields);
void Configuration(std::string_view text);

class EventScope {
public:
    EventScope();
    ~EventScope();
    EventScope(const EventScope&) = delete;
    EventScope& operator=(const EventScope&) = delete;

private:
    u64 previous;
};

} // namespace Input::PadTrace
