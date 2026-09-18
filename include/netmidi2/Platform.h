/*
    libnetmidi2 — platform abstraction.

    The protocol core is OS-agnostic; the host injects concrete I/O. macOS supplies
    these with JUCE (DatagramSocket, NetworkServiceDiscovery, Time); Zephyr supplies
    them with its BSD sockets, k_uptime, and mDNS responder. Keep implementations
    non-blocking.
*/

#pragma once

#include <cstddef>
#include <cstdint>

#include "Auth.h"
#include "Discovery.h"

namespace netmidi2
{

// An IPv4/IPv6 peer address + UDP port. Kept opaque-ish so platforms can store
// whatever they need (here: a printable address string + port for portability).
struct Endpoint
{
    char        address[64] = {}; // numeric IP or hostname, null-terminated
    std::uint16_t port = 0;
    bool operator== (const Endpoint& o) const noexcept
    {
        std::size_t i = 0;
        for (; address[i] && o.address[i]; ++i)
            if (address[i] != o.address[i]) return false;
        return address[i] == o.address[i] && port == o.port;
    }
};

//==============================================================================
// Non-blocking UDP socket. send/recv operate on whole datagrams.
class IUdpSocket
{
public:
    virtual ~IUdpSocket() = default;

    // Bind to a local UDP port (0 = OS-assigned). Returns the bound port, or 0 on
    // failure. Populate `boundPortOut` with the actual port for mDNS advertising.
    virtual bool bind (std::uint16_t desiredPort, std::uint16_t& boundPortOut) = 0;

    // Send one datagram to `to`. Returns bytes sent, or -1 on error.
    virtual int send (const Endpoint& to, const std::uint8_t* data, std::size_t len) = 0;

    // Non-blocking receive of one datagram. Returns bytes read (0 if none pending,
    // -1 on error) and fills `from` with the sender.
    virtual int receive (std::uint8_t* buffer, std::size_t capacity, Endpoint& from) = 0;
};

//==============================================================================
// Monotonic millisecond clock for ping/keepalive/timeout.
class IClock
{
public:
    virtual ~IClock() = default;
    virtual std::uint32_t nowMs() = 0;
};

//==============================================================================
// IDiscovery (mDNS / DNS-SD for `_midi2._udp`) lives in Discovery.h, with the §4
// field limits it has to respect. Optional: leave `discovery` null and drive the
// library with explicit host:port.

//==============================================================================
// Everything the Session needs from the platform, bundled.
struct Platform
{
    IUdpSocket* socket    = nullptr;
    IClock*     clock     = nullptr;
    IDiscovery* discovery = nullptr; // null = explicit-address mode, no mDNS
    ICrypto*    crypto    = nullptr; // null = no authentication (Auth.h)
};

} // namespace netmidi2
