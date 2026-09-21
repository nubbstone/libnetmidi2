/*
    nm2_bench — interop bench harness for libnetmidi2.

    Drives a real Session against a real peer on the network and reports, stage by
    stage, what the peer actually did. This is the tool that answers the one question
    the test suite structurally cannot: the loopback runs our code against our own
    code, so a symmetric misreading of the spec passes. Here the other end is somebody
    else's implementation.

    Usage:
      nm2_bench client <host> <port> [options]     invite a Host and talk to it
      nm2_bench host   <port>       [options]      listen for an incoming Invitation
      nm2_bench browse                             list _midi2._udp peers (via dns-sd)

    Options:
      --name <s>        our UMP Endpoint Name      (default "libnetmidi2 bench")
      --pid <s>         our Product Instance Id    (default "LIBNETMIDI2-BENCH-1")
      --secret <s>      shared secret for authentication (§6.7)
      --seconds <n>     how long to run after Established  (default 5)
      --probe           send a UMP Stream Endpoint Discovery -- makes NO sound
      --note            send a Note On/Off -- WILL make sound on a synth
      --quiet           don't hexdump datagrams

    Build: see CMakeLists.txt (target nm2_bench), or
      clang++ -std=c++17 -Iinclude tools/nm2_bench.cpp -o nm2_bench
*/

#include "netmidi2/Session.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <cstdlib>

using namespace netmidi2;

static bool verbose = true;

//== platform adapters ========================================================
/*  Unlike the loopback test's adapter this binds INADDR_ANY, not INADDR_LOOPBACK:
    the peer is on the LAN. It also hexdumps, because when two implementations
    disagree the bytes are the only impartial witness. */
struct BenchUdp : IUdpSocket
{
    int fd = -1;
    unsigned long long txDatagrams = 0, rxDatagrams = 0;

