// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>

// One-shot NAT test combining:
//  - STUN (RFC 5389/5780) against public servers: the mapping behaviour (is the public port the
//    same for every destination?) and the filtering behaviour (does the router let in packets
//    from an address/port it never sent to?), which together give the NAT type;
//  - the Nextendo NAT-check responders (the same nncs UDP protocol consoles use), which confirm
//    the Nextendo servers are reachable over UDP from this network.
namespace Nextendo::NatTest {

enum class NatType {
    Blocked,              // no UDP reply at all
    OpenInternet,         // public IP, no NAT
    FullCone,             // endpoint-independent mapping and filtering
    RestrictedCone,       // endpoint-independent mapping, address-dependent filtering
    PortRestrictedCone,   // endpoint-independent mapping, address+port-dependent filtering
    ConeUnknownFiltering, // endpoint-independent mapping; server couldn't test filtering
    Symmetric,            // mapping changes per destination
};

enum class NextendoReach { Reachable, Unreachable };

struct Result {
    NatType type = NatType::Blocked;
    NextendoReach nextendo = NextendoReach::Unreachable;
    bool nextendo_consistent_port = false; // nncs saw the same public port from both servers
};

// Blocking (a few seconds); run on a worker thread.
Result Run();

// Approximate Nintendo Switch NAT type letter for display ("A".."F").
const char* SwitchLetter(NatType type);

} // namespace Nextendo::NatTest
