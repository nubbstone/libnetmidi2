/*
    libnetmidi2 — mDNS / DNS-SD discovery contract (§4).

    §4.1: "If Discovery is enabled, then mDNS with DNS-SD shall be used to discover
    Hosts", and it "is optional but recommended and it is the only mechanism defined
    in this specification for automated discovery". Explicit host:port stays valid --
    everything here can be skipped, and `Platform::discovery` may stay null.

    WHAT IS AND IS NOT IN THIS FILE

    Real mDNS is a multicast socket, DNS record encoding, and a cache with TTL
    expiry. None of that belongs in a freestanding core, and it is not here. This
    file is the contract an ADAPTER implements -- Bonjour / NSNetService or JUCE's
    NetworkServiceDiscovery on macOS, Avahi on Linux, Zephyr's mDNS responder on the
    Teensy -- plus the parts the spec constrains that can actually be checked
    without a network: the service type, the TXT field limits, and the TTL ceiling.

    Those limits are worth enforcing here rather than trusting each adapter with
    them, because a name one byte too long does not fail loudly. It produces a
    record some resolvers accept and others silently drop, which is the worst way
    for an interop bug to present.

    WHAT A HOST PUBLISHES (§4.2-4.5)

        PTR    _midi2._udp.local.  ->  <Service Instance Name>
        SRV    <instance>._midi2._udp.local.  ->  <port> <hostname>.local.
        TXT    UMPEndpointName=<utf-8, <=98 bytes>
               ProductInstanceId=<ascii 32-126, <=42 bytes>
        A/AAAA <hostname>.local.  ->  the Host's addresses

    Clients do not advertise (§3.3); they browse.

    TWO NAMES, DO NOT CONFUSE THEM (§4.2)

    The Service Instance Name (PTR) is an internal identifier and "should not be
    displayed to the user"; it should be stable across power cycles, and the spec
    suggests building it from the Product Instance Id plus a model name. The
    UMP Endpoint Name (TXT) is the human-facing one. Showing the wrong one in a
    device picker is the obvious mistake, and nothing on the wire will object.
*/

#pragma once

#include <cstddef>
#include <cstdint>

namespace netmidi2
{

struct Endpoint;   // Platform.h

//==============================================================================
// §4.2: the registered service type, in the local domain.
constexpr char kServiceType[] = "_midi2._udp";
constexpr char kServiceDomain[] = "local";

// §4.4 Table 6 field limits. Enforced by the validators below.
constexpr std::size_t kMaxUmpEndpointNameBytes  = 98;   // UTF-8
constexpr std::size_t kMaxProductInstanceIdBytes = 42;  // ASCII 32..126

/*  §4.6: "a Host should not set its service TTL longer than one minute." The reason
    is failure, not tidiness: mDNS defines how a Host un-publishes itself on a clean
    shutdown, but "there are circumstances, such as a cable disconnect or a system
    crash, where this does not occur". The TTL is the only thing that retires a Host
    that vanished without saying goodbye. */
constexpr std::uint32_t kMaxServiceTtlSeconds = 60;

//==============================================================================
/*  A Host found on the network: its resolved address and port, plus the two TXT
    fields. Sized to the spec's limits with room for a terminator, so this is a
    plain value a caller can keep on the stack -- no allocation, nothing to free. */
struct DiscoveredHost
{
    char          address[64] = {};                              // from A / AAAA
    std::uint16_t port = 0;                                      // from SRV
    char          umpEndpointName[kMaxUmpEndpointNameBytes + 1] = {};
    char          productInstanceId[kMaxProductInstanceIdBytes + 1] = {};
};

//==============================================================================
// Validators. A Host should check its own identity before publishing it; an adapter
// may also use these to reject a malformed record it has received.

inline std::size_t cstrBytes (const char* s) noexcept
{
    std::size_t n = 0;
    while (s && s[n]) ++n;
    return n;
}

/*  §4.4: "the UMP Endpoint Name shall be encoded in UTF-8, and the length shall not
    exceed 98 bytes", and §5.3 requires strings "without a Byte Order Mark".

    Bytes, not characters -- a name well inside 98 glyphs can be well over 98 bytes
    once it leaves ASCII, and that is precisely the case nobody tests. We do not
    validate the UTF-8 encoding itself; that is the adapter's business and the OS
    resolver will have opinions of its own. */
inline bool isValidUmpEndpointName (const char* name) noexcept
{
    const std::size_t n = cstrBytes (name);
    if (n == 0 || n > kMaxUmpEndpointNameBytes)
        return false;

    // UTF-8 BOM (EF BB BF) is explicitly disallowed by §5.3.
    const unsigned char* u = reinterpret_cast<const unsigned char*> (name);
    if (n >= 3 && u[0] == 0xEF && u[1] == 0xBB && u[2] == 0xBF)
        return false;

    return true;
}

/*  §4.4: "The Product Instance Id shall be a set of ASCII characters in the ordinal
    range 32-126 only" and "should not exceed 42 bytes". Also §6.4: it is the same
    value the Invitation carries, so a Host that publishes one string and invites
    with another is advertising a device that does not answer to that name. */
inline bool isValidProductInstanceId (const char* id) noexcept
{
    const std::size_t n = cstrBytes (id);
    if (n == 0 || n > kMaxProductInstanceIdBytes)
        return false;

    for (std::size_t i = 0; i < n; ++i)
    {
        const unsigned char c = static_cast<unsigned char> (id[i]);
        if (c < 32 || c > 126)
            return false;
    }
    return true;
}

//==============================================================================
/*  mDNS / DNS-SD, injected like every other I/O (prime directive 3). Optional: a
    null `Platform::discovery` simply means explicit-address operation.

    Every method must be non-blocking. poll() is drained from the run loop the same
    way the socket is.
*/
enum class DiscoveryEvent : std::uint8_t
{
    found,   // a Host appeared, or its records changed
    lost,    // it said goodbye, or its TTL expired (§4.6)
};

class IDiscovery
{
public:
    virtual ~IDiscovery() = default;

    /*  Host role: publish PTR / SRV / TXT / A for this port (§4.2-4.5).

        `serviceInstanceName` is the PTR name -- internal, stable, not for display.
        `umpEndpointName` and `productInstanceId` become the TXT record, and must be
        the SAME strings the Session sends in its Invitation Reply (§4.4), or a peer
        that recalls devices by those fields will not recognise the one it dialled.

        The adapter is responsible for the TTL ceiling (kMaxServiceTtlSeconds). */
    virtual void advertise (const char* serviceInstanceName, std::uint16_t port,
                            const char* umpEndpointName, const char* productInstanceId) = 0;

    virtual void stopAdvertising() = 0;

    // Client role (§3.3: Clients browse, they do not advertise).
    virtual void startBrowsing() = 0;
    virtual void stopBrowsing() = 0;

    /*  Non-blocking. Returns one pending event per call, false when drained.

        `lost` matters as much as `found`: without it a picker accumulates hosts that
        are no longer there, and the spec's short TTL exists precisely so that a Host
        which crashed or was unplugged can be retired. On a `lost` event only the
        identifying fields are guaranteed meaningful. */
    virtual bool poll (DiscoveryEvent& kindOut, DiscoveredHost& hostOut) = 0;
};

} // namespace netmidi2
