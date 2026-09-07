/*
    libnetmidi2 loopback test — runs a Host Session and a Client Session over real
    localhost UDP, drives the full Invitation handshake, exchanges UMP both ways,
    and closes gracefully. Proves our side of the wire end-to-end (the Teensy's
    titou-based lib interops by both following MA M2-124-UM).

    Build:  clang++ -std=c++17 -I ../include session_loopback.cpp -o session_loopback
*/

#include "netmidi2/Session.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>

using namespace netmidi2;

//== test-only platform adapters ==============================================
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
        if (n < 0) return 0;               // EWOULDBLOCK -> nothing pending
        ::inet_ntop (AF_INET, &a.sin_addr, from.address, sizeof from.address);
        from.port = ntohs (a.sin_port);
        return (int) n;
    }
};

struct PosixClock : IClock
{
    std::uint32_t nowMs() override
    {
        timeval tv; ::gettimeofday (&tv, nullptr);
        return std::uint32_t (tv.tv_sec * 1000ull + tv.tv_usec / 1000);
    }
};

struct Recorder : ISessionListener
{
    const char*  who;
    int          umpCount = 0;
    std::uint32_t lastWord0 = 0;
    State        state = State::idle;
    explicit Recorder (const char* w) : who (w) {}
    void onUmpReceived (const std::uint32_t* words, std::uint8_t count) override
    {
        ++umpCount; lastWord0 = count ? words[0] : 0;
        printf ("  [%s] received UMP: %u word(s), word0=0x%08X\n", who, count, lastWord0);
    }
    void onStateChanged (State s) override
    {
        state = s;
        const char* n = s==State::idle?"idle":s==State::inviting?"inviting":s==State::established?"established":"closed";
        printf ("  [%s] state -> %s\n", who, n);
    }
};

