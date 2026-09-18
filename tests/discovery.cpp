/*
    libnetmidi2 — mDNS / DNS-SD discovery (§4).

    Real mDNS is not exercised here and cannot be: it needs multicast, a responder,
    and a cache with TTL expiry, none of which belong in this library (see
    Discovery.h). What IS testable without a network is everything the spec
    constrains -- the TXT field limits, and the join between discovery and the
    session handshake -- plus the orchestration, driven through a fake adapter that
    stands in for Bonjour or Zephyr's responder.

    The check that matters most is the last one. §4.4 says the advertised
    UMPEndpointName "shall be the same name as the UMP Endpoint Name used in the
    Invitation Reply Commands". Nothing on the wire enforces that: a Host can publish
    one identity and hand out another, discovery still works, the session still
    establishes, and the bug only shows up as a peer that cannot match the device it
    just dialled to the one it thought it was dialling. So the test resolves a Host
    by browsing, connects to it, and compares the advertised strings against the ones
    that actually arrive in the Invitation Reply.
*/

#include "netmidi2/HostPort.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>

using namespace netmidi2;

//== POSIX adapters ============================================================
struct PosixUdp : IUdpSocket
{
    int fd = -1;
    bool bind (std::uint16_t desiredPort, std::uint16_t& boundPortOut) override
    {
        fd = ::socket (AF_INET, SOCK_DGRAM, 0);
        if (fd < 0) return false;
        ::fcntl (fd, F_SETFL, O_NONBLOCK);
        sockaddr_in a {}; a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl (INADDR_LOOPBACK);
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
        ::inet_pton (AF_INET, to.address, &a.sin_addr);
        return (int) ::sendto (fd, data, len, 0, (sockaddr*) &a, sizeof a);
    }
    int receive (std::uint8_t* buffer, std::size_t capacity, Endpoint& from) override
    {
        sockaddr_in a {}; socklen_t al = sizeof a;
        const ssize_t n = ::recvfrom (fd, buffer, capacity, 0, (sockaddr*) &a, &al);
        if (n < 0) return 0;
        ::inet_ntop (AF_INET, &a.sin_addr, from.address, sizeof from.address);
        from.port = ntohs (a.sin_port);
        return (int) n;
    }
};

struct PosixClock : IClock
{
    std::uint32_t nowMs() override
    { timeval tv; ::gettimeofday (&tv, nullptr); return std::uint32_t (tv.tv_sec * 1000ull + tv.tv_usec / 1000); }
};

/*  Stands in for Bonjour / Avahi / Zephyr's responder. Records exactly what a Host
    asked to publish, and lets the test hand a Client the records a browse would
    have produced -- including a `lost` event, which no loopback could stage. */
struct FakeDiscovery : IDiscovery
{
    // advertise() side
    bool          advertising = false;
    int           advertiseCalls = 0;
    char          instanceName[128] = {};
    std::uint16_t port = 0;
    char          txtName[128] = {};
    char          txtProductId[128] = {};

    // browse() side
    bool           browsing = false;
    DiscoveryEvent queuedKind = DiscoveryEvent::found;
    DiscoveredHost queued {};
    bool           hasQueued = false;

    void advertise (const char* n, std::uint16_t p, const char* name, const char* pid) override
    {
        ++advertiseCalls; advertising = true; port = p;
        std::snprintf (instanceName, sizeof instanceName, "%s", n ? n : "");
        std::snprintf (txtName,      sizeof txtName,      "%s", name ? name : "");
        std::snprintf (txtProductId, sizeof txtProductId, "%s", pid ? pid : "");
    }
    void stopAdvertising() override { advertising = false; }
    void startBrowsing() override { browsing = true; }
    void stopBrowsing() override  { browsing = false; }

    bool poll (DiscoveryEvent& kindOut, DiscoveredHost& hostOut) override
    {
        if (! hasQueued) return false;
        kindOut = queuedKind; hostOut = queued; hasQueued = false;
        return true;
    }

    void publishToBrowser (DiscoveryEvent k, const DiscoveredHost& h)
    { queuedKind = k; queued = h; hasQueued = true; }
};

struct Recorder : ISessionListener
{
    int umpCount = 0;
    void onUmpReceived (const std::uint32_t*, std::uint8_t) override { ++umpCount; }
};

static int checks = 0, passes = 0;
static void check (bool ok, const char* label)
{ ++checks; if (ok) ++passes; printf ("  %s  %s\n", ok ? "OK  " : "FAIL", label); }

