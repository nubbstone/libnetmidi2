/*
    libnetmidi2 — Forward Error Correction, sending side (§7.2.2).

    "Every Client and Host should implement FEC when sending UMP packets by including
    two previously sent UMP Data Commands." The receiving half has always worked --
    §7.2.2 makes coping with repeats a receiver `shall`, and the dedup window handles
    it -- so what is new here is emitting them.

    The rule that carries the most weight is the ordering one, because it is the one
    that looks like a style choice and is not:

        "Previous UMP payloads shall be prepended in the order in which they were
         sent. This is to allow the receiving Device to read each UMP Data Command in
         order in which it is received and just skip over the UMP Data Commands it
         has already processed."

    Emit newest-first and a conforming receiver walking forward sees the sequence
    numbers run backwards. So these tests read whole datagrams off the wire and check
    the order of the commands inside them, not merely that repeats are present.
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
    int           umpCount = 0;
    std::uint32_t lastWord0 = 0;
    void onUmpReceived (const std::uint32_t* w, std::uint8_t n) override
    { ++umpCount; lastWord0 = n ? w[0] : 0; }
};

// One datagram, decomposed into the UMP Data commands it carried, in wire order.
struct Datagram
{
    int           commands = 0;
    std::uint16_t seq[8] = {};
    std::uint8_t  words[8] = {};
    std::size_t   bytes = 0;
};

static int checks = 0, passes = 0;
static void check (bool ok, const char* label)
{ ++checks; if (ok) ++passes; printf ("  %s  %s\n", ok ? "OK  " : "FAIL", label); }

int main()
{
    setvbuf (stdout, nullptr, _IONBF, 0);
    std::puts ("FEC sending (spec 7.2.2)\n");

    PosixClock clock;
    PosixUdp senderSock, watcher;
    std::uint16_t sPort = 0, wPort = 0;
    check (senderSock.bind (0, sPort) && watcher.bind (0, wPort), "sockets bound");

    Platform sPlat { &senderSock, &clock, nullptr };
    Recorder sRec;
    Session sender (sPlat, Role::host, &sRec, "FEC Sender", "FEC-SENDER-1");
    Session::Timing t; t.pingIntervalMs = 100000; t.timeoutMs = 600000;
    t.idleDeclareCount = 0;                       // idle handled in its own section
    sender.setTiming (t);
    sender.listen();

    Endpoint sEp {}; std::strcpy (sEp.address, "127.0.0.1"); sEp.port = sPort;
    {
        std::uint8_t b[16]; Writer w (b, sizeof b);
        w.writeSignature(); w.writeHeader (Command::invitation, 0, 0);
        watcher.send (sEp, b, w.size());
    }
    for (int i = 0; i < 200 && sender.state() != State::established; ++i)
    { sender.tick(); usleep (500); }
    check (sender.state()==State::established, "established");

    auto nextDatagram = [&] (Datagram& d) -> bool {
        for (int attempt = 0; attempt < 200; ++attempt)
        {
            std::uint8_t in[kMaxDatagram]; Endpoint from;
            const int n = watcher.receive (in, sizeof in, from);
            if (n > 0)
            {
                d = Datagram {};
                d.bytes = std::size_t (n);
                parseDatagram (in, std::size_t (n), [&] (const ParsedCommand& c) {
                    if (c.code != Command::umpData || d.commands >= 8) return;
                    d.seq[d.commands]   = c.cmdSpecific;
                    d.words[d.commands] = c.payloadWords;
                    ++d.commands;
                });
                if (d.commands > 0) return true;
            }
            sender.tick(); usleep (500);
        }
        return false;
    };
    auto drain = [&] { std::uint8_t in[kMaxDatagram]; Endpoint f;
                       while (watcher.receive (in, sizeof in, f) > 0) {} };

    //-- off by default -------------------------------------------------------
    std::puts ("\nOff unless asked for");
    {
        drain();
        std::uint32_t m[2] = { 0x40903C00u, 0x11110000u };
        sender.sendUmp (m, 2); sender.sendUmp (m, 2); sender.sendUmp (m, 2);
        Datagram d {};
        bool allSingle = true;
        for (int i = 0; i < 3; ++i) { if (! nextDatagram (d) || d.commands != 1) allSingle = false; }
        check (allSingle, "with no slots lent, every datagram carries one command");
    }

    //-- two repeats, oldest first --------------------------------------------
    std::puts ("\nTwo repeats, prepended oldest-first");
    SentUmpSlot slots[2];
    sender.setSentUmpHistory (slots, 2);
    {
        drain();
        std::uint32_t m[2] = { 0x40904000u, 0x22220000u };
        Datagram d1 {}, d2 {}, d3 {}, d4 {};

        sender.sendUmp (m, 2);
        check (nextDatagram (d1) && d1.commands == 1, "1st datagram: nothing to repeat yet");

        sender.sendUmp (m, 2);
        check (nextDatagram (d2) && d2.commands == 2, "2nd datagram: one repeat + the new one");

        sender.sendUmp (m, 2);
        check (nextDatagram (d3) && d3.commands == 3, "3rd datagram: two repeats + the new one");

        sender.sendUmp (m, 2);
        check (nextDatagram (d4) && d4.commands == 3, "4th datagram: still two, the oldest rolls off");

        // The ordering rule, checked as ordering rather than as presence.
        check (d3.commands == 3
                 && std::uint16_t (d3.seq[0] + 1) == d3.seq[1]
                 && std::uint16_t (d3.seq[1] + 1) == d3.seq[2],
               "sequence numbers ASCEND across the datagram (7.2.2 FEC Packet Order)");
        check (d4.commands == 3 && d4.seq[0] == std::uint16_t (d3.seq[0] + 1),
               "...and the window slides by one each time");
        check (d3.commands == 3 && d3.seq[2] == std::uint16_t (d3.seq[0] + 2),
               "...with the NEW command last, not first");
    }

    //-- a receiver sees each message exactly once ----------------------------
    // The two halves have to agree: emitting repeats is only safe because the
    // receiving window skips them. This runs our sender against our receiver.
    std::puts ("\nRound trip: repeats are deduplicated, not replayed");
    {
        PosixUdp aSock, bSock;
        std::uint16_t aPort = 0, bPort = 0;
        aSock.bind (0, aPort); bSock.bind (0, bPort);
        Platform aPlat { &aSock, &clock, nullptr }, bPlat { &bSock, &clock, nullptr };
        Recorder aRec, bRec;
        Session host (aPlat, Role::host,   &aRec, "RT Host",   "RT-HOST-1");
        Session cli  (bPlat, Role::client, &bRec, "RT Client", "RT-CLIENT-1");
        Session::Timing rt; rt.idleDeclareCount = 0; rt.pingIntervalMs = 100000;
        host.setTiming (rt); cli.setTiming (rt);

        SentUmpSlot cliSlots[2];
        cli.setSentUmpHistory (cliSlots, 2);

        host.listen();
        Endpoint hEp {}; std::strcpy (hEp.address, "127.0.0.1"); hEp.port = aPort;
        cli.connect (hEp);
        auto pump = [&] (int n) { for (int i = 0; i < n; ++i) { host.tick(); cli.tick(); usleep (500); } };
        for (int i = 0; i < 600 && cli.state() != State::established; ++i) pump (1);
        check (cli.state()==State::established, "round-trip pair established");

        const int before = aRec.umpCount;
        for (int i = 0; i < 6; ++i)
        {
            std::uint32_t m[2] = { 0x40905000u | std::uint32_t (i), 0x33330000u };
            cli.sendUmp (m, 2);
            pump (40);
        }
        pump (100);
        check (aRec.umpCount - before == 6,
               "6 messages sent with FEC arrive as 6, not 6 + their repeats");
    }

    //-- repeats must not push a datagram past 1400 bytes ---------------------
    std::puts ("\nThe 1400-byte limit wins over the repeats");
    {
        SentUmpSlot big[5];
        sender.setSentUmpHistory (big, 5);
        drain();

        std::uint32_t maxUmp[kMaxUmpWordsPerCommand];
        for (std::size_t i = 0; i < kMaxUmpWordsPerCommand; ++i) maxUmp[i] = 0x40000000u | std::uint32_t (i);

        /* The failure mode to guard against is NOT an oversized datagram -- Writer
         * would refuse that on its own. It is the quiet one: with 5 slots of 64-word
         * commands the repeats alone come to 1304 bytes, so a sender that ignores the
         * budget leaves no room for the new command, the write fails, and sendUmp
         * returns false having sent nothing at all. FEC would then silently stop the
         * traffic it exists to protect. So the check is that every send SUCCEEDS. */
        Datagram d {};
        bool everOversized = false, allSent = true, everyOneArrived = true;
        int  maxCommands = 0;
        for (int i = 0; i < 7; ++i)
        {
            if (! sender.sendUmp (maxUmp, kMaxUmpWordsPerCommand))
                allSent = false;
            if (! nextDatagram (d))
                everyOneArrived = false;
            else
            {
                if (d.bytes > kMaxDatagram) everOversized = true;
                if (d.commands > maxCommands) maxCommands = d.commands;
            }
        }
        check (allSent, "every send succeeds; a full history never blocks new data");
        check (everyOneArrived, "...and each one actually reaches the wire");
        check (! everOversized, "no datagram exceeds 1400 bytes (5.1.1)");
        check (maxCommands >= 2 && maxCommands < 6,
               "...achieved by dropping the oldest repeats, not by sending none");
        check (d.commands >= 1 && d.words[d.commands - 1] == kMaxUmpWordsPerCommand,
               "...and the NEW command is never the one sacrificed");

        sender.setSentUmpHistory (slots, 2);
    }

    //-- idle declarations carry the repeats, then stop -----------------------
    std::puts ("\nEntering an idle period (7.2.2 + 7.2.1)");
    {
        PosixUdp iSock, iWatch;
        std::uint16_t iPort = 0, iwPort = 0;
        iSock.bind (0, iPort); iWatch.bind (0, iwPort);
        Platform iPlat { &iSock, &clock, nullptr };
        Recorder iRec;
        Session idler (iPlat, Role::host, &iRec, "Idler", "IDLER-1");
        Session::Timing it;
        it.pingIntervalMs = 100000; it.timeoutMs = 600000;
        it.idleDeclareMs = 40; it.idleDeclareCount = 5;
        idler.setTiming (it);
        SentUmpSlot iSlots[2];
        idler.setSentUmpHistory (iSlots, 2);
        idler.listen();

        Endpoint iEp {}; std::strcpy (iEp.address, "127.0.0.1"); iEp.port = iPort;
        {
            std::uint8_t b[16]; Writer w (b, sizeof b);
            w.writeSignature(); w.writeHeader (Command::invitation, 0, 0);
            iWatch.send (iEp, b, w.size());
        }
        for (int i = 0; i < 200 && idler.state() != State::established; ++i) { idler.tick(); usleep (500); }
        { std::uint8_t in[kMaxDatagram]; Endpoint f; while (iWatch.receive (in, sizeof in, f) > 0) {} }

        std::uint32_t m[2] = { 0x40906000u, 0x44440000u };
        idler.sendUmp (m, 2);
        idler.sendUmp (m, 2);

        int zeroWithRepeats = 0, zeroBare = 0;
        for (int i = 0; i < 4000; ++i)
        {
            std::uint8_t in[kMaxDatagram]; Endpoint f;
            const int n = iWatch.receive (in, sizeof in, f);
            if (n > 0)
            {
                int cmds = 0; bool hasZero = false;
                parseDatagram (in, std::size_t (n), [&] (const ParsedCommand& c) {
                    if (c.code != Command::umpData) return;
                    ++cmds;
                    if (c.payloadWords == 0) hasZero = true;
                });
                if (hasZero) { if (cmds > 1) ++zeroWithRepeats; else ++zeroBare; }
            }
            idler.tick(); usleep (500);
        }
        check (zeroWithRepeats == 2,
               "the first 2 idle declarations carry the FEC repeats (one per repeat)");
        check (zeroBare == 3, "...and the remaining 3 go out bare");
        check (zeroWithRepeats + zeroBare == 5, "5 declarations in total, as configured");
    }

    printf ("\n%s: FEC sending (%d/%d)\n", passes == checks ? "PASS" : "FAIL", passes, checks);
    return passes == checks ? 0 : 1;
}
