// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "yuzu/nextendo/nextendo_nat_test.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <optional>
#include <random>
#include <set>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "common/common_types.h"
#include "common/logging.h"
#include "common/settings.h"

namespace Nextendo::NatTest {

namespace {

#ifdef _WIN32
using SocketHandle = SOCKET;
constexpr SocketHandle InvalidSocket = INVALID_SOCKET;
void CloseSocket(SocketHandle s) {
    closesocket(s);
}
#else
using SocketHandle = int;
constexpr SocketHandle InvalidSocket = -1;
void CloseSocket(SocketHandle s) {
    close(s);
}
#endif

struct Endpoint {
    u32 ip = 0; // host byte order
    u16 port = 0;
    bool operator==(const Endpoint&) const = default;
};

// Public STUN servers. The first implements RFC 5780 (OTHER-ADDRESS, CHANGE-REQUEST), which the
// filtering test needs; the others only answer plain binding requests (mapping test).
struct StunServer {
    const char* host;
    u16 port;
};
constexpr std::array<StunServer, 3> StunServers{{
    {"stun.stunprotocol.org", 3478},
    {"stun.l.google.com", 19302},
    {"stun.cloudflare.com", 3478},
}};

constexpr u32 MagicCookie = 0x2112A442;
constexpr u16 BindingRequest = 0x0001;
constexpr u16 BindingSuccess = 0x0101;
constexpr u16 AttrMappedAddress = 0x0001;
constexpr u16 AttrChangeRequest = 0x0003;
constexpr u16 AttrChangedAddress = 0x0005; // RFC 3489 name of OTHER-ADDRESS
constexpr u16 AttrXorMappedAddress = 0x0020;
constexpr u16 AttrOtherAddress = 0x802C;
constexpr u32 ChangeIp = 0x04;
constexpr u32 ChangePort = 0x02;

constexpr u32 NncsTestId = 101;
constexpr std::array<u16, 2> NncsPorts{10025, 10125};

u16 ReadU16(const u8* p) {
    return static_cast<u16>((p[0] << 8) | p[1]);
}
u32 ReadU32(const u8* p) {
    return (u32{p[0]} << 24) | (u32{p[1]} << 16) | (u32{p[2]} << 8) | u32{p[3]};
}
void WriteU16(u8* p, u16 v) {
    p[0] = static_cast<u8>(v >> 8);
    p[1] = static_cast<u8>(v);
}
void WriteU32(u8* p, u32 v) {
    p[0] = static_cast<u8>(v >> 24);
    p[1] = static_cast<u8>(v >> 16);
    p[2] = static_cast<u8>(v >> 8);
    p[3] = static_cast<u8>(v);
}

std::optional<u32> Resolve(const char* host) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* list = nullptr;
    if (getaddrinfo(host, nullptr, &hints, &list) != 0 || !list) {
        return std::nullopt;
    }
    const u32 ip = ntohl(reinterpret_cast<sockaddr_in*>(list->ai_addr)->sin_addr.s_addr);
    freeaddrinfo(list);
    return ip;
}

sockaddr_in ToSockaddr(const Endpoint& endpoint) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(endpoint.ip);
    addr.sin_port = htons(endpoint.port);
    return addr;
}

class UdpSocket {
public:
    UdpSocket() {
        handle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (handle != InvalidSocket) {
            sockaddr_in any{};
            any.sin_family = AF_INET;
            bind(handle, reinterpret_cast<sockaddr*>(&any), sizeof(any));
        }
    }
    ~UdpSocket() {
        if (handle != InvalidSocket) {
            CloseSocket(handle);
        }
    }
    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;

    bool Valid() const {
        return handle != InvalidSocket;
    }