int main()
{
    setvbuf (stdout, nullptr, _IONBF, 0);
    std::puts ("mDNS / DNS-SD discovery (spec 4)\n");

    //-- the constants an adapter has to honour ------------------------------
    std::puts ("Service definition");
    check (std::strcmp (kServiceType, "_midi2._udp") == 0, "service type is _midi2._udp (4.2)");
    check (kMaxUmpEndpointNameBytes == 98,   "UMPEndpointName limit is 98 bytes (4.4)");
    check (kMaxProductInstanceIdBytes == 42, "ProductInstanceId limit is 42 bytes (4.4)");
    check (kMaxServiceTtlSeconds == 60,      "service TTL ceiling is 60s (4.6)");

    //-- identity validation --------------------------------------------------
    std::puts ("\nTXT field validation (4.4)");
    {
        check (isValidUmpEndpointName ("M2 SoundGen Host"), "a normal name is accepted");
        check (! isValidUmpEndpointName (""), "an empty name is rejected");

        char tooLong[kMaxUmpEndpointNameBytes + 2];
        std::memset (tooLong, 'x', sizeof tooLong - 1);
        tooLong[sizeof tooLong - 1] = '\0';
        check (! isValidUmpEndpointName (tooLong), "99 bytes is rejected");

        char exact[kMaxUmpEndpointNameBytes + 1];
        std::memset (exact, 'x', kMaxUmpEndpointNameBytes);
        exact[kMaxUmpEndpointNameBytes] = '\0';
        check (isValidUmpEndpointName (exact), "exactly 98 bytes is accepted");

        // The trap: a name well inside 98 CHARACTERS but over 98 BYTES once it
        // leaves ASCII. Counting glyphs instead of bytes passes this and then
        // produces a record some resolvers drop.
        char utf8[128] = {};
        for (int i = 0; i < 34; ++i) std::strcat (utf8, "\xE2\x99\xAA");   // 34 x 3 bytes
        check (std::strlen (utf8) == 102, "...a 34-glyph UTF-8 name is 102 bytes");
        check (! isValidUmpEndpointName (utf8), "...and is rejected on BYTES, not glyphs");

        check (! isValidUmpEndpointName ("\xEF\xBB\xBF" "Named"), "a UTF-8 BOM is rejected (5.3)");

        check (isValidProductInstanceId ("NUBBSOFT-HOST-1"), "a normal product id is accepted");
        check (! isValidProductInstanceId (""), "an empty product id is rejected");
        check (! isValidProductInstanceId ("has space\x01ctrl"), "a control character is rejected");
        check (! isValidProductInstanceId ("caf\xC3\xA9"), "non-ASCII is rejected (32-126 only)");

        char pidLong[kMaxProductInstanceIdBytes + 2];
        std::memset (pidLong, 'A', sizeof pidLong - 1);
        pidLong[sizeof pidLong - 1] = '\0';
        check (! isValidProductInstanceId (pidLong), "43 bytes is rejected");
    }

    //-- a Host publishes its port -------------------------------------------
    std::puts ("\nHost advertises (4.2-4.5)");
    PosixClock clock;
    PosixUdp hostSock;
    FakeDiscovery hostDisco;
    std::uint16_t hostPort = 0;
    check (hostSock.bind (0, hostPort), "host socket bound");

    Platform hostPlat { &hostSock, &clock, &hostDisco };
    Recorder slotRec;
    const char* kHostName = "M2 SoundGen Host";
    const char* kHostPid  = "NUBBSOFT-HOST-1";
    // Two slots sharing ONE identity: N connections to one Host, not N Hosts
    // (§3.2). The second is what lets the probe below get a real Invitation Reply
    // while the discovered Client keeps its session -- with one slot the Host is
    // full and correctly answers Bye 0x40 instead, which is host_multiclient's job.
    Recorder slot1Rec;
    Session slot0 (hostPlat, Role::host, &slotRec,  kHostName, kHostPid);
    Session slot1 (hostPlat, Role::host, &slot1Rec, kHostName, kHostPid);
    Session* slots[] = { &slot0, &slot1 };
    HostPort port (hostPlat, slots, 2);
    port.listen();

    check (! port.advertise ("inst", 0, kHostName, kHostPid),
           "advertising port 0 is refused (bind() reports the real port)");
    check (! port.advertise ("inst", hostPort, "", kHostPid),
           "an invalid UMPEndpointName is refused");
    check (! port.advertise ("inst", hostPort, kHostName, "bad\x01id"),
           "an invalid ProductInstanceId is refused");
    check (hostDisco.advertiseCalls == 0, "...and none of those reached the adapter");

    check (port.advertise ("NUBBSOFT1-SoundGen", hostPort, kHostName, kHostPid),
           "a valid identity is published");
    check (hostDisco.advertising && hostDisco.advertiseCalls == 1, "the adapter was asked once");
    check (hostDisco.port == hostPort, "...with the port the socket actually bound");
    check (std::strcmp (hostDisco.txtName, kHostName) == 0, "...UMPEndpointName in TXT");
    check (std::strcmp (hostDisco.txtProductId, kHostPid) == 0, "...ProductInstanceId in TXT");
    check (std::strcmp (hostDisco.instanceName, "NUBBSOFT1-SoundGen") == 0,
           "...and the PTR instance name kept distinct from the display name");

    //-- a Client browses and connects ----------------------------------------
    std::puts ("\nClient browses, then invites what it found");
    PosixUdp clientSock;
    FakeDiscovery clientDisco;
    std::uint16_t clientPort = 0;
    clientSock.bind (0, clientPort);
    Platform clientPlat { &clientSock, &clock, &clientDisco };
    Recorder clientRec;
    Session client (clientPlat, Role::client, &clientRec, "Teensy Zephyr M2", "TEENSY-CLIENT-1");

    clientDisco.startBrowsing();
    check (clientDisco.browsing, "the client is browsing");

    // What a real browse would have resolved: SRV port + A address + the TXT pair.
    DiscoveredHost found {};
    std::snprintf (found.address, sizeof found.address, "127.0.0.1");
    found.port = hostPort;
    std::snprintf (found.umpEndpointName, sizeof found.umpEndpointName, "%s", hostDisco.txtName);
    std::snprintf (found.productInstanceId, sizeof found.productInstanceId, "%s", hostDisco.txtProductId);
    clientDisco.publishToBrowser (DiscoveryEvent::found, found);

    DiscoveryEvent kind {}; DiscoveredHost got {};
    check (clientDisco.poll (kind, got) && kind == DiscoveryEvent::found, "a host is discovered");
    check (! clientDisco.poll (kind, got), "...and poll() drains, rather than repeating");

    client.connect (got);
    auto pump = [&] (int n) { for (int i = 0; i < n; ++i) { port.tick(); client.tick(); usleep (500); } };
    for (int i = 0; i < 600 && client.state() != State::established; ++i) pump (1);
    check (client.state()==State::established, "the discovered address establishes a session");

    std::uint32_t note[2] = { 0x40903C00u, 0xFFFF0000u };
    client.sendUmp (note, 2); pump (80);
    check (slotRec.umpCount + slot1Rec.umpCount == 1, "...and UMP flows over it");

    //-- the join: advertised identity == handshake identity (4.4) ------------
    std::puts ("\nAdvertised identity matches the handshake (4.4)");
    {
        // Read the Invitation Reply the Host actually sent, straight off the wire.
        PosixUdp probe; std::uint16_t pPort = 0; probe.bind (0, pPort);
        Endpoint hostEp {}; std::strcpy (hostEp.address, "127.0.0.1"); hostEp.port = hostPort;

        std::uint8_t b[256]; Writer w (b, sizeof b);
        const char* pn = "Prober"; const char* pp = "PROBE-1";
        w.writeSignature();
        writeInvitation (w, 0, pn, std::strlen (pn), pp, std::strlen (pp));
        probe.send (hostEp, b, w.size());
        for (int i = 0; i < 300; ++i) { port.tick(); usleep (500); }

        char replyName[128] = {}, replyPid[128] = {};
        bool sawAccepted = false;
        std::uint8_t in[512]; Endpoint from; int n = 0;
        while ((n = probe.receive (in, sizeof in, from)) > 0)
            parseDatagram (in, std::size_t (n), [&] (const ParsedCommand& c) {
                if (c.code != Command::invitationReplyAccepted || ! c.payload) return;
                sawAccepted = true;
                // csd1 = name length in words; the product id follows it.
                const std::size_t nameWords = c.data1();
                const std::size_t nameBytes = nameWords * 4;
                std::size_t i2 = 0;
                for (; i2 < nameBytes && c.payload[i2] && i2 + 1 < sizeof replyName; ++i2)
                    replyName[i2] = char (c.payload[i2]);
                replyName[i2] = '\0';
                const std::size_t pidBytes = std::size_t (c.payloadWords) * 4 - nameBytes;
                std::size_t j = 0;
                for (; j < pidBytes && c.payload[nameBytes + j] && j + 1 < sizeof replyPid; ++j)
                    replyPid[j] = char (c.payload[nameBytes + j]);
                replyPid[j] = '\0';
            });

        check (sawAccepted, "the host answered with an Invitation Reply: Accepted");
        check (std::strcmp (replyName, hostDisco.txtName) == 0,
               "the name in the Reply is the name in the TXT record");
        check (std::strcmp (replyPid, hostDisco.txtProductId) == 0,
               "the product id in the Reply is the one in the TXT record");
    }

    //-- hosts go away, and a browser must be told ----------------------------
    std::puts ("\nA Host that vanishes (4.6)");
    {
        DiscoveredHost gone {};
        std::snprintf (gone.address, sizeof gone.address, "127.0.0.1");
        gone.port = hostPort;
        std::snprintf (gone.umpEndpointName, sizeof gone.umpEndpointName, "%s", kHostName);
        clientDisco.publishToBrowser (DiscoveryEvent::lost, gone);

        DiscoveryEvent k {}; DiscoveredHost h {};
        check (clientDisco.poll (k, h) && k == DiscoveryEvent::lost,
               "a lost event is delivered, so a picker can retire the entry");
        check (client.state()==State::established,
               "...and losing the ADVERTISEMENT does not close a live session");
    }

    //-- teardown --------------------------------------------------------------
    std::puts ("\nWithdrawing the advertisement");
    port.stopAdvertising();
    check (! hostDisco.advertising && ! port.isAdvertising(), "the host stops advertising");
    check (client.state()==State::established, "...and the existing session survives it");

    printf ("\n%s: discovery (%d/%d)\n", passes == checks ? "PASS" : "FAIL", passes, checks);
    return passes == checks ? 0 : 1;
}
