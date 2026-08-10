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

    // 5. Graceful close.
    client.close(); pump (50);
    check (host.state()==State::closed && client.state()==State::closed, "graceful Bye -> both Closed");

    printf ("\n%s: session loopback (%d/%d)\n", passes==checks?"PASS":"FAIL", passes, checks);
    return passes==checks ? 0 : 1;
}
