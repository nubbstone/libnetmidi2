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

    bool bind (std::uint16_t desiredPort, std::uint16_t& boundPortOut) override
    {
        fd = ::socket (AF_INET, SOCK_DGRAM, 0);
        if (fd < 0) return false;
        ::fcntl (fd, F_SETFL, O_NONBLOCK);
        int yes = 1; ::setsockopt (fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);

        sockaddr_in a {}; a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl (INADDR_ANY);
        a.sin_port = htons (desiredPort);
        if (::bind (fd, (sockaddr*) &a, sizeof a) != 0) return false;

        sockaddr_in got {}; socklen_t gl = sizeof got;
        ::getsockname (fd, (sockaddr*) &got, &gl);
        boundPortOut = ntohs (got.sin_port);
        return true;
    }

    int send (const Endpoint& to, const std::uint8_t* data, std::size_t len) override
    {
        sockaddr_in a {}; a.sin_family = AF_INET; a.sin_port = htons (to.port);
        if (::inet_pton (AF_INET, to.address, &a.sin_addr) != 1) return -1;
        const int n = (int) ::sendto (fd, data, len, 0, (sockaddr*) &a, sizeof a);
        if (n > 0) { ++txDatagrams; dump ("TX ->", to, data, len); }
        return n;
    }

    int receive (std::uint8_t* buffer, std::size_t capacity, Endpoint& from) override
    {
        sockaddr_in a {}; socklen_t al = sizeof a;
        const ssize_t n = ::recvfrom (fd, buffer, capacity, 0, (sockaddr*) &a, &al);
        if (n < 0) return 0;
        ::inet_ntop (AF_INET, &a.sin_addr, from.address, sizeof from.address);
        from.port = ntohs (a.sin_port);
        ++rxDatagrams; dump ("RX <-", from, buffer, (std::size_t) n);
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
