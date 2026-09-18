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
        const char* n = s==State::idle?"idle":s==State::inviting?"inviting"
                      : s==State::established?"established":s==State::closing?"closing":"closed";
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

    // 7. A third party cannot operate on a session it is not part of.
    //
    // The established host/client pair from step 1 is still up. A raw socket that
    // has never been invited now sends the three things that used to be acted on
    // regardless of who sent them. Each one used to be a real hole:
    //   Bye      -> closed somebody else's session outright. One 8-byte datagram,
    //               from anywhere on the LAN. This is what made a restart appear
    //               to "fix" a stuck client: the parting Bye of the restarting
    //               session freed the host slot by evicting the box using it.
    //   UMP_DATA -> was delivered to the listener as if the peer had sent it,
    //               i.e. anyone could inject notes into a running MIDI session.
    //   Ping     -> was answered, and (worse) refreshed the liveness timer, so a
    //               peer that had actually gone away still looked present.
    {
        PosixUdp stranger;
        std::uint16_t sPort = 0;
        check (stranger.bind (0, sPort), "stranger: raw socket bound");
        /* hostEp (declared above) already addresses the host from step 1. */

        check (host.state()==State::established && client.state()==State::established,
               "stranger: pair is established to begin with");

        auto sendRaw = [&] (void (*build) (Writer&)) {
            std::uint8_t buf[64]; Writer w (buf, sizeof buf);
            w.writeSignature(); build (w);
            stranger.send (hostEp, buf, w.size());
        };

        const int hostRxBefore = hostRec.umpCount;

        // Pull one datagram off the stranger's socket and report its first command.
        std::uint32_t lastWord0 = 0;
        auto recvFirst = [&] (Command& codeOut, std::uint8_t& data1Out) -> bool {
            std::uint8_t in[256]; Endpoint from;
            const int n = stranger.receive (in, sizeof in, from);
            if (n <= 0)
                return false;
            bool got = false;
            parseDatagram (in, std::size_t (n), [&] (const ParsedCommand& c) {
                if (got) return;
                codeOut = c.code; data1Out = c.data1();
                lastWord0 = c.payload ? get32 (c.payload) : 0u;
                got = true;
            });
            return got;
        };

        /* Order matters: the Bye goes LAST. Sent first it would close the session,
         * and the UMP and Ping checks below would then pass for the wrong reason
         * -- rejected as "no session" rather than as "not your session". */
        {
            std::uint32_t spoof[2] = { 0x40903C00u, 0xFFFF0000u };
            std::uint8_t buf[64]; Writer w (buf, sizeof buf);
            w.writeSignature(); writeUmpData (w, 0x4242, spoof, 2);
            stranger.send (hostEp, buf, w.size());
        }
        pump (30);
        check (hostRec.umpCount == hostRxBefore, "stranger: UMP_DATA is NOT delivered");
        check (host.state()==State::established, "stranger: still established after stray UMP");

        /* §7.1: UMP Data received outside an Established Session shall be answered
         * with Bye reason 0x05. We are established -- but not with THIS sender, so
         * from its point of view there is no session and it is owed the Bye. Being
         * told is the point: a peer that still believes in a session we no longer
         * have (because we restarted) would otherwise transmit into the void
         * forever. Answering must not disturb our real session, which the checks
         * above and below confirm. */
        {
            Command code {}; std::uint8_t d1 = 0;
            const bool got = recvFirst (code, d1);
            check (got && code == Command::bye
                       && d1 == std::uint8_t (ByeReason::sessionNotEstablished),
                   "stranger: stray UMP_DATA is answered with Bye 0x05 (spec 7.1)");
        }

        /* A Ping IS answered, whoever sends it. §6.1 Table 9 puts Ping in "Every
         * State" and §6.13 lets anyone send one at any time -- it is how a peer
         * checks we are alive before inviting. A host busy with one Client that
         * stayed silent would look dead to every other box on the LAN.
         *
         * Answering is not accepting: the liveness timer must NOT be refreshed by
         * this, which is what the `liveness:` checks below prove separately. Those
         * two behaviours used to be welded together -- refusing the Ping was how the
         * timer was protected -- and separating them is what makes this safe. */
        sendRaw ([] (Writer& w) { writePing (w, 0x99u); });
        pump (30);
        {
            Command code {}; std::uint8_t d1 = 0;
            const bool got = recvFirst (code, d1);
            check (got && code == Command::pingReply,
                   "stranger: a Ping IS answered even mid-session (spec 6.1 Table 9)");
            check (lastWord0 == 0x99u, "stranger: ...echoing the sender's Ping Id");
        }

        sendRaw ([] (Writer& w) { writeBye (w, ByeReason::undefined); });
        pump (30);
        check (host.state()==State::established, "stranger: Bye does NOT close the session");

        /* ...but it IS acknowledged. §6.16: the Bye Reply "shall also be sent if
         * there is no Pending or Established Session", because a Bye is repeated
         * until answered — so the sender we know nothing about is exactly the one
         * that would otherwise retransmit until it times out. Acknowledging is not
         * accepting: the check above already confirmed our session is untouched. */
        {
            Command code {}; std::uint8_t d1 = 0;
            const bool got = recvFirst (code, d1);
            check (got && code == Command::byeReply,
                   "stranger: ...but the Bye IS acknowledged with a Bye Reply (spec 6.16)");
        }

        // ...and the legitimate peer is completely unaffected by all of it.
        std::uint32_t real[2] = { 0x40B04A00u, 0x80000000u };
        const int cliBefore = clientRec.umpCount;
        host.sendUmp (real, 2);
        pump (50);
        check (clientRec.umpCount == cliBefore + 1, "stranger: real peer still flows afterwards");
    }

    // 7b. A stranger's traffic must not keep a dead session looking alive.
    //
    // The other half of the sender check, and the subtler half. Liveness used to
    // be refreshed by ANY datagram from ANY source, so a box that pinged a host it
    // was not in session with would hold that host's timeout open indefinitely --
    // the real peer could be switched off and unplugged and the host would never
    // notice. (This is precisely the pathology the NAK fix above documents on the
    // Zephyr side; this library had its own version of it.)
    //
    // Short timeout so the test costs a second, not ten.
    {
        PosixUdp h4Sock, c4Sock, nosy;
        std::uint16_t h4 = 0, c4 = 0, n4 = 0;
        h4Sock.bind (0, h4); c4Sock.bind (0, c4); nosy.bind (0, n4);
        Platform h4Plat { &h4Sock, &clock, nullptr }, c4Plat { &c4Sock, &clock, nullptr };
        Recorder h4Rec ("host4"), c4Rec ("client4");
        Session h4s (h4Plat, Role::host,   &h4Rec, "Timeout Host",   "TO-HOST-1");
        Session c4s (c4Plat, Role::client, &c4Rec, "Timeout Client", "TO-CLIENT-1");

        Session::Timing fast; fast.timeoutMs = 400; fast.pingIntervalMs = 100000;
        fast.byeTimeoutMs = 300;   // the vanished peer will never answer the Bye
        h4s.setTiming (fast);

        h4s.listen();
        Endpoint h4Ep {}; std::strcpy (h4Ep.address, "127.0.0.1"); h4Ep.port = h4;
        c4s.connect (h4Ep);
        for (int i = 0; i < 500 && h4s.state() != State::established; ++i)
        { h4s.tick(); c4s.tick(); usleep (1000); }
        check (h4s.state()==State::established, "liveness: established before the peer vanishes");

        // The real client now goes silent (never ticked again), while the nosy box
        // pings steadily. Only the silence should count.
        for (int i = 0; i < 800 && h4s.state() == State::established; ++i)
        {
            if (i % 50 == 0)
            {
                std::uint8_t buf[64]; Writer w (buf, sizeof buf);
                w.writeSignature(); writePing (w, 0x77u);
                nosy.send (h4Ep, buf, w.size());
            }
            h4s.tick(); usleep (1000);
        }
        // Timing out goes through Pending Bye now, not straight to Closed: the host
        // announces its departure with Bye 0x04 and waits for the acknowledgement.
        check (h4s.state()==State::closing,
               "liveness: host still times out despite a stranger's pings");

        // The peer is gone and will never answer, so the Bye must not be repeated
        // forever either -- §6.2 excepts a repeated Bye from "send a Bye on
        // timeout", so this expires quietly into Closed.
        for (int i = 0; i < 800 && h4s.state() != State::closed; ++i)
        { h4s.tick(); usleep (1000); }
        check (h4s.state()==State::closed,
               "liveness: an unanswered Bye eventually gives up rather than repeating forever");
    }

    // 7c. An over-long UMP Data command must not be believed.
    //
    // Regression test for a remotely triggerable stack buffer overflow. Payload
    // Length is one byte on the wire, so a command can claim up to 255 words, but
    // §7.1 Table 29 bounds a UMP Data command at 64 and deliverUmp's buffer is sized
    // for 64. Nothing on the receive path checked it, so a peer claiming 255 wrote
    // 191 words past the end of that buffer -- straight through the saved registers
    // and return address of the frames above it.
    //
    // It needed no malformed framing at all: 8 header bytes + 255*4 = 1028 bytes is
    // a perfectly well-formed datagram, comfortably inside the 1400-byte limit, so
    // parseDatagram's truncation check passed it along without complaint. Sent from
    // the established peer, so the sender check added in 7 does not mask it.
    // Confirmed with -fstack-protector-all: SIGABRT, "stack smashing detected".
    //
    // Note the shape of this check: the session must SURVIVE and ignore the command.
    // Before the fix this test did not fail politely, it aborted the whole binary.
    {
        const int hostRxBefore = hostRec.umpCount;

        std::uint8_t buf[kMaxDatagram];
        Writer w (buf, sizeof buf);
        w.writeSignature();
        w.writeHeader (Command::umpData, 255, 0x7777);   // 255 words claimed...
        for (int i = 0; i < 255; ++i)                    // ...and 255 actually sent,
            w.u32 (0xDEADBEEFu);                         //    so this is NOT truncated

        check (w.size() == 1028 && w.ok(), "oversized: datagram is well-formed (1028 bytes)");

        // Re-establish: step 7b's host timed out, but `host`/`client` from step 1 are
        // still live and still each other's peer.
        check (host.state()==State::established, "oversized: pair still established");

        clientSock.send (hostEp, buf, w.size());
        pump (40);

        check (hostRec.umpCount == hostRxBefore, "oversized: 255-word UMP Data is NOT delivered");
        check (host.state()==State::established,  "oversized: host survives it and stays established");

        // ...and a legitimate command still works immediately afterwards, so the
        // guard rejects the bad one without poisoning the session.
        std::uint32_t good[2] = { 0x40904000u, 0x12340000u };
        client.sendUmp (good, 2);
        pump (40);
        check (hostRec.umpCount == hostRxBefore + 1, "oversized: a valid UMP still flows after");
    }

    // 7d. The two replies the spec owes a sender we have no session with.
    //
    // Both are about not leaving a peer guessing. Silence is what made the NAK bug
    // (section 6) so expensive to find: a peer transmitting into a session the other
    // end has forgotten gets no signal at all, and cannot know to re-invite.
    //
    //   §7.1  UMP Data while not Established -> Bye reason 0x05. This is the
    //         restart case: we reboot, the peer still believes in the session and
    //         keeps sending. The Bye is what tells it otherwise.
    //   §5.5  A Command Code we do not support -> NAK reason 0x01, echoing the
    //         offending command's header word so the sender knows which one.
    //
    // Neither may refresh liveness or alter state -- answering is not accepting.
    {
        PosixUdp idleHostSock, farEnd;
        std::uint16_t ihPort = 0, fePort = 0;
        check (idleHostSock.bind (0, ihPort) && farEnd.bind (0, fePort), "spec-reply: sockets bound");

        Platform ihPlat { &idleHostSock, &clock, nullptr };
        Recorder ihRec ("idlehost");
        Session idleHost (ihPlat, Role::host, &ihRec, "Idle Host", "IDLE-HOST-1");
        idleHost.listen();                       // listening, but no session with anyone

        Endpoint ihEp {}; std::strcpy (ihEp.address, "127.0.0.1"); ihEp.port = ihPort;

        auto farRecvFirst = [&] (Command& codeOut, std::uint8_t& d1Out,
                                 std::uint32_t& firstWordOut) -> bool {
            std::uint8_t in[256]; Endpoint from;
            const int n = farEnd.receive (in, sizeof in, from);
            if (n <= 0)
                return false;
            bool got = false;
            parseDatagram (in, std::size_t (n), [&] (const ParsedCommand& c) {
                if (got) return;
                codeOut = c.code; d1Out = c.data1();
                firstWordOut = c.payload ? get32 (c.payload) : 0u;
                got = true;
            });
            return got;
        };

        // --- §7.1: UMP Data into a session that does not exist ---------------
        {
            std::uint32_t note[2] = { 0x40903C00u, 0xFFFF0000u };
            std::uint8_t buf[64]; Writer w (buf, sizeof buf);
            w.writeSignature(); writeUmpData (w, 0x0001, note, 2);
            farEnd.send (ihEp, buf, w.size());
        }
        for (int i = 0; i < 60; ++i) { idleHost.tick(); usleep (1000); }

        {
            Command code {}; std::uint8_t d1 = 0; std::uint32_t word0 = 0;
            const bool got = farRecvFirst (code, d1, word0);
            check (got && code == Command::bye
                       && d1 == std::uint8_t (ByeReason::sessionNotEstablished),
                   "spec-reply: UMP_DATA with no session earns Bye 0x05");
        }
        check (ihRec.umpCount == 0, "spec-reply: ...and the UMP itself is not delivered");
        check (idleHost.state() == State::idle, "spec-reply: ...and the host stays idle");

        // --- §5.5: a command code we do not implement ------------------------
        // Session Reset (0x82) is a real spec command, Phase 2, unimplemented here.
        // "Not supported" is exactly what NAK 0x01 is for.
        std::uint32_t offending = 0;
        {
            std::uint8_t buf[64]; Writer w (buf, sizeof buf);
            w.writeSignature();
            w.writeHeader (Command::sessionReset, 0, 0);
            offending = (std::uint32_t (std::uint8_t (Command::sessionReset)) << 24);
            farEnd.send (ihEp, buf, w.size());
        }
        for (int i = 0; i < 60; ++i) { idleHost.tick(); usleep (1000); }

        {
            Command code {}; std::uint8_t d1 = 0; std::uint32_t word0 = 0;
            const bool got = farRecvFirst (code, d1, word0);
            check (got && code == Command::nak, "spec-reply: unsupported command earns a NAK");
            check (d1 == std::uint8_t (NakReason::commandNotSupported),
                   "spec-reply: ...with reason 0x01 Command Not Supported");
            check (word0 == offending,
                   "spec-reply: ...echoing the offending command's header word");
        }
        check (idleHost.state() == State::idle, "spec-reply: ...and still no session was created");

        // --- §6.16: a Bye with no session at all is still acknowledged -------
        // The case the spec is actually about: a peer is tearing down a session we
        // have no record of (we restarted), and it repeats the Bye until answered.
        // Unanswered, it retransmits until it times out.
        {
            std::uint8_t buf[64]; Writer w (buf, sizeof buf);
            w.writeSignature(); writeBye (w, ByeReason::undefined);
            farEnd.send (ihEp, buf, w.size());
        }
        for (int i = 0; i < 60; ++i) { idleHost.tick(); usleep (1000); }
        {
            Command code {}; std::uint8_t d1 = 0; std::uint32_t word0 = 0;
            const bool got = farRecvFirst (code, d1, word0);
            check (got && code == Command::byeReply,
                   "spec-reply: a Bye with no session is still acknowledged");
        }
        check (idleHost.state() == State::idle,
               "spec-reply: ...and acknowledging it did not create or close anything");

        // --- §6.5: an Accepted with no invitation outstanding ----------------
        // "If a Client receives this Command when it is not in a Pending Session
        // with the Host, then the Client shall send a Bye Command to the Host with
        // reason 0x06 (No Pending Invitation)."
        //
        // It must also NOT open a session. An Accepted from an endpoint we never
        // invited is either a stale retransmission or someone handing us a session
        // unasked; neither may establish one.
        {
            std::uint8_t buf[64]; Writer w (buf, sizeof buf);
            const char* n = "Pushy Host"; const char* p = "PUSHY-1";
            w.writeSignature();
            writeInvitationAccepted (w, n, std::strlen (n), p, std::strlen (p));
            farEnd.send (ihEp, buf, w.size());
        }
        for (int i = 0; i < 60; ++i) { idleHost.tick(); usleep (1000); }
        {
            Command code {}; std::uint8_t d1 = 0; std::uint32_t word0 = 0;
            const bool got = farRecvFirst (code, d1, word0);
            check (got && code == Command::bye
                       && d1 == std::uint8_t (ByeReason::noPendingSession),
                   "spec-reply: an unsolicited Accepted earns Bye 0x06");
        }
        check (idleHost.state() == State::idle,
               "spec-reply: ...and an uninvited Accepted does NOT open a session");

        // --- a supported command must NOT be NAK'ed --------------------------
        // Ping is answered (an idle host that refuses to answer looks dead), and the
        // answer must be a Ping Reply, not a NAK.
        {
            std::uint8_t buf[64]; Writer w (buf, sizeof buf);
            w.writeSignature(); writePing (w, 0x1234u);
            farEnd.send (ihEp, buf, w.size());
        }
        for (int i = 0; i < 60; ++i) { idleHost.tick(); usleep (1000); }
        {
            Command code {}; std::uint8_t d1 = 0; std::uint32_t word0 = 0;
            const bool got = farRecvFirst (code, d1, word0);
            check (got && code == Command::pingReply && word0 == 0x1234u,
                   "spec-reply: a supported command is answered normally, not NAK'ed");
        }
    }

    // 7e. A repeated Accepted must not tear down the session it just opened.
    //
    // The dangerous half of §6.5. A host repeats its Invitation Reply: Accepted
    // until it sees traffic, so a duplicate landing just after the client
    // established is entirely routine -- not an error. §6.5 says to ignore it when
    // already Established, and that clause has to be checked BEFORE the "no pending
    // invitation -> Bye 0x06" clause. Get the order wrong and the client answers its
    // own host's retransmission with a Bye, using the handshake's own recovery
    // mechanism to destroy the session. That is a worse bug than the one Bye 0x06
    // fixes, so it is pinned here.
    {
        check (client.state()==State::established, "dup-accept: client established to begin with");
        const int cliBefore = clientRec.umpCount;

        // Sent from the host's own socket, so it genuinely arrives from the client's
        // peer -- a stranger's copy would be a different case entirely (7d).
        Endpoint clientEp {}; std::strcpy (clientEp.address, "127.0.0.1"); clientEp.port = clientPort;
        {
            std::uint8_t buf[128]; Writer w (buf, sizeof buf);
            const char* n = "M2 SoundGen Host"; const char* p = "NUBBSOFT-HOST-1";
            w.writeSignature();
            writeInvitationAccepted (w, n, std::strlen (n), p, std::strlen (p));
            hostSock.send (clientEp, buf, w.size());
        }
        pump (40);

        check (client.state()==State::established, "dup-accept: client is STILL established");
        // If the client had replied Bye 0x06, the host would have closed on receipt.
        check (host.state()==State::established,   "dup-accept: the host was NOT sent a Bye");

        std::uint32_t good[2] = { 0x40B04B00u, 0x80000000u };
        host.sendUmp (good, 2); pump (40);
        check (clientRec.umpCount == cliBefore + 1, "dup-accept: UMP still flows afterwards");
    }

    // 7f. FEC repeats must be recognised as already-seen.
    //
    // §7.2: "Receivers ignore UMP Data Commands with a Sequence Number which has
    // already been received and processed", and §7.2.2 makes it a receiver shall:
    // "Every Device receiving a UDP packet with UMP data shall be able to skip
    // previously received UMP Data Commands."
    //
    // Remembering only the newest Sequence Number failed this completely. A FEC
    // sender prepends its previous commands oldest-first, so datagram N carries
    // [N-2, N-1, N]; compared against the newest alone, N-2 does not match, gets
    // re-delivered, AND drags the marker backwards so N-1 misses too. Measured
    // against a standard two-repeat sender before the fix: every message delivered
    // three times -- every note-on fired three times, every CC applied three times.
    // It had not been noticed only because the Teensy does not send FEC yet.
    {
        PosixUdp fecHostSock, fecPeer;
        std::uint16_t fhPort = 0, fpPort = 0;
        check (fecHostSock.bind (0, fhPort) && fecPeer.bind (0, fpPort), "fec: sockets bound");

        Platform fhPlat { &fecHostSock, &clock, nullptr };
        Recorder fhRec ("fechost");
        Session fecHost (fhPlat, Role::host, &fhRec, "FEC Host", "FEC-HOST-1");
        fecHost.listen();

        Endpoint fhEp {}; std::strcpy (fhEp.address, "127.0.0.1"); fhEp.port = fhPort;
        {
            std::uint8_t b[16]; Writer w (b, sizeof b);
            w.writeSignature(); w.writeHeader (Command::invitation, 0, 0);
            fecPeer.send (fhEp, b, w.size());
        }
        for (int i = 0; i < 100 && fecHost.state() != State::established; ++i)
        { fecHost.tick(); usleep (1000); }
        check (fecHost.state()==State::established, "fec: established");

        // One datagram carrying these Sequence Numbers, oldest first (FEC order).
        auto sendFec = [&] (const std::uint16_t* seqs, int n) {
            std::uint8_t b[kMaxDatagram]; Writer w (b, sizeof b);
            w.writeSignature();
            for (int i = 0; i < n; ++i)
            {
                const std::uint32_t ump[2] = { 0x40904000u | std::uint32_t (seqs[i] & 0xFFu),
                                               0x11110000u };
                writeUmpData (w, seqs[i], ump, 2);
            }
            fecPeer.send (fhEp, b, w.size());
            for (int i = 0; i < 30; ++i) { fecHost.tick(); usleep (500); }
        };

        // --- steady state: five messages, each repeated twice by FEC -----------
        int mark = fhRec.umpCount;
        { std::uint16_t s[] = {1};       sendFec (s, 1); }
        { std::uint16_t s[] = {1,2};     sendFec (s, 2); }
        { std::uint16_t s[] = {1,2,3};   sendFec (s, 3); }
        { std::uint16_t s[] = {2,3,4};   sendFec (s, 3); }
        { std::uint16_t s[] = {3,4,5};   sendFec (s, 3); }
        check (fhRec.umpCount - mark == 5,
               "fec: 5 distinct messages delivered exactly once each (was 3x each)");

        // --- FEC does its job: a lost datagram is recovered from the next ------
        // The datagram whose newest command was seq 6 never arrives. Its content
        // comes back as a repeat inside the following one, and must be DELIVERED --
        // that recovery is the entire point of FEC, so "not the newest" can never
        // by itself be a reason to discard.
        mark = fhRec.umpCount;
        { std::uint16_t s[] = {5,6,7};   sendFec (s, 3); }
        check (fhRec.umpCount - mark == 2, "fec: a dropped datagram is recovered (6 and 7)");

        // --- an old duplicate still inside the window is ignored ---------------
        mark = fhRec.umpCount;
        { std::uint16_t s[] = {3};       sendFec (s, 1); }
        check (fhRec.umpCount - mark == 0, "fec: an older, already-seen seq is ignored");

        // --- something far older than the window remembers is ignored ----------
        mark = fhRec.umpCount;
        { std::uint16_t s[] = {0xFF00};  sendFec (s, 1); }   // ~250 behind
        check (fhRec.umpCount - mark == 0, "fec: a seq older than the window is ignored");

        // --- the session still works normally afterwards -----------------------
        mark = fhRec.umpCount;
        { std::uint16_t s[] = {8};       sendFec (s, 1); }
        check (fhRec.umpCount - mark == 1, "fec: a genuinely new seq still arrives");
    }

    // 7g. Sequence numbers wrap at 0xFFFF (§5.6) — the window must wrap with them.
    {
        PosixUdp wrapHostSock, wrapPeer;
        std::uint16_t whPort = 0, wpPort = 0;
        wrapHostSock.bind (0, whPort); wrapPeer.bind (0, wpPort);

        Platform whPlat { &wrapHostSock, &clock, nullptr };
        Recorder whRec ("wraphost");
        Session wrapHost (whPlat, Role::host, &whRec, "Wrap Host", "WRAP-HOST-1");
        wrapHost.listen();

        Endpoint whEp {}; std::strcpy (whEp.address, "127.0.0.1"); whEp.port = whPort;
        {
            std::uint8_t b[16]; Writer w (b, sizeof b);
            w.writeSignature(); w.writeHeader (Command::invitation, 0, 0);
            wrapPeer.send (whEp, b, w.size());
        }
        for (int i = 0; i < 100 && wrapHost.state() != State::established; ++i)
        { wrapHost.tick(); usleep (1000); }

        auto sendWrap = [&] (const std::uint16_t* seqs, int n) {
            std::uint8_t b[kMaxDatagram]; Writer w (b, sizeof b);
            w.writeSignature();
            for (int i = 0; i < n; ++i)
            {
                const std::uint32_t ump[2] = { 0x40905000u, 0x22220000u };
                writeUmpData (w, seqs[i], ump, 2);
            }
            wrapPeer.send (whEp, b, w.size());
            for (int i = 0; i < 30; ++i) { wrapHost.tick(); usleep (500); }
        };

        const int mark = whRec.umpCount;
        { std::uint16_t s[] = {0xFFFE};                 sendWrap (s, 1); }
        { std::uint16_t s[] = {0xFFFE, 0xFFFF, 0x0000}; sendWrap (s, 3); }
        { std::uint16_t s[] = {0xFFFF, 0x0000, 0x0001}; sendWrap (s, 3); }

        // 0xFFFE, 0xFFFF, 0x0000, 0x0001 — four distinct, and the repeats across
        // the wrap boundary must be seen as repeats, not as a 65535-packet jump.
        check (whRec.umpCount - mark == 4,
               "wrap: 0xFFFE->0x0001 delivers 4 distinct, repeats still deduplicated");
    }

    // 7h. An invitation that is never answered must give up (§6.2).
    //
    // "If a Device reaches its preferred timeout without receiving a suitable reply,
    // then the Device shall cease repeating the Command and send a Bye Command."
    //
    // A client used to retry the Invitation forever: no timeout, no error, no Bye,
    // no way for the application to learn that nobody was listening. This is the
    // client-side half of the handshake deadlock -- 141a075 fixed the host's refusal
    // to re-answer, but the client's inability to ever stop was left in place, and
    // that half needs no broken peer at all. Here the "host" is a bare socket that
    // never replies.
    {
        PosixUdp lonelyClientSock, deafHost;
        std::uint16_t lcPort = 0, dhPort = 0;
        check (lonelyClientSock.bind (0, lcPort) && deafHost.bind (0, dhPort),
               "invite-timeout: sockets bound");

        Platform lcPlat { &lonelyClientSock, &clock, nullptr };
        Recorder lcRec ("lonely");
        Session lonely (lcPlat, Role::client, &lcRec, "Lonely Client", "LONELY-1");

        // Timing chosen for margin, not speed: the "still trying" checkpoint sits
        // far below inviteTimeoutMs so a slow CI runner overshooting its sleeps
        // cannot turn a correct library into a red build. Everything below is
        // driven off the clock rather than an iteration count, for the same reason.
        Session::Timing t;
        t.inviteRetryMs   = 50;
        t.inviteTimeoutMs = 1000;
        t.byeTimeoutMs    = 400;      // the deaf host will not answer the Bye either
        lonely.setTiming (t);

        // One drain, accumulating into counters. Reading the socket is destructive,
        // so a drain that throws away what it does not currently care about will
        // eat the evidence a later check needs -- which it did.
        int  invitations   = 0;
        bool sawByeTimeout = false;
        auto drainDeafHost = [&] {
            std::uint8_t in[512]; Endpoint from; int n = 0;
            while ((n = deafHost.receive (in, sizeof in, from)) > 0)
                parseDatagram (in, std::size_t (n), [&] (const ParsedCommand& c) {
                    if (c.code == Command::invitation)
                        ++invitations;
                    else if (c.code == Command::bye
                             && c.data1() == std::uint8_t (ByeReason::timeout))
                        sawByeTimeout = true;
                });
        };

        Endpoint dhEp {}; std::strcpy (dhEp.address, "127.0.0.1"); dhEp.port = dhPort;
        lonely.connect (dhEp);
        check (lonely.state()==State::inviting, "invite-timeout: client starts out inviting");

        // Keep trying for a while -- giving up instantly would be its own bug.
        const std::uint32_t t0 = clock.nowMs();
        while (clock.nowMs() - t0 < 250) { lonely.tick(); drainDeafHost(); usleep (500); }
        check (lonely.state()==State::inviting, "invite-timeout: still inviting well before the timeout");
        check (invitations >= 2, "invite-timeout: the Invitation was actually repeated");

        // ...then stop, announcing it with a Bye rather than going quiet. Break the
        // instant the state leaves `inviting`, so Pending Bye is observed as it
        // happens and cannot be missed by expiring into Closed first.
        for (int i = 0; i < 4000 && lonely.state() == State::inviting; ++i)
        { lonely.tick(); drainDeafHost(); usleep (500); }
        check (lonely.state()==State::closing, "invite-timeout: gives up and enters Pending Bye");

        for (int i = 0; i < 20; ++i) { lonely.tick(); drainDeafHost(); usleep (500); }
        check (sawByeTimeout, "invite-timeout: ...and says why, with Bye reason 0x04");

        for (int i = 0; i < 4000 && lonely.state() != State::closed; ++i)
        { lonely.tick(); drainDeafHost(); usleep (500); }
        check (lonely.state()==State::closed, "invite-timeout: reaches Closed, not stuck");
    }

    // 7i. A Bye is repeated until acknowledged (§6.16).
    //
    // "The Bye Command should be sent repeatedly until a Bye Reply Command is
    // received, or until a timeout occurs."
    //
    // close() used to send exactly one Bye and declare itself Closed on the spot, so
    // a single lost datagram left the peer holding a session we had already dropped,
    // waiting out its own idle timeout with no idea we had gone -- the teardown
    // twin of the lost-Accepted bug. The peer here is a raw socket that ignores the
    // first Bye entirely and only then answers.
    {
        PosixUdp goodbyeSock, lazyPeer;
        std::uint16_t gbPort = 0, lpPort = 0;
        goodbyeSock.bind (0, gbPort); lazyPeer.bind (0, lpPort);

        Platform gbPlat { &goodbyeSock, &clock, nullptr };
        Recorder gbRec ("goodbye");
        Session goodbye (gbPlat, Role::host, &gbRec, "Goodbye Host", "BYE-HOST-1");
        // byeTimeoutMs is deliberately huge: this test answers the Bye itself, so
        // the timeout must never be what ends the wait, however slow the machine.
        Session::Timing t; t.byeRetryMs = 60; t.byeTimeoutMs = 60000;
        goodbye.setTiming (t);
        goodbye.listen();

        Endpoint gbEp {}; std::strcpy (gbEp.address, "127.0.0.1"); gbEp.port = gbPort;
        {
            std::uint8_t b[16]; Writer w (b, sizeof b);
            w.writeSignature(); w.writeHeader (Command::invitation, 0, 0);
            lazyPeer.send (gbEp, b, w.size());
        }
        for (int i = 0; i < 100 && goodbye.state() != State::established; ++i)
        { goodbye.tick(); usleep (1000); }
        check (goodbye.state()==State::established, "bye-repeat: established");

        { std::uint8_t in[256]; Endpoint f; while (lazyPeer.receive (in, sizeof in, f) > 0) {} }

        goodbye.close (ByeReason::userTerminated);
        check (goodbye.state()==State::closing,
               "bye-repeat: close() enters Pending Bye, not Closed");

        // Ignore the Bye for a while. It must keep arriving.
        for (int i = 0; i < 250; ++i) { goodbye.tick(); usleep (1000); }
        check (goodbye.state()==State::closing, "bye-repeat: still waiting for the Bye Reply");

        int byes = 0; Endpoint fromPeerAddr {};
        {
            std::uint8_t in[256]; Endpoint from; int n = 0;
            while ((n = lazyPeer.receive (in, sizeof in, from)) > 0)
            {
                fromPeerAddr = from;
                parseDatagram (in, std::size_t (n), [&] (const ParsedCommand& c) {
                    if (c.code == Command::bye
                        && c.data1() == std::uint8_t (ByeReason::userTerminated))
                        ++byes;
                });
            }
        }
        check (byes >= 2, "bye-repeat: the Bye was retransmitted, reason preserved");

        // Now answer it; the repeats must stop and the session finish.
        {
            std::uint8_t b[16]; Writer w (b, sizeof b);
            w.writeSignature(); writeByeReply (w);
            lazyPeer.send (gbEp, b, w.size());
        }
        for (int i = 0; i < 200 && goodbye.state() != State::closed; ++i)
        { goodbye.tick(); usleep (1000); }
        check (goodbye.state()==State::closed, "bye-repeat: the Bye Reply completes the close");

        { std::uint8_t in[256]; Endpoint f; while (lazyPeer.receive (in, sizeof in, f) > 0) {} }
        for (int i = 0; i < 200; ++i) { goodbye.tick(); usleep (1000); }
        {
            std::uint8_t in[256]; Endpoint f;
            check (lazyPeer.receive (in, sizeof in, f) <= 0,
                   "bye-repeat: ...and nothing more is sent afterwards");
        }
    }

    // 7j. A quiet sender declares itself idle (§7.2.1).
    //
    // "If a Sender has a period where there is no UMP data to send, the Sender shall
    // send a Zero Length UMP Data Command to inform the Receiver that the Sender
    // currently has no further UMP data" -- the first within 300ms of the last
    // non-zero-length command, then at growing intervals, eventually stopping.
    //
    // Without it, a sender with nothing to play looks exactly like one that has died
    // or lost its route, and the receiver can only wait out its own idle timeout.
    {
        PosixUdp idleSock, watcher;
        std::uint16_t isPort = 0, wPort = 0;
        check (idleSock.bind (0, isPort) && watcher.bind (0, wPort), "idle: sockets bound");

        Platform isPlat { &idleSock, &clock, nullptr };
        Recorder isRec ("idlesender");
        Session sender (isPlat, Role::host, &isRec, "Idle Sender", "IDLE-SENDER-1");
        Session::Timing t;
        t.pingIntervalMs   = 100000;   // keep pings out of the way
        t.idleDeclareMs    = 60;
        t.idleDeclareCount = 4;        // 60, 120, 240, 480 then silence
        sender.setTiming (t);
        sender.listen();

        Endpoint isEp {}; std::strcpy (isEp.address, "127.0.0.1"); isEp.port = isPort;
        {
            std::uint8_t b[16]; Writer w (b, sizeof b);
            w.writeSignature(); w.writeHeader (Command::invitation, 0, 0);
            watcher.send (isEp, b, w.size());
        }
        for (int i = 0; i < 200 && sender.state() != State::established; ++i)
        { sender.tick(); usleep (500); }
        check (sender.state()==State::established, "idle: established");

        int zeroLen = 0, withData = 0;
        std::uint16_t seqs[16] = {}; int seqCount = 0;
        auto drainWatcher = [&] {
            std::uint8_t in[512]; Endpoint from; int n = 0;
            while ((n = watcher.receive (in, sizeof in, from)) > 0)
                parseDatagram (in, std::size_t (n), [&] (const ParsedCommand& c) {
                    if (c.code != Command::umpData) return;
                    if (c.payloadWords == 0) ++zeroLen; else ++withData;
                    if (seqCount < 16) seqs[seqCount++] = c.cmdSpecific;
                });
        };

        // Nothing has been sent yet, so there is nothing to be idle FROM. A peer
        // that has never sent UMP must stay quiet rather than chatter.
        for (int i = 0; i < 400; ++i) { sender.tick(); drainWatcher(); usleep (500); }
        check (zeroLen == 0, "idle: a sender that never sent UMP stays silent");

        // Send one real message; the idle declarations should follow it.
        std::uint32_t note[2] = { 0x40903C00u, 0xFFFF0000u };
        check (sender.sendUmp (note, 2), "idle: a real UMP is sent");
        for (int i = 0; i < 300; ++i) { sender.tick(); drainWatcher(); usleep (500); }
        check (withData == 1, "idle: ...and arrives exactly once");
        check (zeroLen >= 1, "idle: a zero-length declaration follows the quiet period");

        // Let the backoff run out. It must stop, not trickle forever.
        for (int i = 0; i < 3000; ++i) { sender.tick(); drainWatcher(); usleep (500); }
        check (zeroLen == 4, "idle: exactly idleDeclareCount declarations, then silence");

        const int settled = zeroLen;
        for (int i = 0; i < 1500; ++i) { sender.tick(); drainWatcher(); usleep (500); }
        check (zeroLen == settled, "idle: ...and it really has stopped");

        // §7.2.1: "Zero Length Data Commands shall use Sequence Numbers in the same
        // manner as any other UMP Data Command." Consecutive and increasing, so the
        // receiver's gap detection still works across the quiet patch.
        bool seqOk = (seqCount >= 5);
        for (int i = 1; i < seqCount && seqOk; ++i)
            seqOk = (std::uint16_t (seqs[i-1] + 1) == seqs[i]);
        check (seqOk, "idle: declarations carry consecutive Sequence Numbers (spec 7.2.1)");

        // Sending real data again must restart the whole cycle.
        check (sender.sendUmp (note, 2), "idle: a second real UMP is sent");
        for (int i = 0; i < 400; ++i) { sender.tick(); drainWatcher(); usleep (500); }
        check (zeroLen > settled, "idle: fresh data re-arms the idle declarations");
    }

    // 8. Graceful close.
    client.close(); pump (50);
    check (host.state()==State::closed && client.state()==State::closed, "graceful Bye -> both Closed");

    printf ("\n%s: session loopback (%d/%d)\n", passes==checks?"PASS":"FAIL", passes, checks);
    return passes==checks ? 0 : 1;
}