//== the test =================================================================
int main()
{
    PosixUdp hostSock, clientSock;
    PosixClock clock;

    std::uint16_t hostPort = 0, clientPort = 0;
    if (! hostSock.bind (0, hostPort) || ! clientSock.bind (0, clientPort))
    { std::puts ("FAIL: bind"); return 1; }
    printf ("host bound :%u, client bound :%u\n", hostPort, clientPort);

    Platform hostPlat  { &hostSock,   &clock, nullptr };
    Platform clientPlat { &clientSock, &clock, nullptr };

    Recorder hostRec ("host"), clientRec ("client");
    Session host   (hostPlat,   Role::host,   &hostRec,   "M2 SoundGen Host", "NUBBSOFT-HOST-1");
    Session client (clientPlat, Role::client, &clientRec, "Teensy Zephyr M2", "TEENSY-CLIENT-1");

    host.listen();
    Endpoint hostEp {}; std::strcpy (hostEp.address, "127.0.0.1"); hostEp.port = hostPort;
    client.connect (hostEp);

    auto pump = [&] (int iterations) {
        for (int i = 0; i < iterations; ++i) { host.tick(); client.tick(); usleep (1000); }
    };

    int checks = 0, passes = 0;
    auto check = [&] (bool ok, const char* label) { ++checks; if (ok) ++passes; printf ("  %s  %s\n", ok?"OK  ":"FAIL", label); };

    // 1. Handshake -> both Established.
    for (int i = 0; i < 500 && ! (host.state()==State::established && client.state()==State::established); ++i) pump (1);
    check (host.state()==State::established && client.state()==State::established, "handshake: both Established");

    // 2. Client -> Host UMP (MIDI 2.0 note-on C4).
    std::uint32_t noteOn[2] = { 0x40903C00u, 0xFFFF0000u };
    client.sendUmp (noteOn, 2); pump (20);
    check (hostRec.umpCount==1 && hostRec.lastWord0==0x40903C00u, "client -> host UMP delivered");

    // 3. Host -> Client UMP (CC).
    std::uint32_t cc[2] = { 0x40B04A00u, 0x80000000u };
    host.sendUmp (cc, 2); pump (20);
    check (clientRec.umpCount==1 && clientRec.lastWord0==0x40B04A00u, "host -> client UMP delivered");

    // 4. Duplicate sequence number is ignored (resend same seq by hand).
    {
        std::uint8_t buf[64]; Writer w (buf, sizeof buf);
        w.writeSignature(); writeUmpData (w, 0x0000, noteOn, 2);   // seq 0 again (already seen)
        clientSock.send (hostEp, buf, w.size()); pump (20);
        check (hostRec.umpCount==1, "duplicate sequence number ignored");
    }

    // 5. A lost InvitationAccepted must be recoverable.
    //
    // This is a regression test for a handshake that could deadlock permanently.
    // The host used to accept an Invitation only while it was NOT established, so
    // if its one Accepted went missing the client retried forever against a host
    // that had already made up its mind. Nothing timed out either: the host kept
    // receiving those retries and counted them as liveness. Seen in the field as a
    // host reporting `established` with a peer whose client still said `inviting`,
    // minutes later.
    //
    // Simulated by draining the client's socket after the host accepts, which
    // discards the Accepted exactly as the network would have.
    {
        PosixUdp host2Sock, client2Sock;
        std::uint16_t h2 = 0, c2 = 0;
        host2Sock.bind (0, h2); client2Sock.bind (0, c2);
        Platform h2Plat { &host2Sock, &clock, nullptr }, c2Plat { &client2Sock, &clock, nullptr };
        Recorder h2Rec ("host2"), c2Rec ("client2");
        Session h2s (h2Plat, Role::host,   &h2Rec, "Lossy Host",   "LOSSY-HOST-1");
        Session c2s (c2Plat, Role::client, &c2Rec, "Lossy Client", "LOSSY-CLIENT-1");

        h2s.listen();
        Endpoint h2Ep {}; std::strcpy (h2Ep.address, "127.0.0.1"); h2Ep.port = h2;
        c2s.connect (h2Ep);

        // Let the host accept, but never let the client tick -- so the Accepted is
        // still sitting in its socket, unread.
        for (int i = 0; i < 200 && h2s.state() != State::established; ++i) { h2s.tick(); usleep (1000); }
        check (h2s.state()==State::established, "lost-accept: host accepted");

        // Drop it on the floor.
        { std::uint8_t junk[2048]; Endpoint from; while (client2Sock.receive (junk, sizeof junk, from) > 0) {} }
        check (c2s.state()==State::inviting, "lost-accept: client left inviting (the deadlock)");

        // The client retries every inviteRetryMs (500). An established host must
        // answer that retry instead of ignoring it.
        for (int i = 0; i < 3000 && c2s.state() != State::established; ++i)
        { h2s.tick(); c2s.tick(); usleep (1000); }
        check (c2s.state()==State::established, "lost-accept: client recovers on retry");

        // ...but a DIFFERENT endpoint must not be able to take the session away.
        {
            PosixUdp intruder; std::uint16_t ip = 0; intruder.bind (0, ip);
            const Endpoint before = h2s.remote();
            std::uint8_t buf[128]; Writer w (buf, sizeof buf);
            const char *iname = "Intruder", *ipid = "INTRUDER-1";
            w.writeSignature();
            writeInvitation (w, 0, iname, std::strlen (iname), ipid, std::strlen (ipid));
            intruder.send (h2Ep, buf, w.size());
            for (int i = 0; i < 50; ++i) { h2s.tick(); usleep (1000); }
            check (h2s.state()==State::established && h2s.remote() == before,
                   "lost-accept: a different endpoint cannot steal the session");
        }
    }

    // 6. NAK recovery: a peer that forgot our session must be recoverable
    // automatically, not just by an application restarting the connection by
    // hand.
    //
    // This is the bug measured against the Teensy. Our client stayed
    // `established` forever because the peer answers Ping unconditionally, with
    // no session check at all -- so touch() kept resetting our idle timer even
    // after the peer had no memory of us (e.g. after it rebooted). The peer DOES
    // still send a NAK for the UMP_DATA it rejects (Zephyr's netmidi2.c does;
    // Session.h's own host role does not yet -- a separate, smaller gap). That
    // NAK is concrete proof the session died, and used to fall into the
    // "Phase 1: ignore" default case.
    //
    // No Session plays the host role here: a raw socket stands in for "a peer
    // that no longer runs the protocol on our behalf", which is exactly what a
    // rebooted host is from the client's point of view.
    {
        PosixUdp rawHost, client3Sock;
        std::uint16_t rhPort = 0, c3Port = 0;
        rawHost.bind (0, rhPort); client3Sock.bind (0, c3Port);
        Platform c3Plat { &client3Sock, &clock, nullptr };
        Recorder c3Rec ("client3");
        Session client3 (c3Plat, Role::client, &c3Rec, "Nak Client", "NAK-CLIENT-1");

        Endpoint rhEp {}; std::strcpy (rhEp.address, "127.0.0.1"); rhEp.port = rhPort;
        client3.connect (rhEp);

        // Waits specifically for an Invitation, ignoring anything else the client
        // sends meanwhile (its periodic Ping, in particular) -- otherwise this
        // would "succeed" by answering the wrong packet and mask a client that
        // never actually re-invited.
        auto answerNextInvitation = [&] (const char* label) -> bool {
            for (int i = 0; i < 500; ++i)
            {
                std::uint8_t inbuf[512]; Endpoint from;
                const int n = rawHost.receive (inbuf, sizeof inbuf, from);
                bool gotInvitation = false;
                if (n > 0)
                    parseDatagram (inbuf, std::size_t (n), [&] (const ParsedCommand& c) {
                        if (c.code == Command::invitation) gotInvitation = true;
                    });
                if (gotInvitation)
                {
                    check (true, label);
                    std::uint8_t buf[128]; Writer w (buf, sizeof buf);
                    const char *n_ = "Raw Host", *p_ = "RAW-HOST-1";
                    w.writeSignature();
                    writeInvitationAccepted (w, n_, std::strlen (n_), p_, std::strlen (p_));
                    rawHost.send (from, buf, w.size());
                    return true;
                }
                client3.tick(); usleep (1000);
            }
            check (false, label);
            return false;
        };

        answerNextInvitation ("nak-recovery: raw host saw the Invitation");
        for (int i = 0; i < 500 && client3.state() != State::established; ++i) { client3.tick(); usleep (1000); }
        check (client3.state()==State::established, "nak-recovery: client established against raw host");

        // The "host" now rejects everything, as if it restarted and forgot us.
        std::uint32_t noteOn2[2] = { 0x40903C00u, 0xFFFF0000u };
        client3.sendUmp (noteOn2, 2);
        {
            std::uint8_t inbuf[512]; Endpoint from; int n = 0;
            for (int i = 0; i < 200 && n <= 0; ++i)
            { n = rawHost.receive (inbuf, sizeof inbuf, from); if (n<=0) usleep (1000); }
            check (n > 0, "nak-recovery: raw host saw the stray UMP_DATA");
            std::uint8_t buf[64]; Writer w (buf, sizeof buf);
            w.writeSignature(); w.writeHeader (Command::nak, 0, 0);
            if (n > 0) rawHost.send (from, buf, w.size());
        }

        for (int i = 0; i < 200 && client3.state() != State::inviting; ++i) { client3.tick(); usleep (1000); }
        check (client3.state()==State::inviting, "nak-recovery: client re-invites after a NAK");

        answerNextInvitation ("nak-recovery: raw host saw the fresh Invitation");
        for (int i = 0; i < 500 && client3.state() != State::established; ++i) { client3.tick(); usleep (1000); }
        check (client3.state()==State::established, "nak-recovery: client re-established");
    }

    // 7. Graceful close.
    client.close(); pump (50);
    check (host.state()==State::closed && client.state()==State::closed, "graceful Bye -> both Closed");

    printf ("\n%s: session loopback (%d/%d)\n", passes==checks?"PASS":"FAIL", passes, checks);
    return passes==checks ? 0 : 1;
}