    void Send(const Endpoint& to, const std::vector<u8>& data) {
        const sockaddr_in addr = ToSockaddr(to);
        sendto(handle, reinterpret_cast<const char*>(data.data()), static_cast<int>(data.size()),
               0, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
    }

    // Waits up to timeout_ms for one datagram.
    std::optional<std::pair<Endpoint, std::vector<u8>>> Receive(int timeout_ms) {
        fd_set set;
        FD_ZERO(&set);
        FD_SET(handle, &set);
        timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
        if (select(static_cast<int>(handle) + 1, &set, nullptr, nullptr, &tv) <= 0) {
            return std::nullopt;
        }
        std::vector<u8> buffer(2048);
        sockaddr_in from{};
        socklen_t from_len = sizeof(from);
        const auto received =
            recvfrom(handle, reinterpret_cast<char*>(buffer.data()),
                     static_cast<int>(buffer.size()), 0, reinterpret_cast<sockaddr*>(&from),
                     &from_len);
        if (received <= 0) {
            return std::nullopt;
        }
        buffer.resize(static_cast<std::size_t>(received));
        return std::make_pair(Endpoint{ntohl(from.sin_addr.s_addr), ntohs(from.sin_port)},
                              std::move(buffer));
    }

private:
    SocketHandle handle = InvalidSocket;
};

struct StunReply {
    Endpoint mapped;
    std::optional<Endpoint> other;
};

std::optional<Endpoint> ParseAddress(const u8* value, u16 length, bool xored) {
    if (length < 8 || value[1] != 0x01) { // IPv4 only
        return std::nullopt;
    }
    u16 port = ReadU16(value + 2);
    u32 ip = ReadU32(value + 4);
    if (xored) {
        port ^= static_cast<u16>(MagicCookie >> 16);
        ip ^= MagicCookie;
    }
    return Endpoint{ip, port};
}

std::optional<StunReply> StunRequest(UdpSocket& socket, const Endpoint& server, u32 change_flags) {
    static std::mt19937 rng{std::random_device{}()};
    std::vector<u8> request(change_flags ? 28 : 20, 0);
    WriteU16(request.data(), BindingRequest);
    WriteU16(request.data() + 2, static_cast<u16>(request.size() - 20));
    WriteU32(request.data() + 4, MagicCookie);
    std::array<u8, 12> transaction{};
    for (auto& b : transaction) {
        b = static_cast<u8>(rng());
    }
    std::memcpy(request.data() + 8, transaction.data(), transaction.size());
    if (change_flags) {
        WriteU16(request.data() + 20, AttrChangeRequest);
        WriteU16(request.data() + 22, 4);
        WriteU32(request.data() + 24, change_flags);
    }

    for (int attempt = 0; attempt < 3; ++attempt) {
        socket.Send(server, request);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(700);
        while (std::chrono::steady_clock::now() < deadline) {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                                       deadline - std::chrono::steady_clock::now())
                                       .count();
            auto datagram = socket.Receive(static_cast<int>(std::max<long long>(remaining, 1)));
            if (!datagram) {
                break;
            }
            const auto& data = datagram->second;
            if (data.size() < 20 || ReadU16(data.data()) != BindingSuccess ||
                std::memcmp(data.data() + 8, transaction.data(), transaction.size()) != 0) {
                continue; // stray or late reply to an earlier request
            }
            StunReply reply{};
            bool has_mapped = false;
            std::size_t offset = 20;
            while (offset + 4 <= data.size()) {
                const u16 type = ReadU16(data.data() + offset);
                const u16 length = ReadU16(data.data() + offset + 2);
                if (offset + 4 + length > data.size()) {
                    break;
                }
                const u8* value = data.data() + offset + 4;
                if (type == AttrXorMappedAddress) {
                    if (auto e = ParseAddress(value, length, true)) {
                        reply.mapped = *e;
                        has_mapped = true;
                    }
                } else if (type == AttrMappedAddress && !has_mapped) {
                    if (auto e = ParseAddress(value, length, false)) {
                        reply.mapped = *e;
                        has_mapped = true;
                    }
                } else if (type == AttrOtherAddress || type == AttrChangedAddress) {
                    reply.other = ParseAddress(value, length, false);
                }
                offset += 4 + ((length + 3u) & ~3u);
            }
            if (has_mapped) {
                return reply;
            }
        }
    }
    return std::nullopt;
}

struct NncsOutcome {
    bool reachable = false;
    bool consistent = false;
};

