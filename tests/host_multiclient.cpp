/*
    libnetmidi2 — one Host UDP port serving several Clients (§3.2).

    "A Host shares its UDP port with all Clients... the Host shall uniquely identify
    the connection for each Client via the Client's source IP address and UDP port
    number of the incoming UDP packets."

    A bare Session holds one peer, so a Host built from a single Session answers the
    first Client to invite it and ignores every other one. That matters because a
    Host advertises ONE port over mDNS (§4.3): every Client that discovers it aims at
    that port, and the losers sit at `inviting` against a host that looks healthy.
    HostPort owns the socket and routes by sender so the slots can each hold a
    conversation.

    Everything here runs over real localhost UDP, like session_loopback.
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

struct Recorder : ISessionListener
{
    const char*   who;
    int           umpCount = 0;
    std::uint32_t lastWord0 = 0;
    explicit Recorder (const char* w) : who (w) {}
    void onUmpReceived (const std::uint32_t* words, std::uint8_t count) override
    { ++umpCount; lastWord0 = count ? words[0] : 0; }
};

static int checks = 0, passes = 0;
static void check (bool ok, const char* label)
{ ++checks; if (ok) ++passes; printf ("  %s  %s\n", ok ? "OK  " : "FAIL", label); }

int main()
{
    setvbuf (stdout, nullptr, _IONBF, 0);   // unbuffered: survive a crash
    std::puts ("One Host port, many Clients (spec 3.2)\n");

    PosixClock clock;
    PosixUdp hostSock, aSock, bSock, cSock;
    std::uint16_t hPort = 0, aPort = 0, bPort = 0, cPort = 0;
    check (hostSock.bind (0, hPort) && aSock.bind (0, aPort)
             && bSock.bind (0, bPort) && cSock.bind (0, cPort), "sockets bound");

    Platform hostPlat { &hostSock, &clock, nullptr };
    Platform aPlat { &aSock, &clock, nullptr };
    Platform bPlat { &bSock, &clock, nullptr };
    Platform cPlat { &cSock, &clock, nullptr };

    // Two slots: N connections to ONE Host identity, so same name and product id.
    Recorder slot0Rec ("slot0"), slot1Rec ("slot1");
    Session slot0 (hostPlat, Role::host, &slot0Rec, "Shared Host", "SHARED-HOST-1");
    Session slot1 (hostPlat, Role::host, &slot1Rec, "Shared Host", "SHARED-HOST-1");
    Session* slots[] = { &slot0, &slot1 };
    HostPort port (hostPlat, slots, 2);
    port.listen();

    Recorder aRec ("clientA"), bRec ("clientB"), cRec ("clientC");
    Session clientA (aPlat, Role::client, &aRec, "Client A", "CLIENT-A");
    Session clientB (bPlat, Role::client, &bRec, "Client B", "CLIENT-B");
    Session clientC (cPlat, Role::client, &cRec, "Client C", "CLIENT-C");

    Endpoint hostEp {}; std::strcpy (hostEp.address, "127.0.0.1"); hostEp.port = hPort;

    auto pump = [&] (int iterations) {
        for (int i = 0; i < iterations; ++i)
        { port.tick(); clientA.tick(); clientB.tick(); clientC.tick(); usleep (500); }
    };

    //-- two Clients, one port ------------------------------------------------
    clientA.connect (hostEp);
    for (int i = 0; i < 400 && clientA.state() != State::established; ++i) pump (1);
    check (clientA.state()==State::established, "client A establishes");

    clientB.connect (hostEp);
    for (int i = 0; i < 400 && clientB.state() != State::established; ++i) pump (1);
    check (clientB.state()==State::established,
           "client B establishes on the SAME port (one Session could not)");
    check (clientA.state()==State::established, "...and A is undisturbed by B arriving");
    check (port.activeCount() == 2, "the host is holding two sessions at once");

    //-- routed by sender, so no crosstalk ------------------------------------
    // Look the slots up the way the HOST sees each Client: by the Client's own
    // source address and port (§3.2). Note this is NOT clientA.remote(), which is
    // the host endpoint as seen from A.
    Endpoint aEp {}; std::strcpy (aEp.address, "127.0.0.1"); aEp.port = aPort;
    Endpoint bEp {}; std::strcpy (bEp.address, "127.0.0.1"); bEp.port = bPort;

    Session* forA = port.sessionFor (aEp);
    Session* forB = port.sessionFor (bEp);
    check (forA != nullptr && forB != nullptr && forA != forB,
           "sessionFor() distinguishes the two Clients by source endpoint");
    if (forA == nullptr || forB == nullptr)
    {
        printf ("\nFAIL: cannot continue without both slots resolved\n");
        return 1;
    }

    std::uint32_t fromA[2] = { 0x40903C00u, 0xAAAA0000u };
    std::uint32_t fromB[2] = { 0x40904D00u, 0xBBBB0000u };
    clientA.sendUmp (fromA, 2); pump (60);
    clientB.sendUmp (fromB, 2); pump (60);

    Recorder* recA = (forA == &slot0) ? &slot0Rec : &slot1Rec;
    Recorder* recB = (forB == &slot0) ? &slot0Rec : &slot1Rec;
    check (recA->umpCount == 1 && recA->lastWord0 == 0x40903C00u, "A's UMP reached A's slot");
    check (recB->umpCount == 1 && recB->lastWord0 == 0x40904D00u, "B's UMP reached B's slot");
    check (recA != recB, "...and they are genuinely different listeners (no crosstalk)");

    // Host -> each Client individually.
    std::uint32_t toA[2] = { 0x40B04A00u, 0x11110000u };
    std::uint32_t toB[2] = { 0x40B04B00u, 0x22220000u };
    forA->sendUmp (toA, 2); forB->sendUmp (toB, 2); pump (80);
    check (aRec.umpCount == 1 && aRec.lastWord0 == 0x40B04A00u, "host -> A delivered to A only");
    check (bRec.umpCount == 1 && bRec.lastWord0 == 0x40B04B00u, "host -> B delivered to B only");

    //-- a third Client, with the host full -----------------------------------
    // §6.16 Table 27: Bye 0x40 is "Invitation Failed: too many opened sessions".
    // Silence here is what makes a full host indistinguishable from a dead one.
    clientC.connect (hostEp);
    pump (200);
    check (clientC.state() != State::established, "client C is NOT given a session");
    check (port.activeCount() == 2, "...the host still holds exactly two");
    check (clientA.state()==State::established && clientB.state()==State::established,
           "...and neither existing Client was disturbed");

    bool sawTooMany = false;
    {
        // clientC's Session ignores a Bye 0x40 arriving while inviting, so read the
        // wire directly to prove the host actually answered rather than went quiet.
        PosixUdp probe; std::uint16_t pPort = 0; probe.bind (0, pPort);
        std::uint8_t b[128]; Writer w (b, sizeof b);
        const char* n = "Probe"; const char* pid = "PROBE-1";
        w.writeSignature();
        writeInvitation (w, 0, n, std::strlen (n), pid, std::strlen (pid));
        probe.send (hostEp, b, w.size());
        for (int i = 0; i < 200; ++i) { port.tick(); usleep (500); }

        std::uint8_t in[256]; Endpoint from; int rn = 0;
        while ((rn = probe.receive (in, sizeof in, from)) > 0)
            parseDatagram (in, std::size_t (rn), [&] (const ParsedCommand& c) {
                if (c.code == Command::bye
                    && c.data1() == std::uint8_t (ByeReason::tooManySessions))
                    sawTooMany = true;
            });
    }
    check (sawTooMany, "a full host answers an Invitation with Bye 0x40, not silence");

    //-- being refused is an answer, and C acts on it --------------------------
    // The Bye ends C's pending invitation instead of leaving it retrying into a
    // host that has no room. That is the point of answering at all: a Client that
    // is told "full" can report it, back off, or try elsewhere, none of which it
    // can do while guessing. Getting back in is then a deliberate new Invitation.
    for (int i = 0; i < 400 && clientC.state() == State::inviting; ++i) pump (1);
    check (clientC.state() != State::inviting,
           "the refused Client stops inviting rather than retrying into a full host");

    //-- a departing Client frees its slot -------------------------------------
    clientA.close (ByeReason::userTerminated);
    for (int i = 0; i < 600 && clientA.state() != State::closed; ++i) pump (1);
    check (clientA.state()==State::closed, "client A closes");
    for (int i = 0; i < 400 && port.activeCount() != 1; ++i) pump (1);
    check (port.activeCount() == 1, "...and the host drops to one session");
    check (clientB.state()==State::established, "...while B carries on untouched");

    // The freed slot must be reusable, not dead weight left behind by the Client
    // that vacated it.
    clientC.connect (hostEp);
    for (int i = 0; i < 1500 && clientC.state() != State::established; ++i) pump (1);
    check (clientC.state()==State::established,
           "client C gets the freed slot on a fresh Invitation");
    check (port.activeCount() == 2, "the host is full again, with a different pair");
    check (clientB.state()==State::established, "...and B survived all of it");

    Endpoint cEp {}; std::strcpy (cEp.address, "127.0.0.1"); cEp.port = cPort;
    Session* forC = port.sessionFor (cEp);
    check (forC != nullptr && forC == forA,
           "...reusing the very slot A vacated, not a third one");

    printf ("\n%s: host multi-client (%d/%d)\n",
            passes == checks ? "PASS" : "FAIL", passes, checks);
    return passes == checks ? 0 : 1;
}