    /*  Dual-stack, and that is not a nicety.
        A macOS Host advertises its hostname over mDNS, and `<host>.local` resolves to
        BOTH families -- with the IPv6 records listed first. A peer that dials us by
        name therefore tends to arrive over IPv6, and an AF_INET socket simply never
        sees the Invitation: no error, no packet, just a peer reporting that we did
        not answer. This was found exactly that way, with macOS Tahoe's own client
        timing out against an IPv4-only build of this tool.

        So: one AF_INET6 socket with IPV6_V6ONLY off, which receives both families.
        IPv4 peers arrive as ::ffff:a.b.c.d and are normalised back to dotted quad, so
        that the address string the Session stores still compares equal to the one a
        user typed -- Endpoint identity is a string comparison, and "203.0.113.5" and
        "::ffff:203.0.113.5" are the same peer wearing two hats. */
    bool bind (std::uint16_t desiredPort, std::uint16_t& boundPortOut) override
    {
        fd = ::socket (AF_INET6, SOCK_DGRAM, 0);
        if (fd < 0)
            return false;

        int off = 0;
        ::setsockopt (fd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof off);
        int yes = 1; ::setsockopt (fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
        ::fcntl (fd, F_SETFL, O_NONBLOCK);

        sockaddr_in6 a {};
        a.sin6_family = AF_INET6;
        a.sin6_addr   = in6addr_any;
        a.sin6_port   = htons (desiredPort);
        if (::bind (fd, (sockaddr*) &a, sizeof a) != 0)
            return false;

        sockaddr_in6 got {}; socklen_t gl = sizeof got;
        ::getsockname (fd, (sockaddr*) &got, &gl);
        boundPortOut = ntohs (got.sin6_port);
        return true;
    }

    int send (const Endpoint& to, const std::uint8_t* d, std::size_t len) override
    {
        sockaddr_in6 a {};
        a.sin6_family = AF_INET6;
        a.sin6_port   = htons (to.port);

        if (std::strchr (to.address, ':') == nullptr)
        {
            // IPv4 literal -> ::ffff:a.b.c.d so it can go out of the v6 socket.
            in_addr v4 {};
            if (::inet_pton (AF_INET, to.address, &v4) != 1) return -1;
            a.sin6_addr.s6_addr[10] = 0xFF;
            a.sin6_addr.s6_addr[11] = 0xFF;
            std::memcpy (&a.sin6_addr.s6_addr[12], &v4, 4);
        }
        else
        {
            /*  A literal v6 address, possibly carrying a %scope for a link-local one.
                getaddrinfo with AI_NUMERICHOST parses the scope; inet_pton cannot,
                and link-local is exactly what a .local peer on the LAN gives us. */
            addrinfo hints {};
            hints.ai_family   = AF_INET6;
            hints.ai_socktype = SOCK_DGRAM;
            hints.ai_flags    = AI_NUMERICHOST;
            addrinfo* res = nullptr;
            if (::getaddrinfo (to.address, nullptr, &hints, &res) != 0 || res == nullptr)
                return -1;
            auto* r = (sockaddr_in6*) res->ai_addr;
            a.sin6_addr     = r->sin6_addr;
            a.sin6_scope_id = r->sin6_scope_id;
            ::freeaddrinfo (res);
        }

        const int n = (int) ::sendto (fd, d, len, 0, (sockaddr*) &a, sizeof a);
        if (n > 0) { ++txDatagrams; dump ("TX ->", to, d, len); }
        return n;
    }

    int receive (std::uint8_t* buf, std::size_t cap, Endpoint& from) override
    {
        sockaddr_storage ss {}; socklen_t sl = sizeof ss;
        const ssize_t n = ::recvfrom (fd, buf, cap, 0, (sockaddr*) &ss, &sl);
        if (n < 0) return 0;                       // nothing pending, not an error

        if (ss.ss_family == AF_INET6)
        {
            auto* a6 = (sockaddr_in6*) &ss;
            if (IN6_IS_ADDR_V4MAPPED (&a6->sin6_addr))
            {
                in_addr v4 {};
                std::memcpy (&v4, &a6->sin6_addr.s6_addr[12], 4);
                ::inet_ntop (AF_INET, &v4, from.address, sizeof from.address);
            }
            else
            {
                ::inet_ntop (AF_INET6, &a6->sin6_addr, from.address, sizeof from.address);
                if (a6->sin6_scope_id != 0)        // link-local needs its interface
                {
                    char scope[16];
                    std::snprintf (scope, sizeof scope, "%%%u", a6->sin6_scope_id);
                    const std::size_t have = std::strlen (from.address);
                    if (have + std::strlen (scope) < sizeof from.address)
                        std::strcat (from.address, scope);
                }
            }
            from.port = ntohs (a6->sin6_port);
        }
        else
        {
            auto* a4 = (sockaddr_in*) &ss;
            ::inet_ntop (AF_INET, &a4->sin_addr, from.address, sizeof from.address);
            from.port = ntohs (a4->sin_port);
        }

        ++rxDatagrams;
        dump ("RX <-", from, buf, (std::size_t) n);
        return (int) n;
    }

    static void dump (const char* dir, const Endpoint& ep,
                      const std::uint8_t* d, std::size_t len)
    {
        if (! verbose) return;
        printf ("  %s %s:%u  %zu bytes\n     ", dir, ep.address, ep.port, len);
        const std::size_t show = len < 64 ? len : 64;
        for (std::size_t i = 0; i < show; ++i)
        {
            printf ("%02X", d[i]);
            if ((i & 3) == 3) printf (" ");
        }
        if (show < len) printf ("... (+%zu)", len - show);
        // Decode the command code of the first command, the useful bit at a glance.
        if (len >= 8) printf ("   [cmd 0x%02X]", d[4]);
        printf ("\n");
    }
};

struct BenchClock : IClock
{
    std::uint32_t nowMs() override
    { timeval tv; ::gettimeofday (&tv, nullptr);
      return std::uint32_t (tv.tv_sec * 1000ull + tv.tv_usec / 1000); }
};

static const char* stateName (State s)
{
    switch (s)
    {
        case State::idle:           return "idle";
        case State::inviting:       return "inviting";
        case State::authenticating: return "authenticating";
        case State::established:    return "established";
        case State::resetting:      return "resetting";
        case State::closing:        return "closing";
        case State::closed:         return "closed";
    }
    return "?";
}

struct BenchListener : ISessionListener
{
    int  umpMessages = 0, umpWords = 0, lost = 0, resets = 0;
    bool everEstablished = false, everClosed = false;