NncsOutcome ProbeNextendo() {
    NncsOutcome outcome;
    std::vector<Endpoint> targets;
    for (const std::string& host : {Settings::values.nextendo_server_ip.GetValue(),
                                    Settings::values.nextendo_nat_ip.GetValue()}) {
        const auto ip = Resolve(host.c_str());
        if (!ip) {
            continue;
        }
        for (const u16 port : NncsPorts) {
            targets.push_back(Endpoint{*ip, port});
        }
    }
    if (targets.empty()) {
        return outcome;
    }

    UdpSocket socket;
    if (!socket.Valid()) {
        return outcome;
    }
    std::vector<u8> probe(16, 0);
    WriteU32(probe.data(), NncsTestId);

    std::set<Endpoint, decltype([](const Endpoint& a, const Endpoint& b) {
                  return a.ip != b.ip ? a.ip < b.ip : a.port < b.port;
              })>
        answered;
    std::set<u32> external_ports;
    for (int round = 0; round < 2 && answered.size() < targets.size(); ++round) {
        for (const auto& target : targets) {
            socket.Send(target, probe);
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
        while (std::chrono::steady_clock::now() < deadline) {
            auto datagram = socket.Receive(200);
            if (!datagram) {
                continue;
            }
            const auto& [from, data] = *datagram;
            if (data.size() < 16 || ReadU32(data.data()) != NncsTestId) {
                continue;
            }
            bool known = false;
            for (const auto& target : targets) {
                known |= target == from;
            }
            if (known && answered.insert(from).second) {
                external_ports.insert(ReadU32(data.data() + 4));
            }
        }
    }
    outcome.reachable = !answered.empty();
    outcome.consistent = answered.size() >= 2 && external_ports.size() == 1;
    return outcome;
}

} // namespace

Result Run() {
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    Result result;

    const auto nncs = ProbeNextendo();
    result.nextendo = nncs.reachable ? NextendoReach::Reachable : NextendoReach::Unreachable;
    result.nextendo_consistent_port = nncs.consistent;

    UdpSocket socket;
    if (!socket.Valid()) {
        return result;
    }

    // Mapping: the public endpoint seen by every server we can reach, from one local socket.
    std::vector<Endpoint> mapped;
    std::optional<Endpoint> primary;
    std::optional<StunReply> primary_reply;
    for (std::size_t i = 0; i < StunServers.size(); ++i) {
        const auto ip = Resolve(StunServers[i].host);
        if (!ip) {
            continue;
        }
        const Endpoint server{*ip, StunServers[i].port};
        const auto reply = StunRequest(socket, server, 0);
        if (!reply) {
            continue;
        }
        mapped.push_back(reply->mapped);
        if (i == 0) {
            primary = server;
            primary_reply = reply;
        }
    }
    // RFC 5780 mapping test: same server, alternate IP.
    if (primary_reply && primary_reply->other) {
        const Endpoint alternate{primary_reply->other->ip, primary->port};
        if (const auto reply = StunRequest(socket, alternate, 0)) {
            mapped.push_back(reply->mapped);
        }
    }

    if (mapped.empty()) {
        result.type = NatType::Blocked;
        return result;
    }
    for (const auto& endpoint : mapped) {
        if (!(endpoint == mapped.front())) {
            result.type = NatType::Symmetric;
            return result;
        }
    }

    // Public IP on a local interface means no NAT at all.
    {
        char hostname[256]{};
        if (gethostname(hostname, sizeof(hostname)) == 0) {
            addrinfo hints{};
            hints.ai_family = AF_INET;
            addrinfo* list = nullptr;
            if (getaddrinfo(hostname, nullptr, &hints, &list) == 0) {
                for (addrinfo* it = list; it; it = it->ai_next) {
                    const u32 ip =
                        ntohl(reinterpret_cast<sockaddr_in*>(it->ai_addr)->sin_addr.s_addr);
                    if (ip == mapped.front().ip) {
                        result.type = NatType::OpenInternet;
                    }
                }
                freeaddrinfo(list);
            }
        }
        if (result.type == NatType::OpenInternet) {
            return result;
        }
    }

    // Filtering: ask the RFC 5780 server to answer from another IP+port, then another port.
    if (!primary || !primary_reply || !primary_reply->other) {
        result.type = NatType::ConeUnknownFiltering;
        return result;
    }
    if (StunRequest(socket, *primary, ChangeIp | ChangePort)) {
        result.type = NatType::FullCone;
    } else if (StunRequest(socket, *primary, ChangePort)) {
        result.type = NatType::RestrictedCone;
    } else {
        result.type = NatType::PortRestrictedCone;
    }
    return result;
}

const char* SwitchLetter(NatType type) {
    switch (type) {
    case NatType::OpenInternet:
    case NatType::FullCone:
        return "A";
    case NatType::RestrictedCone:
    case NatType::PortRestrictedCone:
        return "B";
    case NatType::ConeUnknownFiltering:
        return "A/B";
    case NatType::Symmetric:
        return "D";
    case NatType::Blocked:
        return "F";
    }
    return "?";
}

} // namespace Nextendo::NatTest
