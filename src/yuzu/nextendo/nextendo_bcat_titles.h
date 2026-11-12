// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/common_types.h"

// Titles whose online data (schedule seed or BCAT delivery package) Nextendo serves.
namespace Nextendo::BcatTitles {

inline constexpr u64 Splatoon2Eu = 0x0100f8f0000a2000ULL;
inline constexpr u64 Splatoon2Us = 0x01003bc0000a0000ULL;
inline constexpr u64 Splatoon2Jp = 0x01003c700009c800ULL;
inline constexpr u64 Splatoon3 = 0x0100C2500FC20000ULL;
inline constexpr u64 PokemonViolet = 0x01008F6008C5E000ULL;
inline constexpr u64 PokemonScarlet = 0x0100A3D008C5C000ULL;
inline constexpr u64 PokemonLegendsZa = 0x0100F43008C44000ULL;
inline constexpr u64 NintendoSwitchSports = 0x0100D2F00D5C0000ULL;
inline constexpr u64 PokemonSword = 0x0100ABF008968000ULL;
inline constexpr u64 PokemonShield = 0x01008DB008C2C000ULL;
// Super Smash Bros. Ultimate reads its news ticker, gifts and announcements from its delivery
// cache; without it the ticker says "Connect to the Internet".
inline constexpr u64 SuperSmashBrosUltimate = 0x01006A800016E000ULL;

inline constexpr bool IsSplatoon2(u64 title_id) {
    return title_id == Splatoon2Eu || title_id == Splatoon2Us || title_id == Splatoon2Jp;
}

// Plain package from /api/bcat/<title>, installed as the title's whole delivery cache.
inline constexpr bool IsPackage(u64 title_id) {
    return title_id == PokemonViolet || title_id == PokemonScarlet ||
           title_id == PokemonLegendsZa || title_id == NintendoSwitchSports ||
           title_id == PokemonSword || title_id == PokemonShield ||
           title_id == SuperSmashBrosUltimate;
}

inline constexpr bool IsBcatTitle(u64 title_id) {
    return IsSplatoon2(title_id) || title_id == Splatoon3 || IsPackage(title_id);
}

} // namespace Nextendo::BcatTitles