    void onUmpReceived (const std::uint32_t* words, std::uint8_t count) override
    {
        ++umpMessages; umpWords += count;
        printf ("  << UMP  %u word(s):", count);
        for (std::uint8_t i = 0; i < count; ++i) printf (" %08X", words[i]);
        // Message Type is the top nibble -- enough to say what class of thing it is.
        if (count) 
        {
            const unsigned mt = words[0] >> 28;
            const char* kind = mt==0x0?"Utility" : mt==0x1?"System RT" : mt==0x2?"MIDI1 ch"
                             : mt==0x3?"Data64" : mt==0x4?"MIDI2 ch" : mt==0x5?"Data128"
                             : mt==0xD?"Flex" : mt==0xF?"UMP Stream" : "other";
            printf ("   (mt=0x%X %s)", mt, kind);
        }
        printf ("\n");
    }
    void onStateChanged (State s) override
    {
        printf ("  ** state -> %s\n", stateName (s));
        if (s == State::established) everEstablished = true;
        if (s == State::closed)      everClosed = true;
    }
    void onUmpLost (std::uint16_t seq) override
    { ++lost; printf ("  !! UMP lost, sequence %u\n", seq); }
    void onSessionReset() override
    { ++resets; printf ("  ** session reset\n"); }
};

//== helpers ==================================================================
static bool resolveHost (const char* host, char out[64])
{
    addrinfo hints {}; hints.ai_family = AF_INET; hints.ai_socktype = SOCK_DGRAM;
    addrinfo* res = nullptr;
    if (::getaddrinfo (host, nullptr, &hints, &res) != 0 || res == nullptr) return false;
    auto* a = (sockaddr_in*) res->ai_addr;
    ::inet_ntop (AF_INET, &a->sin_addr, out, 64);
    ::freeaddrinfo (res);
    return true;
}

int main (int argc, char** argv)
{
    if (argc < 2)
    { std::puts ("usage: nm2_bench client <host> <port> | host <port> | browse"); return 2; }

    const char* mode = argv[1];

    if (std::strcmp (mode, "browse") == 0)
    {
        std::puts ("Browsing _midi2._udp for 5s (dns-sd)...");
        return std::system ("dns-sd -Z _midi2._udp local & P=$!; sleep 5; kill $P 2>/dev/null");
    }

    const bool isClient = std::strcmp (mode, "client") == 0;
    const bool isHost   = std::strcmp (mode, "host") == 0;
    if (! isClient && ! isHost)
    { std::puts ("first arg must be client, host or browse"); return 2; }

    const char* name    = "libnetmidi2 bench";
    const char* pid     = "LIBNETMIDI2-BENCH-1";
    const char* secret  = nullptr;
    int  seconds = 5;
    bool probe = false, note = false;

    const char* peerHost = nullptr;
    std::uint16_t peerPort = 0, listenPort = 0;
    int argi = 2;

    if (isClient)
    {
        if (argc < 4) { std::puts ("client needs <host> <port>"); return 2; }
        peerHost = argv[2]; peerPort = (std::uint16_t) std::atoi (argv[3]); argi = 4;
    }
    else
    {
        if (argc < 3) { std::puts ("host needs <port>"); return 2; }
        listenPort = (std::uint16_t) std::atoi (argv[2]); argi = 3;
    }

    for (; argi < argc; ++argi)
    {
        const char* a = argv[argi];
        auto next = [&] () -> const char* { return argi + 1 < argc ? argv[++argi] : ""; };
        if      (! std::strcmp (a, "--name"))    name   = next();
        else if (! std::strcmp (a, "--pid"))     pid    = next();
        else if (! std::strcmp (a, "--secret"))  secret = next();
        else if (! std::strcmp (a, "--seconds")) seconds = std::atoi (next());
        else if (! std::strcmp (a, "--probe"))   probe = true;
        else if (! std::strcmp (a, "--note"))    note  = true;
        else if (! std::strcmp (a, "--quiet"))   verbose = false;
        else { printf ("unknown option %s\n", a); return 2; }
    }

    BenchUdp sock; BenchClock clock;
    std::uint16_t bound = 0;
    if (! sock.bind (listenPort, bound)) { std::puts ("FAIL: bind"); return 1; }

    Platform plat { &sock, &clock, nullptr, nullptr };
    BenchListener listener;
    Session session (plat, isClient ? Role::client : Role::host, &listener, name, pid);

    if (secret != nullptr)
    {
        if (isClient) session.setSharedSecret (secret);
        else          session.requireAuthentication (secret);
    }

    printf ("=== nm2_bench ===\n");
    printf ("role        : %s\n", isClient ? "Client" : "Host");
    printf ("local port  : %u\n", bound);
    printf ("endpoint    : \"%s\"  pid \"%s\"\n", name, pid);
    if (secret) printf ("auth        : shared secret supplied\n");

    if (isClient)
    {
        Endpoint ep {};
        if (! resolveHost (peerHost, ep.address))
        { printf ("FAIL: cannot resolve %s\n", peerHost); return 1; }
        ep.port = peerPort;
        printf ("peer        : %s -> %s:%u\n\n", peerHost, ep.address, ep.port);
        session.connect (ep);
    }
    else
    {
        printf ("waiting for an Invitation on :%u\n\n", bound);
        session.listen();
    }

    BenchClock c; const std::uint32_t start = c.nowMs();
    const std::uint32_t handshakeBudgetMs = 12000;
    std::uint32_t establishedAt = 0;
    bool probed = false, closing = false;

    while (true)
    {
        session.tick();
        usleep (2000);
        const std::uint32_t now = c.nowMs();

        if (session.state() == State::established && establishedAt == 0)
        {
            establishedAt = now;
            printf ("\n--- Established after %u ms ---\n", now - start);

            if (probe)
            {
                /*  UMP Stream "Endpoint Discovery" (mt=0xF, status 0x00), asking for
                    an Endpoint Info Notification. Chosen deliberately: it is a legal
                    UMP that a conformant MIDI 2.0 endpoint answers, and it cannot
                    make a sound on somebody's live rig. */
                const std::uint32_t disco[4] = { 0xF0000101u, 0x00000001u, 0, 0 };
                printf ("  >> UMP Stream Endpoint Discovery (silent probe)\n");
                if (! session.sendUmp (disco, 4)) printf ("  !! sendUmp refused\n");
                probed = true;
            }
            if (note)
            {
                /*  MIDI 2.0 Channel Voice Note On/Off, group 0 channel 0, note 60.
                    This WILL sound on a synth -- opt-in only. */
                const std::uint32_t on[2]  = { 0x40903C00u, 0x7FFF0000u };
                printf ("  >> UMP MIDI2 Note On  (audible)\n");
                session.sendUmp (on, 2);
            }
        }

        if (establishedAt != 0 && ! closing
            && now - establishedAt >= (std::uint32_t) seconds * 1000u)
        {
            if (note)
            {
                const std::uint32_t off[2] = { 0x40803C00u, 0x00000000u };
                printf ("  >> UMP MIDI2 Note Off\n");
                session.sendUmp (off, 2);
                for (int i = 0; i < 50; ++i) { session.tick(); usleep (2000); }
            }
            printf ("\n--- closing (sending Bye) ---\n");
            session.close();
            closing = true;
        }

        if (session.state() == State::closed) break;

        if (establishedAt == 0 && now - start > handshakeBudgetMs)
        { printf ("\n--- handshake budget expired ---\n"); break; }

        if (closing && now - establishedAt > (std::uint32_t) seconds * 1000u + 6000u)
        { printf ("\n--- close budget expired ---\n"); break; }
    }

    printf ("\n=== summary ===\n");
    printf ("established     : %s\n", listener.everEstablished ? "YES" : "no");
    printf ("clean close     : %s\n", listener.everClosed ? "YES (reached closed)" : "no");
    printf ("final state     : %s\n", stateName (session.state()));
    printf ("datagrams tx/rx : %llu / %llu\n", sock.txDatagrams, sock.rxDatagrams);
    printf ("UMP msgs rx     : %d  (%d words)\n", listener.umpMessages, listener.umpWords);
    if (probed) printf ("silent probe    : sent; %s\n",
                        listener.umpMessages ? "peer sent UMP back" : "no UMP came back");
    printf ("ump lost        : %d\nsession resets  : %d\n", listener.lost, listener.resets);

    return listener.everEstablished ? 0 : 1;
}
