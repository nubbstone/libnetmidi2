/*
    libnetmidi2 — Session Reset (§6.11 Reset / §6.12 Reset Reply).

    "A Session Reset Command is used to reset the Sequence Number to zero", for when
    the two ends have drifted apart and recovery is not possible -- §6.11 gives
    "packets have been lost, 2 devices are out of sync, data recovery is not possible"
    and aborting an oversized System Exclusive as the cases it is for.

    The subtle half is the RECEIVE side. Resetting the send counter is obvious;
    forgetting to clear the receive window is not, and it fails in a way that looks
    like the reset worked: the peer restarts at Sequence Number 0, the window still
    remembers the old numbers, and every message after the reset is silently
    discarded as a duplicate it has "already seen". So the central check here is not
    that a reset happened but that traffic still flows afterwards.
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
    bool bind (std::uint16_t d, std::uint16_t& o) override
    {
        fd = ::socket (AF_INET, SOCK_DGRAM, 0); if (fd < 0) return false;
        ::fcntl (fd, F_SETFL, O_NONBLOCK);
        sockaddr_in a {}; a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl (INADDR_LOOPBACK); a.sin_port = htons (d);
        if (::bind (fd, (sockaddr*) &a, sizeof a) != 0) return false;
        sockaddr_in g {}; socklen_t gl = sizeof g; ::getsockname (fd, (sockaddr*) &g, &gl);
        o = ntohs (g.sin_port); return true;
    }
    int send (const Endpoint& to, const std::uint8_t* d, std::size_t n) override
    {
        sockaddr_in a {}; a.sin_family = AF_INET; a.sin_port = htons (to.port);
        ::inet_pton (AF_INET, to.address, &a.sin_addr);
        return (int) ::sendto (fd, d, n, 0, (sockaddr*) &a, sizeof a);
    }
    int receive (std::uint8_t* b, std::size_t cap, Endpoint& f) override
    {
        sockaddr_in a {}; socklen_t al = sizeof a;
        const ssize_t n = ::recvfrom (fd, b, cap, 0, (sockaddr*) &a, &al);
        if (n < 0) return 0;
        ::inet_ntop (AF_INET, &a.sin_addr, f.address, sizeof f.address);
        f.port = ntohs (a.sin_port); return (int) n;
    }
};
struct PosixClock : IClock
{
    std::uint32_t nowMs() override
    { timeval tv; ::gettimeofday (&tv, nullptr); return std::uint32_t (tv.tv_sec*1000ull + tv.tv_usec/1000); }
};
struct Recorder : ISessionListener
{
    int umpCount = 0, resets = 0;
    std::uint32_t lastWord0 = 0;
    void onUmpReceived (const std::uint32_t* w, std::uint8_t n) override
    { ++umpCount; lastWord0 = n ? w[0] : 0; }
    void onSessionReset() override { ++resets; }
};

static int checks = 0, passes = 0;
static void check (bool ok, const char* label)
{ ++checks; if (ok) ++passes; printf ("  %s  %s\n", ok ? "OK  " : "FAIL", label); }

int main()
{
    setvbuf (stdout, nullptr, _IONBF, 0);
    std::puts ("Session Reset (spec 6.11 / 6.12)\n");
    PosixClock clock;

    //== the full exchange, between two real Sessions ==========================
    std::puts ("Resetting an established session");
    {
        PosixUdp hSock, cSock;
        std::uint16_t hp = 0, cp = 0;
        check (hSock.bind (0, hp) && cSock.bind (0, cp), "sockets bound");
        Platform hPlat { &hSock, &clock, nullptr, nullptr };
        Platform cPlat { &cSock, &clock, nullptr, nullptr };
        Recorder hRec, cRec;
        Session host   (hPlat, Role::host,   &hRec, "Host", "H-1");
        Session client (cPlat, Role::client, &cRec, "Cli",  "C-1");
        Session::Timing t; t.idleDeclareCount = 0; t.pingIntervalMs = 100000;
        host.setTiming (t); client.setTiming (t);
        SentUmpSlot hist[4];
        client.setSentUmpHistory (hist, 4);
        host.listen();

        Endpoint hEp {}; std::strcpy (hEp.address, "127.0.0.1"); hEp.port = hp;
        client.connect (hEp);
        auto pump = [&] (int n) { for (int i = 0; i < n; ++i) { host.tick(); client.tick(); usleep (500); } };
        for (int i = 0; i < 800 && client.state() != State::established; ++i) pump (1);
        check (client.state()==State::established, "established");

        // Burn through some sequence numbers so a reset is observable.
        for (int i = 0; i < 5; ++i)
        {
            std::uint32_t m[2] = { 0x40903C00u | std::uint32_t (i), 0x11110000u };
            client.sendUmp (m, 2); pump (30);
        }
        check (hRec.umpCount == 5, "five messages delivered before the reset");

        // §6.11: not instantaneous -- Pending Session Reset until the reply lands.
        check (client.resetSession(), "resetSession() is accepted");
        check (client.state()==State::resetting, "...and enters Pending Session Reset, not done");

        // "the Device shall not send any UMP Data Commands" while waiting.
        std::uint32_t blocked[2] = { 0x40904000u, 0x99990000u };
        check (! client.sendUmp (blocked, 2), "UMP is refused while the reset is pending (6.11)");

        for (int i = 0; i < 800 && client.state() != State::established; ++i) pump (1);
        check (client.state()==State::established, "the reply completes the reset");
        check (host.state()==State::established,   "...and the host is established throughout");
        check (cRec.resets == 1 && hRec.resets == 1,
               "both ends notified the application (6.11: so it can stop hanging notes)");

        /* The check that matters. Both ends restart at Sequence Number 0, so the
         * receive window has to have been cleared -- otherwise seq 0 is discarded as
         * a duplicate and the session goes quiet while looking perfectly healthy. */
        const int before = hRec.umpCount;
        for (int i = 0; i < 3; ++i)
        {
            std::uint32_t m[2] = { 0x40905000u | std::uint32_t (i), 0x22220000u };
            client.sendUmp (m, 2); pump (40);
        }
        pump (60);
        check (hRec.umpCount == before + 3,
               "traffic flows after the reset -- restarted seq 0 is NOT seen as a duplicate");
        check (hRec.lastWord0 == 0x40905002u, "...and it is the right traffic");
    }

    //== observed on the wire ==================================================
    std::puts ("\nOn the wire");
    {
        PosixUdp hSock, raw;
        std::uint16_t hp = 0, rp = 0;
        hSock.bind (0, hp); raw.bind (0, rp);
        Platform hPlat { &hSock, &clock, nullptr, nullptr };
        Recorder hRec;
        Session host (hPlat, Role::host, &hRec, "Host", "H-1");
        Session::Timing t; t.idleDeclareCount = 0; t.pingIntervalMs = 100000; t.timeoutMs = 600000;
        host.setTiming (t);
        host.listen();
        Endpoint hEp {}; std::strcpy (hEp.address, "127.0.0.1"); hEp.port = hp;
        { std::uint8_t b[16]; Writer w (b, sizeof b);
          w.writeSignature(); w.writeHeader (Command::invitation, 0, 0);
          raw.send (hEp, b, w.size()); }
        for (int i = 0; i < 300 && host.state() != State::established; ++i) { host.tick(); usleep (500); }
        { std::uint8_t in[kMaxDatagram]; Endpoint f; while (raw.receive (in, sizeof in, f) > 0) {} }

        // A peer asks US to reset: §6.11 "the Receiver shall respond with the Session
        // Reset Reply Command and reset the session".
        { std::uint8_t b[16]; Writer w (b, sizeof b);
          w.writeSignature(); writeSessionReset (w); raw.send (hEp, b, w.size()); }
        for (int i = 0; i < 300; ++i) { host.tick(); usleep (500); }

        bool sawReply = false;
        std::uint8_t in[512]; Endpoint f; int n = 0;
        while ((n = raw.receive (in, sizeof in, f)) > 0)
            parseDatagram (in, std::size_t (n), [&] (const ParsedCommand& c) {
                if (c.code == Command::sessionResetReply) sawReply = true;
            });
        check (sawReply, "an incoming Session Reset is answered with a Reply");
        check (hRec.resets == 1, "...and we reset too, not just acknowledge");
        check (host.state()==State::established, "...without leaving Established");
    }

    //== the unsolicited reply (6.12) ==========================================
    std::puts ("\nA Session Reset Reply we never asked for");
    {
        PosixUdp hSock, raw;
        std::uint16_t hp = 0, rp = 0;
        hSock.bind (0, hp); raw.bind (0, rp);
        Platform hPlat { &hSock, &clock, nullptr, nullptr };
        Recorder hRec;
        Session host (hPlat, Role::host, &hRec, "Host", "H-1");
        Session::Timing t; t.idleDeclareCount = 0; t.pingIntervalMs = 100000; t.timeoutMs = 600000;
        host.setTiming (t);
        host.listen();
        Endpoint hEp {}; std::strcpy (hEp.address, "127.0.0.1"); hEp.port = hp;
        { std::uint8_t b[16]; Writer w (b, sizeof b);
          w.writeSignature(); w.writeHeader (Command::invitation, 0, 0);
          raw.send (hEp, b, w.size()); }
        for (int i = 0; i < 300 && host.state() != State::established; ++i) { host.tick(); usleep (500); }
        { std::uint8_t in[kMaxDatagram]; Endpoint f; while (raw.receive (in, sizeof in, f) > 0) {} }

        /* §6.12: "If the receiver of a Session Reset Reply Command has not sent a
         * prior Session Reset Command, then the receiver should reset the Session by
         * sending a Session Reset Command." The peer thinks a reset happened that we
         * know nothing about, so the two now disagree about the numbering. */
        { std::uint8_t b[16]; Writer w (b, sizeof b);
          w.writeSignature(); writeSessionResetReply (w); raw.send (hEp, b, w.size()); }
        for (int i = 0; i < 300; ++i) { host.tick(); usleep (500); }

        bool sawReset = false;
        std::uint8_t in[512]; Endpoint f; int n = 0;
        while ((n = raw.receive (in, sizeof in, f)) > 0)
            parseDatagram (in, std::size_t (n), [&] (const ParsedCommand& c) {
                if (c.code == Command::sessionReset) sawReset = true;
            });
        check (sawReset, "an unsolicited Reply makes us request a reset of our own (6.12)");
        check (host.state()==State::resetting, "...so we are now Pending Session Reset");
    }

    //== nobody answers (6.11) =================================================
    std::puts ("\nA reset nobody answers");
    {
        PosixUdp hSock, deaf;
        std::uint16_t hp = 0, dp = 0;
        hSock.bind (0, hp); deaf.bind (0, dp);
        Platform hPlat { &hSock, &clock, nullptr, nullptr };
        Recorder hRec;
        Session host (hPlat, Role::host, &hRec, "Host", "H-1");
        Session::Timing t;
        t.idleDeclareCount = 0; t.pingIntervalMs = 100000; t.timeoutMs = 600000;
        t.resetRetryMs = 40; t.resetTimeoutMs = 300; t.byeTimeoutMs = 200;
        host.setTiming (t);
        host.listen();
        Endpoint hEp {}; std::strcpy (hEp.address, "127.0.0.1"); hEp.port = hp;
        { std::uint8_t b[16]; Writer w (b, sizeof b);
          w.writeSignature(); w.writeHeader (Command::invitation, 0, 0);
          deaf.send (hEp, b, w.size()); }
        for (int i = 0; i < 300 && host.state() != State::established; ++i) { host.tick(); usleep (500); }
        { std::uint8_t in[kMaxDatagram]; Endpoint f; while (deaf.receive (in, sizeof in, f) > 0) {} }

        check (host.resetSession(), "reset requested");
        int resets = 0; bool sawBye04 = false;
        for (int i = 0; i < 3000 && host.state() != State::closed; ++i)
        {
            host.tick(); usleep (500);
            std::uint8_t in[512]; Endpoint f; int n = 0;
            while ((n = deaf.receive (in, sizeof in, f)) > 0)
                parseDatagram (in, std::size_t (n), [&] (const ParsedCommand& c) {
                    if (c.code == Command::sessionReset) ++resets;
                    if (c.code == Command::bye
                        && c.data1() == std::uint8_t (ByeReason::timeout))
                        sawBye04 = true;
                });
        }
        check (resets >= 2, "the Session Reset is repeated (6.2 / 6.11)");
        check (sawBye04, "...then the session ends with Bye 0x04 Timeout (6.11)");
        check (host.state()==State::closed, "...and reaches Closed");
    }

    //== outside a session =====================================================
    std::puts ("\nReset commands with no session");
    {
        PosixUdp iSock, prod;
        std::uint16_t ip = 0, pp = 0;
        iSock.bind (0, ip); prod.bind (0, pp);
        Platform iPlat { &iSock, &clock, nullptr, nullptr };
        Recorder iRec;
        Session idle (iPlat, Role::host, &iRec, "Idle", "I-1");
        idle.listen();
        Endpoint iEp {}; std::strcpy (iEp.address, "127.0.0.1"); iEp.port = ip;

        for (int which = 0; which < 2; ++which)
        {
            std::uint8_t b[16]; Writer w (b, sizeof b);
            w.writeSignature();
            if (which == 0) writeSessionReset (w); else writeSessionResetReply (w);
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
            check (sawBye05, which == 0 ? "a Session Reset with no session earns Bye 0x05"
                                        : "a Session Reset Reply with no session earns Bye 0x05");
        }
        check (iRec.resets == 0, "...and neither one reset anything");
    }

    printf ("\n%s: session reset (%d/%d)\n", passes == checks ? "PASS" : "FAIL", passes, checks);
    return passes == checks ? 0 : 1;
}
