/*
    libnetmidi2 — Retransmit (§7.2.3 Request / §7.2.4 Error).

    The other half of loss recovery. FEC repairs a gap blindly by repeating recent
    commands; Retransmit repairs one on request, and reaches further back because the
    responder answers from the whole retained history rather than the last two.

    Two spec contradictions had to be decided, and both are asserted here so the
    choices are visible rather than accidental:

      - What to send when only part of the requested range survives. §7.2.3 says
        retransmit what follows the gap; §7.2.4 says "shall not retransmit other
        available UMP Data Commands". We follow §7.2.3 -- withholding data that is
        right there helps nobody, and the requester's dedup window makes an extra
        command free.

      - What Sequence Number the Error carries. Table 31 says "the first UMP Data
        Command that COULD be retransmitted"; §7.2.3's prose says "the first missing
        Sequence Number". The table wins, as it has every other time the two have
        disagreed in this spec, and it is the only one a requester can act on.
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

struct PosixUdp : IUdpSocket
{
    int fd = -1;
    bool bind (std::uint16_t d, std::uint16_t& out) override
    {
        fd = ::socket (AF_INET, SOCK_DGRAM, 0);
        if (fd < 0) return false;
        ::fcntl (fd, F_SETFL, O_NONBLOCK);
        sockaddr_in a {}; a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl (INADDR_LOOPBACK); a.sin_port = htons (d);
        if (::bind (fd, (sockaddr*) &a, sizeof a) != 0) return false;
        sockaddr_in g {}; socklen_t gl = sizeof g;
        ::getsockname (fd, (sockaddr*) &g, &gl); out = ntohs (g.sin_port);
        return true;
    }
    int send (const Endpoint& to, const std::uint8_t* d, std::size_t n) override
    {
        sockaddr_in a {}; a.sin_family = AF_INET; a.sin_port = htons (to.port);
        ::inet_pton (AF_INET, to.address, &a.sin_addr);
        return (int) ::sendto (fd, d, n, 0, (sockaddr*) &a, sizeof a);
    }
    int receive (std::uint8_t* b, std::size_t cap, Endpoint& from) override
    {
        sockaddr_in a {}; socklen_t al = sizeof a;
        const ssize_t n = ::recvfrom (fd, b, cap, 0, (sockaddr*) &a, &al);
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
    int           umpCount = 0;
    int           lostCount = 0;
    std::uint16_t lastLostSeq = 0;
    State         current = State::idle;
    int           leftEstablished = 0;   // transitions OUT of established

    void onUmpReceived (const std::uint32_t*, std::uint8_t) override { ++umpCount; }
    void onUmpLost (std::uint16_t seq) override { ++lostCount; lastLostSeq = seq; }
    void onStateChanged (State s) override
    {
        if (current == State::established && s != State::established)
            ++leftEstablished;
        current = s;
    }
};

static int checks = 0, passes = 0;
static void check (bool ok, const char* label)
{ ++checks; if (ok) ++passes; printf ("  %s  %s\n", ok ? "OK  " : "FAIL", label); }

int main()
{
    setvbuf (stdout, nullptr, _IONBF, 0);
    std::puts ("Retransmit (spec 7.2.3 / 7.2.4)\n");

    PosixClock clock;

    //== responder: a Session serving requests from its history ================
    PosixUdp holderSock, asker;
    std::uint16_t hPort = 0, aPort = 0;
    check (holderSock.bind (0, hPort) && asker.bind (0, aPort), "sockets bound");

    Platform hPlat { &holderSock, &clock, nullptr };
    Recorder hRec;
    Session holder (hPlat, Role::host, &hRec, "Holder", "HOLDER-1");
    Session::Timing t; t.pingIntervalMs = 100000; t.timeoutMs = 600000; t.idleDeclareCount = 0;
    holder.setTiming (t);
    SentUmpSlot hist[6];
    holder.setSentUmpHistory (hist, 6);
    holder.setFecRepeats (0);            // isolate Retransmit from FEC here
    holder.listen();

    Endpoint hEp {}; std::strcpy (hEp.address, "127.0.0.1"); hEp.port = hPort;
    {
        std::uint8_t b[16]; Writer w (b, sizeof b);
        w.writeSignature(); w.writeHeader (Command::invitation, 0, 0);
        asker.send (hEp, b, w.size());
    }
    for (int i = 0; i < 300 && holder.state() != State::established; ++i) { holder.tick(); usleep (500); }
    check (holder.state()==State::established, "responder established");

    /*  Drain until the socket has been quiet for a while, NOT just once.

        A single non-blocking pass right after a burst of sends leaves whatever is
        still in flight, and those stragglers then land inside the next collection
        window and are counted as replies. That produced a 1-in-3 flake where a
        request appeared to be answered twice -- the diagnostic read "5 6 7 8 9 10
        5 6 7 8 9 10", which is six undrained originals followed by six genuine
        retransmits. The library was right; the test was measuring its own leftovers.
    */
    auto drainAsker = [&] {
        int quiet = 0;
        for (int i = 0; i < 400 && quiet < 80; ++i)
        {
            bool got = false;
            std::uint8_t in[kMaxDatagram]; Endpoint f;
            while (asker.receive (in, sizeof in, f) > 0) got = true;
            quiet = got ? 0 : quiet + 1;
            usleep (500);
        }
    };
    // Everything the holder sends back, gathered across the whole wait rather than
    // sampled once at the end. Draining only after the tick loop was flaky roughly
    // 1 run in 10: if the request happened to be processed on a late tick, the
    // replies were still in flight when the drain ran and the count came up short.
    struct Reply { int umps = 0; std::uint16_t seq[16] = {}; bool error = false;
                   std::uint8_t errReason = 0; std::uint16_t errSeq = 0; int naks = 0; };
    auto askFor = [&] (std::uint16_t seq) -> Reply {
        Reply r {};
        std::uint8_t b[32]; Writer w (b, sizeof b);
        w.writeSignature(); writeRetransmitRequest (w, seq, 0);
        asker.send (hEp, b, w.size());

        int quiet = 0;
        for (int i = 0; i < 600 && quiet < 120; ++i)
        {
            holder.tick(); usleep (500);
            bool got = false;
            std::uint8_t in[kMaxDatagram]; Endpoint f; int n = 0;
            while ((n = asker.receive (in, sizeof in, f)) > 0)
            {
                got = true;
                parseDatagram (in, std::size_t (n), [&] (const ParsedCommand& c) {
                    if (c.code == Command::umpData && r.umps < 16) r.seq[r.umps++] = c.cmdSpecific;
                    else if (c.code == Command::retransmitError)
                    { r.error = true; r.errReason = c.data1(); if (c.payload) r.errSeq = get16 (c.payload); }
                    else if (c.code == Command::nak) ++r.naks;
                });
            }
            quiet = got ? 0 : quiet + 1;     // settle once nothing more arrives
        }
        return r;
    };

    // Fill the history with five commands, seq 0..4.
    std::puts ("\nServing a request from the history");
    {
        for (int i = 0; i < 5; ++i)
        {
            std::uint32_t m[2] = { 0x40903C00u | std::uint32_t (i), 0x11110000u };
            holder.sendUmp (m, 2);
        }
        drainAsker();

        Reply r = askFor (2);
        check (! r.error && r.umps == 3, "asking for seq 2 returns 2, 3 and 4");
        check (r.umps == 3 && r.seq[0] == 2 && r.seq[1] == 3 && r.seq[2] == 4,
               "...in order, with no gaps (7.2.3)");
        check (r.naks == 0, "...and no NAK: we implement Retransmit, so we must not claim otherwise");
    }

    //-- a Sequence Number that has aged out -----------------------------------
    std::puts ("\nA request the buffer can no longer satisfy");
    {
        // Push the early ones out: history is 6 deep, so send enough to evict seq 0-4.
        for (int i = 0; i < 6; ++i)
        {
            std::uint32_t m[2] = { 0x40904C00u | std::uint32_t (i), 0x22220000u };
            holder.sendUmp (m, 2);
        }
        drainAsker();

        Reply r = askFor (1);             // long gone
        check (r.error, "a vanished Sequence Number earns a Retransmit Error (7.2.4)");
        check (r.errReason == std::uint8_t (RetransmitError::notInTransmitBuffer),
               "...with reason 0x01, not in the transmit buffer");
        check (r.errSeq == 5,
               "...naming the oldest seq still held, per Table 31 (not the missing one)");
        if (r.umps != 6)
        {
            printf ("        DIAG umps=%d errSeq=%u seqs:", r.umps, r.errSeq);
            for (int i = 0; i < r.umps; ++i) printf (" %u", r.seq[i]);
            printf ("\n");
        }
        check (r.umps == 6,
               "...and what survived is still sent (7.2.3), rather than withheld (7.2.4)");
    }

    //-- asking for something never sent ---------------------------------------
    std::puts ("\nA request for a Sequence Number we have not reached");
    {
        drainAsker();
        Reply r = askFor (9000);
        check (r.error, "an unknown future seq earns an Error");
        check (r.umps == 0, "...and no spurious replay of the whole buffer");
    }

    //-- with no history at all ------------------------------------------------
    std::puts ("\nWith no history lent to the Session");
    {
        PosixUdp bareSock, bareAsk;
        std::uint16_t bp = 0, ap2 = 0;
        bareSock.bind (0, bp); bareAsk.bind (0, ap2);
        Platform bPlat { &bareSock, &clock, nullptr };
        Recorder bRec;
        Session bare (bPlat, Role::host, &bRec, "Bare", "BARE-1");
        bare.setTiming (t);
        bare.listen();
        Endpoint bEp {}; std::strcpy (bEp.address, "127.0.0.1"); bEp.port = bp;
        { std::uint8_t b[16]; Writer w (b, sizeof b);
          w.writeSignature(); w.writeHeader (Command::invitation, 0, 0);
          bareAsk.send (bEp, b, w.size()); }
        for (int i = 0; i < 300 && bare.state() != State::established; ++i) { bare.tick(); usleep (500); }
        { std::uint8_t in[kMaxDatagram]; Endpoint f; while (bareAsk.receive (in, sizeof in, f) > 0) {} }

        { std::uint8_t b[32]; Writer w (b, sizeof b);
          w.writeSignature(); writeRetransmitRequest (w, 0, 0);
          bareAsk.send (bEp, b, w.size()); }
        for (int i = 0; i < 200; ++i) { bare.tick(); usleep (500); }

        bool sawError = false, sawNak = false;
        std::uint8_t in[kMaxDatagram]; Endpoint f; int n = 0;
        while ((n = bareAsk.receive (in, sizeof in, f)) > 0)
            parseDatagram (in, std::size_t (n), [&] (const ParsedCommand& c) {
                if (c.code == Command::retransmitError) sawError = true;
                if (c.code == Command::nak) sawNak = true;
            });
        check (sawError && ! sawNak,
               "an empty buffer answers Retransmit Error, not NAK (we do support it)");
    }

    //-- outside a session (7.2.3 / 7.2.4) -------------------------------------
    std::puts ("\nRetransmit commands with no session");
    {
        PosixUdp idleSock, prod;
        std::uint16_t ip = 0, pp = 0;
        idleSock.bind (0, ip); prod.bind (0, pp);
        Platform iPlat { &idleSock, &clock, nullptr };
        Recorder iRec;
        Session idle (iPlat, Role::host, &iRec, "Idle", "IDLE-1");
        idle.listen();
        Endpoint iEp {}; std::strcpy (iEp.address, "127.0.0.1"); iEp.port = ip;

        for (int which = 0; which < 2; ++which)
        {
            std::uint8_t b[32]; Writer w (b, sizeof b);
            w.writeSignature();
            if (which == 0) writeRetransmitRequest (w, 3, 0);
            else            writeRetransmitError (w, RetransmitError::unknown, 3);
            prod.send (iEp, b, w.size());
            for (int i = 0; i < 200; ++i) { idle.tick(); usleep (500); }

            bool sawBye05 = false;
            std::uint8_t in[512]; Endpoint f; int n = 0;
            while ((n = prod.receive (in, sizeof in, f)) > 0)
                parseDatagram (in, std::size_t (n), [&] (const ParsedCommand& c) {
                    if (c.code == Command::bye
                        && c.data1() == std::uint8_t (ByeReason::sessionNotEstablished))
                        sawBye05 = true;
                });
            check (sawBye05, which == 0 ? "a Retransmit Request with no session earns Bye 0x05"
                                        : "a Retransmit Error with no session earns Bye 0x05");
        }
    }

    //== requester: detecting a gap and asking ==================================
    std::puts ("\nAsking for a gap, end to end");
    {
        PosixUdp rxSock, txRaw;
        std::uint16_t rp = 0, tp = 0;
        rxSock.bind (0, rp); txRaw.bind (0, tp);
        Platform rPlat { &rxSock, &clock, nullptr };
        Recorder rRec;
        Session receiver (rPlat, Role::host, &rRec, "Receiver", "RX-1");
        Session::Timing rt; rt.pingIntervalMs = 100000; rt.timeoutMs = 600000;
        rt.idleDeclareCount = 0; rt.retransmitDelayMs = 20; rt.retransmitMaxRequests = 3;
        receiver.setTiming (rt);
        receiver.listen();

        Endpoint rEp {}; std::strcpy (rEp.address, "127.0.0.1"); rEp.port = rp;
        { std::uint8_t b[16]; Writer w (b, sizeof b);
          w.writeSignature(); w.writeHeader (Command::invitation, 0, 0);
          txRaw.send (rEp, b, w.size()); }
        for (int i = 0; i < 300 && receiver.state() != State::established; ++i) { receiver.tick(); usleep (500); }
        { std::uint8_t in[kMaxDatagram]; Endpoint f; while (txRaw.receive (in, sizeof in, f) > 0) {} }

        auto sendSeq = [&] (std::uint16_t seq) {
            std::uint32_t m[2] = { 0x40905C00u, 0x33330000u };
            std::uint8_t b[64]; Writer w (b, sizeof b);
            w.writeSignature(); writeUmpData (w, seq, m, 2);
            txRaw.send (rEp, b, w.size());
            for (int i = 0; i < 40; ++i) { receiver.tick(); usleep (500); }
        };

        sendSeq (0);
        sendSeq (1);
        sendSeq (4);                      // 2 and 3 never arrive
        check (rRec.umpCount == 3, "three datagrams delivered, two numbers missing");

        // The request must not go out instantly -- the delay is what lets reordering
        // and FEC resolve the gap first (7.2.3).
        std::uint16_t askedFor = 0; int requests = 0;
        for (int i = 0; i < 1200; ++i)
        {
            receiver.tick(); usleep (500);
            std::uint8_t in[512]; Endpoint f; int n = 0;
            while ((n = txRaw.receive (in, sizeof in, f)) > 0)
                parseDatagram (in, std::size_t (n), [&] (const ParsedCommand& c) {
                    if (c.code == Command::retransmitRequest) { ++requests; askedFor = c.cmdSpecific; }
                });
        }
        check (requests >= 1, "the receiver asks for the gap");
        check (askedFor == 2, "...starting at the FIRST missing number, not the last");
        check (requests <= 3, "...and repeats a bounded number of times, not forever");

        // Giving up happens on the due-check AFTER the last request, so keep ticking
        // rather than stopping the moment the requests stop.
        for (int i = 0; i < 2000 && rRec.lostCount == 0; ++i) { receiver.tick(); usleep (500); }
        check (rRec.lostCount == 1 && rRec.lastLostSeq == 2,
               "giving up reports the loss to the application (7.2.4)");
        check (requests <= 3, "...without having asked again while winding down");
    }

    //-- a peer that does not implement Retransmit ------------------------------
    std::puts ("\nA peer that NAKs the request");
    {
        PosixUdp rxSock, txRaw;
        std::uint16_t rp = 0, tp = 0;
        rxSock.bind (0, rp); txRaw.bind (0, tp);
        Platform rPlat { &rxSock, &clock, nullptr };
        Recorder rRec;
        Session receiver (rPlat, Role::client, &rRec, "Receiver2", "RX-2");
        Session::Timing rt; rt.pingIntervalMs = 100000; rt.timeoutMs = 600000;
        rt.idleDeclareCount = 0; rt.retransmitDelayMs = 20;
        receiver.setTiming (rt);

        Endpoint tEp {}; std::strcpy (tEp.address, "127.0.0.1"); tEp.port = tp;
        receiver.connect (tEp);
        // Play the host: accept, then answer any Retransmit Request with NAK 0x01.
        bool established = false;
        for (int i = 0; i < 600 && ! established; ++i)
        {
            std::uint8_t in[512]; Endpoint f; int n = 0;
            while ((n = txRaw.receive (in, sizeof in, f)) > 0)
                parseDatagram (in, std::size_t (n), [&] (const ParsedCommand& c) {
                    if (c.code != Command::invitation) return;
                    std::uint8_t b[128]; Writer w (b, sizeof b);
                    const char* nm = "Raw"; const char* pid = "RAW-1";
                    w.writeSignature();
                    writeInvitationAccepted (w, nm, std::strlen (nm), pid, std::strlen (pid));
                    txRaw.send (f, b, w.size());
                });
            receiver.tick(); usleep (500);
            established = (receiver.state() == State::established);
        }
        check (established, "established against a raw host");

        Endpoint rEp {}; std::strcpy (rEp.address, "127.0.0.1"); rEp.port = rp;
        auto sendSeq = [&] (std::uint16_t seq) {
            std::uint32_t m[2] = { 0x40906C00u, 0x44440000u };
            std::uint8_t b[64]; Writer w (b, sizeof b);
            w.writeSignature(); writeUmpData (w, seq, m, 2);
            txRaw.send (rEp, b, w.size());
            for (int i = 0; i < 40; ++i) { receiver.tick(); usleep (500); }
        };
        sendSeq (0); sendSeq (5);         // a gap at 1

        int requests = 0;
        for (int i = 0; i < 1500; ++i)
        {
            receiver.tick(); usleep (500);
            std::uint8_t in[512]; Endpoint f; int n = 0;
            while ((n = txRaw.receive (in, sizeof in, f)) > 0)
                parseDatagram (in, std::size_t (n), [&] (const ParsedCommand& c) {
                    if (c.code != Command::retransmitRequest) return;
                    ++requests;
                    // "shall reply ... with a NAK Command with reason 0x01" (7.2.3)
                    std::uint8_t b[64]; Writer w (b, sizeof b);
                    w.writeSignature();
                    writeNak (w, NakReason::commandNotSupported, c.headerWord());
                    txRaw.send (f, b, w.size());
                });
        }
        check (requests >= 1, "we asked at least once");
        check (requests <= 2, "...then stopped: 7.2.3 says do not ask a peer that NAKed");

        /* Checking the FINAL state is not enough here, and the weak version of this
         * check passed against a deliberately broken build. The generic NAK handler
         * re-invites, and this test's raw host answers Invitations, so a session torn
         * down by a mishandled NAK is re-established within milliseconds and looks
         * untouched by the end. Count the transitions instead. */
        check (rRec.leftEstablished == 0,
               "a NAK of a Retransmit Request must NOT tear down the session");
        check (rRec.lostCount >= 1, "...and the loss is reported instead");
    }

    printf ("\n%s: retransmit (%d/%d)\n", passes == checks ? "PASS" : "FAIL", passes, checks);
    return passes == checks ? 0 : 1;
}
