// SPDX-FileCopyrightText: Copyright 2026 Eden Nextendo
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>

// [Nextendo] Whether the title being booted plays online on Nextendo's servers. The frontend sets
// it before boot (NextendoApplyPerGameWorkarounds) and clears it when emulation stops. While it is
// set, the emulator favors steady frame pacing over raw shader throughput: an online match drops a
// player whose game stops answering for a few seconds, and a burst of shader compilation that eats
// every host core is the usual way that happens. Read from video_core (pipeline workers) and core
// (stall log), hence common/.
namespace Nextendo::OnlineTuning {

namespace Detail {
inline std::atomic<bool>& ActiveFlag() {
    static std::atomic<bool> active{false};
    return active;
}
} // namespace Detail

inline void SetActive(bool active) {
    Detail::ActiveFlag().store(active, std::memory_order_relaxed);
}

inline bool IsActive() {
    return Detail::ActiveFlag().load(std::memory_order_relaxed);
}

} // namespace Nextendo::OnlineTuning
