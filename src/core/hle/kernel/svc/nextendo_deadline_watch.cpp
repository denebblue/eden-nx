// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>

#include "core/hle/kernel/svc/nextendo_deadline_watch.h"

namespace Kernel::Svc {

std::atomic<u64> g_nextendo_deadline_watch_until_ms{0};

namespace {
u64 NowMs() {
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count());
}
} // namespace

void ArmNextendoDeadlineWatch(u64 duration_ms) {
    g_nextendo_deadline_watch_until_ms.store(NowMs() + duration_ms, std::memory_order_relaxed);
}

bool IsNextendoDeadlineWatchActive() {
    const u64 until_ms = g_nextendo_deadline_watch_until_ms.load(std::memory_order_relaxed);
    if (until_ms == 0) {
        return false;
    }
    if (NowMs() >= until_ms) {
        g_nextendo_deadline_watch_until_ms.store(0, std::memory_order_relaxed);
        return false;
    }
    return true;
}

} // namespace Kernel::Svc
