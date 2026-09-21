/*
    nm2_cli — a small, complete Network MIDI 2.0 endpoint built on libnetmidi2.

    Where nm2_bench is a diagnostic that runs one session and prints the bytes, this
    is the shape of a real application: it advertises itself over mDNS, accepts
    several Clients on one port, dials out to another device, and plays something.

    It exists to be read as much as run. Everything below the adapters is ordinary
    library use, and the adapters are the parts the library deliberately does not
    ship -- a socket, a clock, and an mDNS responder.

    Usage:
      nm2_cli --listen [port]              be a Host, advertise, accept Clients
      nm2_cli --connect <target>           be a Client, dial out and play
      nm2_cli --listen --connect <target>  both at once (spec 8)

    `target` is either host:port (203.0.113.50:5004) or a name to discover: we browse
    _midi2._udp and match it against each host's UMP Endpoint Name or Product Instance
    Id -- the two fields a user can actually see, and the two the spec puts in TXT.
    (Deliberately not the PTR service instance name: §4.2 says that one is internal
    and "should not be displayed to the user", so matching on it would be a trap.)

    Run with --help for the full option list.

    Build:  cmake -S . -B build -DNETMIDI2_BUILD_TOOLS=ON && cmake --build build
*/

#include "netmidi2/Session.h"
#include "netmidi2/HostPort.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cctype>
#include <cstdarg>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(__APPLE__) || defined(HAVE_DNS_SD)
 #include <dns_sd.h>
 #define NM2_HAVE_BONJOUR 1
#endif

using namespace netmidi2;

//==============================================================================
// Logging. Verbose by default (level 1); --quiet drops to 0, -v raises to 2.
static int  logLevel = 1;
static void say  (const char* fmt, ...) __attribute__ ((format (printf, 1, 2)));
static void note (const char* fmt, ...) __attribute__ ((format (printf, 1, 2)));

static void say (const char* fmt, ...)
{
    if (logLevel < 1) return;
    va_list a; va_start (a, fmt); vprintf (fmt, a); va_end (a); fflush (stdout);
}
static void note (const char* fmt, ...)   // level 2: the chatty stuff
{
    if (logLevel < 2) return;
    va_list a; va_start (a, fmt); vprintf (fmt, a); va_end (a); fflush (stdout);
}

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

//==============================================================================
//  Adapter 1 of 3: the UDP socket.
//==============================================================================
struct PosixUdp : IUdpSocket
{
    int         fd = -1;
    const char* tag = "sock";

    bool bind (std::uint16_t desiredPort, std::uint16_t& boundPortOut) override
    {
        fd = ::socket (AF_INET, SOCK_DGRAM, 0);
        if (fd < 0) return false;
        ::fcntl (fd, F_SETFL, O_NONBLOCK);
        int yes = 1; ::setsockopt (fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);

        sockaddr_in a {};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl (INADDR_ANY);
        a.sin_port = htons (desiredPort);
        if (::bind (fd, (sockaddr*) &a, sizeof a) != 0) return false;

        sockaddr_in got {}; socklen_t gl = sizeof got;
        ::getsockname (fd, (sockaddr*) &got, &gl);
        boundPortOut = ntohs (got.sin_port);
        return true;
    }

    int send (const Endpoint& to, const std::uint8_t* d, std::size_t len) override
    {
        sockaddr_in a {};
        a.sin_family = AF_INET;
        a.sin_port = htons (to.port);
        if (::inet_pton (AF_INET, to.address, &a.sin_addr) != 1) return -1;
        const int n = (int) ::sendto (fd, d, len, 0, (sockaddr*) &a, sizeof a);
        if (n > 0) dump ("TX ->", to, d, len);
        return n;
    }

    int receive (std::uint8_t* buf, std::size_t cap, Endpoint& from) override
    {
        sockaddr_in a {}; socklen_t al = sizeof a;
        const ssize_t n = ::recvfrom (fd, buf, cap, 0, (sockaddr*) &a, &al);
        if (n < 0) return 0;                       // nothing pending, not an error
        ::inet_ntop (AF_INET, &a.sin_addr, from.address, sizeof from.address);
        from.port = ntohs (a.sin_port);
        dump ("RX <-", from, buf, (std::size_t) n);
        return (int) n;
    }

    void dump (const char* dir, const Endpoint& ep, const std::uint8_t* d, std::size_t len) const
    {
        if (logLevel < 2) return;
        printf ("    [%s] %s %s:%u %zu bytes", tag, dir, ep.address, ep.port, len);
        if (len >= 8) printf ("  cmd 0x%02X", d[4]);
        printf ("\n");
        fflush (stdout);
    }
};

//==============================================================================
//  Adapter 2 of 3: the clock.
//==============================================================================
struct PosixClock : IClock
{
    std::uint32_t nowMs() override
    {
        timeval tv; ::gettimeofday (&tv, nullptr);
        return std::uint32_t (tv.tv_sec * 1000ull + tv.tv_usec / 1000);
    }
};

//==============================================================================
//  Adapter 3 of 3: mDNS, over Bonjour.
//
//  This is the one the library refuses to ship, because multicast, record encoding
//  and a TTL cache look completely different on Bonjour, Avahi and Zephyr. It is
//  about 150 lines, which is a fair estimate of what the equivalent costs elsewhere.
//
//  Honest limitation: DNSServiceRegister gives no control over the record TTL, so
//  this adapter cannot honour §4.6's "not longer than one minute" recommendation --
//  mDNSResponder picks its own. A production adapter that cares should drive
//  DNSServiceRegisterRecord, or the platform equivalent, and set it explicitly.
//==============================================================================
#if NM2_HAVE_BONJOUR

class BonjourDiscovery : public IDiscovery
{
public:
    ~BonjourDiscovery() override { stopAdvertising(); stopBrowsing(); }

    void advertise (const char* serviceInstanceName, std::uint16_t port,
                    const char* umpEndpointName, const char* productInstanceId) override
    {
        stopAdvertising();

        TXTRecordRef txt;
        TXTRecordCreate (&txt, 0, nullptr);
        TXTRecordSetValue (&txt, "UMPEndpointName",
                           (std::uint8_t) std::strlen (umpEndpointName), umpEndpointName);
        TXTRecordSetValue (&txt, "ProductInstanceId",
                           (std::uint8_t) std::strlen (productInstanceId), productInstanceId);

        const DNSServiceErrorType e =
            DNSServiceRegister (&registerRef, 0, 0, serviceInstanceName, kServiceType,
                                nullptr, nullptr, htons (port),
                                TXTRecordGetLength (&txt), TXTRecordGetBytesPtr (&txt),
                                nullptr, nullptr);
        TXTRecordDeallocate (&txt);

        if (e != kDNSServiceErr_NoError)
        {
            printf ("  mDNS: advertise failed (%d)\n", (int) e);
            registerRef = nullptr;
        }
        else
            say ("  mDNS: advertising \"%s\" on port %u\n", serviceInstanceName, port);
    }

    void stopAdvertising() override
    {
        if (registerRef != nullptr) { DNSServiceRefDeallocate (registerRef); registerRef = nullptr; }
    }

    void startBrowsing() override
    {
        if (browseRef != nullptr) return;
        const DNSServiceErrorType e =
            DNSServiceBrowse (&browseRef, 0, 0, kServiceType, nullptr, onBrowse, this);
        if (e != kDNSServiceErr_NoError) { browseRef = nullptr; return; }
        say ("  mDNS: browsing %s\n", kServiceType);
    }

    void stopBrowsing() override
    {
        if (browseRef != nullptr) { DNSServiceRefDeallocate (browseRef); browseRef = nullptr; }
        for (auto& r : resolves)
            if (r.ref != nullptr) { DNSServiceRefDeallocate (r.ref); r.ref = nullptr; }
    }

    bool poll (DiscoveryEvent& kindOut, DiscoveredHost& hostOut) override
    {
        pump();
        if (qHead == qTail) return false;
        kindOut = queue[qHead].kind;
        hostOut = queue[qHead].host;
        qHead = (qHead + 1) % kQueue;
        return true;
    }

private:
    static constexpr int kQueue    = 32;
    static constexpr int kResolves = 16;
    static constexpr int kCache    = 32;

    struct Ev    { DiscoveryEvent kind; DiscoveredHost host; };
    struct Res   { DNSServiceRef ref = nullptr; bool done = false; };
    struct Known { char instance[128] = {}; DiscoveredHost host; bool used = false; };

    DNSServiceRef registerRef = nullptr;
    DNSServiceRef browseRef   = nullptr;
    Res           resolves[kResolves];
    Known         cache[kCache];
    Ev            queue[kQueue];
    int           qHead = 0, qTail = 0;

    void push (DiscoveryEvent k, const DiscoveredHost& h)
    {
        const int next = (qTail + 1) % kQueue;
        if (next == qHead) return;            // full: drop, rather than block
        queue[qTail].kind = k;
        queue[qTail].host = h;
        qTail = next;
    }

    // Service every Bonjour fd that has something waiting, without blocking.
    void pump()
    {
        for (;;)
        {
            fd_set fds; FD_ZERO (&fds);
            int maxFd = -1;
            auto watch = [&] (DNSServiceRef r) {
                if (r == nullptr) return;
                const int fd = DNSServiceRefSockFD (r);
                if (fd < 0) return;
                FD_SET (fd, &fds);
                if (fd > maxFd) maxFd = fd;
            };
            watch (browseRef);
            for (auto& r : resolves) watch (r.ref);

            if (maxFd < 0) break;

            timeval zero { 0, 0 };
            if (::select (maxFd + 1, &fds, nullptr, nullptr, &zero) <= 0) break;

            bool progressed = false;
            auto service = [&] (DNSServiceRef r) {
                if (r == nullptr) return;
                const int fd = DNSServiceRefSockFD (r);
                if (fd >= 0 && FD_ISSET (fd, &fds)) { DNSServiceProcessResult (r); progressed = true; }
            };
            service (browseRef);
            for (auto& r : resolves) service (r.ref);

            // A resolve is one-shot; tear it down once its callback has fired. Doing
            // it here rather than inside the callback keeps the ref valid for the
            // duration of the call that is using it.
            for (auto& r : resolves)
                if (r.done && r.ref != nullptr) { DNSServiceRefDeallocate (r.ref); r.ref = nullptr; r.done = false; }

            if (! progressed) break;
        }
    }

    Known* findCache (const char* instance)
    {
        for (auto& k : cache)
            if (k.used && std::strcmp (k.instance, instance) == 0) return &k;
        return nullptr;
    }
    Known* freeCache()
    {
        for (auto& k : cache) if (! k.used) return &k;
        return nullptr;
    }

    static void DNSSD_API onBrowse (DNSServiceRef, DNSServiceFlags flags, std::uint32_t ifIndex,
                                    DNSServiceErrorType err, const char* name,
                                    const char* type, const char* domain, void* ctx)
    {
        if (err != kDNSServiceErr_NoError) return;
        auto* self = static_cast<BonjourDiscovery*> (ctx);

        if ((flags & kDNSServiceFlagsAdd) != 0)
        {
            for (auto& r : self->resolves)
            {
                if (r.ref != nullptr) continue;
                if (DNSServiceResolve (&r.ref, 0, ifIndex, name, type, domain,
                                       onResolve, self) != kDNSServiceErr_NoError)
                    r.ref = nullptr;
                return;
            }
            return;                                  // no free slot; ignore this one
        }

        // Removal. Only the identifying fields are promised on a `lost`, but we can
        // do better than that from the cache, so a picker can match it to its row.
        if (Known* k = self->findCache (name))
        {
            self->push (DiscoveryEvent::lost, k->host);
            k->used = false;
        }
        else
        {
            DiscoveredHost h {};
            std::snprintf (h.umpEndpointName, sizeof h.umpEndpointName, "%s", name);
            self->push (DiscoveryEvent::lost, h);
        }
    }

    static void DNSSD_API onResolve (DNSServiceRef ref, DNSServiceFlags, std::uint32_t,
                                     DNSServiceErrorType err, const char* fullName,
                                     const char* hostTarget, std::uint16_t portNet,
                                     std::uint16_t txtLen, const unsigned char* txt, void* ctx)
    {
        auto* self = static_cast<BonjourDiscovery*> (ctx);
        for (auto& r : self->resolves) if (r.ref == ref) r.done = true;
        if (err != kDNSServiceErr_NoError) return;

        DiscoveredHost h {};
        h.port = ntohs (portNet);

        // TXT (§4.4). Absent fields stay empty rather than being invented.
        std::uint8_t len = 0;
        if (const void* v = TXTRecordGetValuePtr (txtLen, txt, "UMPEndpointName", &len))
            std::snprintf (h.umpEndpointName, sizeof h.umpEndpointName, "%.*s", (int) len, (const char*) v);
        if (const void* v = TXTRecordGetValuePtr (txtLen, txt, "ProductInstanceId", &len))
            std::snprintf (h.productInstanceId, sizeof h.productInstanceId, "%.*s", (int) len, (const char*) v);

        /*  SRV gives a hostname, and the Session needs an address. Resolving it here
            is a blocking call, which is normally forbidden in a poll() -- it is
            tolerable only because it happens once per discovered device, on a name
            the local responder has already cached. A GUI app should use
            DNSServiceGetAddrInfo and stay fully asynchronous. */
        char host[256];
        std::snprintf (host, sizeof host, "%s", hostTarget);
        if (const std::size_t n = std::strlen (host))
            if (host[n - 1] == '.') host[n - 1] = '\0';     // strip the FQDN dot

        addrinfo hints {}; hints.ai_family = AF_INET; hints.ai_socktype = SOCK_DGRAM;
        addrinfo* res = nullptr;
        if (::getaddrinfo (host, nullptr, &hints, &res) == 0 && res != nullptr)
        {
            auto* a = (sockaddr_in*) res->ai_addr;
            ::inet_ntop (AF_INET, &a->sin_addr, h.address, sizeof h.address);
            ::freeaddrinfo (res);
        }
        else
            return;                                  // unresolvable; not a usable host

        // Remember it so a later removal can be reported with its real identity.
        char instance[128];
        std::snprintf (instance, sizeof instance, "%s", fullName);
        if (char* dot = std::strstr (instance, "._midi2._udp")) *dot = '\0';

        Known* k = self->findCache (instance);
        if (k == nullptr) k = self->freeCache();
        if (k != nullptr)
        {
            std::snprintf (k->instance, sizeof k->instance, "%s", instance);
            k->host = h;
            k->used = true;
        }

        self->push (DiscoveryEvent::found, h);
    }
};

#endif // NM2_HAVE_BONJOUR

//==============================================================================
//  The arpeggiator. Pure UMP generation -- nothing here knows about the network.
//==============================================================================
struct Arpeggiator
{
    std::uint8_t  notes[16] = {};
    int           noteCount = 0;
    int           octaves   = 1;
    int           bpm       = 120;
    int           division  = 2;      // 1 = quarter, 2 = eighth, 4 = sixteenth
    int           gatePct   = 80;
    std::uint8_t  group     = 0;
    std::uint8_t  channel   = 0;
    bool          midi1     = false;
    enum class Pattern { up, down, updown } pattern = Pattern::up;

    int           step        = 0;
    bool          noteIsOn    = false;
    std::uint8_t  soundingNote = 0;
    std::uint32_t nextEventMs = 0;

    std::uint32_t stepMs() const
    { return std::uint32_t (60000 / (bpm > 0 ? bpm : 120) / (division > 0 ? division : 1)); }

    int sequenceLength() const { return noteCount * (octaves > 0 ? octaves : 1); }

    std::uint8_t noteAt (int index) const
    {
        const int len = sequenceLength();
        if (len == 0) return 60;

        int i = index % len;
        if (pattern == Pattern::down)
            i = len - 1 - i;
        else if (pattern == Pattern::updown && len > 1)
        {
            const int cycle = i % (2 * len - 2 > 0 ? 2 * len - 2 : 1);
            i = cycle < len ? cycle : (2 * len - 2 - cycle);
        }

        const int octave = i / noteCount;
        const int pitch  = int (notes[i % noteCount]) + 12 * octave;
        return std::uint8_t (pitch > 127 ? 127 : pitch);
    }

    // Build a Note On/Off as UMP. MIDI 2.0 Channel Voice (mt=4) by default: two
    // words, with a 16-bit velocity instead of MIDI 1.0's 7 bits. That resolution
    // is the entire reason for preferring this transport, so it is the default.
    int buildNote (std::uint32_t* out, bool on, std::uint8_t note, std::uint16_t velocity) const
    {
        const std::uint8_t status = on ? 0x9 : 0x8;
        if (midi1)
        {
            const std::uint8_t v7 = std::uint8_t (velocity >> 9);   // 16-bit -> 7-bit
            out[0] = (std::uint32_t (0x2) << 28)
                   | (std::uint32_t (group & 0xF) << 24)
                   | (std::uint32_t (status) << 20)
                   | (std::uint32_t (channel & 0xF) << 16)
                   | (std::uint32_t (note & 0x7F) << 8)
                   | std::uint32_t (on ? (v7 ? v7 : 1) : 0);
            return 1;
        }
        out[0] = (std::uint32_t (0x4) << 28)
               | (std::uint32_t (group & 0xF) << 24)
               | (std::uint32_t (status) << 20)
               | (std::uint32_t (channel & 0xF) << 16)
               | (std::uint32_t (note & 0x7F) << 8);
        out[1] = std::uint32_t (velocity) << 16;
        return 2;
    }
};

//==============================================================================
//  Listener: logs what arrives and what the session is doing.
//==============================================================================
struct CliListener : ISessionListener
{
    const char* who = "session";
    int  umpIn = 0;
    bool established = false;

    explicit CliListener (const char* w) : who (w) {}

    void onUmpReceived (const std::uint32_t* words, std::uint8_t count) override
    {
        ++umpIn;
        if (logLevel < 1) return;
        const unsigned mt = count ? (words[0] >> 28) : 0;
        printf ("  [%s] UMP in : %u word(s), mt=0x%X", who, count, mt);
        for (std::uint8_t i = 0; i < count && i < 4; ++i) printf (" %08X", words[i]);
        printf ("\n"); fflush (stdout);
    }

    void onStateChanged (State s) override
    {
        established = (s == State::established);
        say ("  [%s] state -> %s\n", who, stateName (s));
    }

    void onUmpLost (std::uint16_t seq) override
    { say ("  [%s] UMP %u lost -- an All Notes Off would be wise here\n", who, seq); }

    void onSessionReset() override
    { say ("  [%s] session reset: counters back to zero\n", who); }

    void onInvitationPending() override
    { say ("  [%s] host is deciding (maybe asking a user) -- waiting\n", who); }
};

//==============================================================================
//  Argument parsing helpers
//==============================================================================
static volatile std::sig_atomic_t stopRequested = 0;
static void onSigint (int) { stopRequested = 1; }

static bool parseNoteName (const char* s, int& out)
{
    static const int semitone[7] = { 9, 11, 0, 2, 4, 5, 7 };   // A B C D E F G
    if (s == nullptr || *s == '\0') return false;

    const char c = char (std::toupper ((unsigned char) s[0]));
    if (c < 'A' || c > 'G') return false;
    int value = semitone[c - 'A'];
    int i = 1;
    if (s[i] == '#') { ++value; ++i; }
    else if (s[i] == 'b') { --value; ++i; }

    if (s[i] == '\0') return false;
    char* end = nullptr;
    const long octave = std::strtol (s + i, &end, 10);
    if (end == s + i) return false;

    const long midi = (octave + 1) * 12 + value;    // C4 = 60
    if (midi < 0 || midi > 127) return false;
    out = int (midi);
    return true;
}

// "60,64,67" | "C4,E4,G4" | "C4:maj7"
static bool parseChord (const char* spec, Arpeggiator& arp)
{
    arp.noteCount = 0;

    if (const char* colon = std::strchr (spec, ':'))
    {
        char root[16] = {};
        const std::size_t n = std::size_t (colon - spec);
        if (n == 0 || n >= sizeof root) return false;
        std::memcpy (root, spec, n);

        int base = 0;
        if (! parseNoteName (root, base))
        {
            char* end = nullptr;
            base = int (std::strtol (root, &end, 10));
            if (end == root) return false;
        }

        struct Q { const char* name; int n; int iv[5]; };
        static const Q qualities[] = {
            { "maj",  3, { 0, 4, 7 } },        { "min",  3, { 0, 3, 7 } },
            { "dim",  3, { 0, 3, 6 } },        { "aug",  3, { 0, 4, 8 } },
            { "sus2", 3, { 0, 2, 7 } },        { "sus4", 3, { 0, 5, 7 } },
            { "maj7", 4, { 0, 4, 7, 11 } },    { "min7", 4, { 0, 3, 7, 10 } },
            { "7",    4, { 0, 4, 7, 10 } },    { "dim7", 4, { 0, 3, 6, 9 } },
            { "min9", 5, { 0, 3, 7, 10, 14 } },{ "maj9", 5, { 0, 4, 7, 11, 14 } },
        };
        for (const auto& q : qualities)
            if (std::strcmp (colon + 1, q.name) == 0)
            {
                for (int i = 0; i < q.n; ++i)
                {
                    const int p = base + q.iv[i];
                    if (p >= 0 && p <= 127) arp.notes[arp.noteCount++] = std::uint8_t (p);
                }
                return arp.noteCount > 0;
            }
        return false;
    }

    char buf[256];
    std::snprintf (buf, sizeof buf, "%s", spec);
    for (char* tok = std::strtok (buf, ","); tok != nullptr; tok = std::strtok (nullptr, ","))
    {
        while (*tok == ' ') ++tok;
        if (arp.noteCount >= 16) break;

        int midi = 0;
        if (parseNoteName (tok, midi)) { arp.notes[arp.noteCount++] = std::uint8_t (midi); continue; }

        char* end = nullptr;
        const long v = std::strtol (tok, &end, 10);
        if (end == tok || v < 0 || v > 127) return false;
        arp.notes[arp.noteCount++] = std::uint8_t (v);
    }
    return arp.noteCount > 0;
}

static void usage()
{
    std::puts (
"nm2_cli -- a Network MIDI 2.0 endpoint built on libnetmidi2\n"
"\n"
"ROLE (at least one required)\n"
"  --listen [port]     act as a Host on <port> (default 5004; 0 = any free port)\n"
"  --connect <target>  act as a Client. <target> is host:port, or a device name to\n"
"                      discover: matched against UMP Endpoint Name or Product\n"
"                      Instance Id from the mDNS TXT record.\n"
"  Giving both runs both at once, on separate ports (spec 8).\n"
"\n"
"IDENTITY\n"
"  --name <s>          UMP Endpoint Name, advertised and sent in invitations\n"
"  --pid <s>           Product Instance Id (stable per device; a serial is ideal)\n"
"  --instance <s>      mDNS service instance name (default: derived from --pid)\n"
"  --no-advertise      listen, but publish nothing over mDNS\n"
"  --clients <n>       simultaneous Clients the Host accepts (default 4)\n"
"  --secret <s>        shared secret: required as Host, offered as Client\n"
"\n"
"WHAT TO PLAY\n"
"  --chord <spec>      60,64,67 | C4,E4,G4 | C4:maj7        (default C4:maj)\n"
"  --bpm <n>           tempo (default 120)\n"
"  --div <n>           1=quarter 2=eighth 4=sixteenth (default 2)\n"
"  --octaves <n>       octaves the arpeggio spans (default 1)\n"
"  --gate <pct>        note length as %% of the step (default 80)\n"
"  --pattern <p>       up | down | updown (default up)\n"
"  --group <n>         UMP group 0-15 (default 0)\n"
"  --channel <n>       channel 0-15 (default 0)\n"
"  --midi1             send MIDI 1.0 channel voice UMP instead of MIDI 2.0\n"
"  --silent            establish sessions but play nothing\n"
"\n"
"OUTPUT\n"
"  --quiet             errors and results only\n"
"  -v, --verbose       add per-datagram logging\n"
"  -h, --help          this\n"
"\n"
"Ctrl-C stops the notes, closes every session properly, and waits for the Byes.");
}

//==============================================================================
int main (int argc, char** argv)
{
    const char* endpointName = "libnetmidi2 CLI";
    const char* productId    = "LIBNETMIDI2-CLI-1";
    const char* instanceName = nullptr;
    const char* secret       = nullptr;
    const char* target       = nullptr;

    bool          doListen    = false;
    bool          advertising = true;
    bool          silent      = false;
    std::uint16_t listenPort  = 5004;
    int           clientSlots = 4;

    Arpeggiator arp;
    parseChord ("C4:maj", arp);

    for (int i = 1; i < argc; ++i)
    {
        const char* a = argv[i];
        auto next = [&] () -> const char* { return i + 1 < argc ? argv[++i] : ""; };
        auto nextInt = [&] (int def) { const char* v = next(); return *v ? std::atoi (v) : def; };

        if (! std::strcmp (a, "--listen"))
        {
            doListen = true;
            if (i + 1 < argc && argv[i + 1][0] != '-')
                listenPort = (std::uint16_t) std::atoi (argv[++i]);
        }
        else if (! std::strcmp (a, "--connect"))      target = next();
        else if (! std::strcmp (a, "--name"))         endpointName = next();
        else if (! std::strcmp (a, "--pid"))          productId = next();
        else if (! std::strcmp (a, "--instance"))     instanceName = next();
        else if (! std::strcmp (a, "--secret"))       secret = next();
        else if (! std::strcmp (a, "--no-advertise")) advertising = false;
        else if (! std::strcmp (a, "--clients"))      clientSlots = nextInt (4);
        else if (! std::strcmp (a, "--bpm"))          arp.bpm = nextInt (120);
        else if (! std::strcmp (a, "--div"))          arp.division = nextInt (2);
        else if (! std::strcmp (a, "--octaves"))      arp.octaves = nextInt (1);
        else if (! std::strcmp (a, "--gate"))         arp.gatePct = nextInt (80);
        else if (! std::strcmp (a, "--group"))        arp.group = (std::uint8_t) nextInt (0);
        else if (! std::strcmp (a, "--channel"))      arp.channel = (std::uint8_t) nextInt (0);
        else if (! std::strcmp (a, "--midi1"))        arp.midi1 = true;
        else if (! std::strcmp (a, "--silent"))       silent = true;
        else if (! std::strcmp (a, "--quiet"))        logLevel = 0;
        else if (! std::strcmp (a, "-v") || ! std::strcmp (a, "--verbose")) logLevel = 2;
        else if (! std::strcmp (a, "--pattern"))
        {
            const char* p = next();
            arp.pattern = ! std::strcmp (p, "down")   ? Arpeggiator::Pattern::down
                        : ! std::strcmp (p, "updown") ? Arpeggiator::Pattern::updown
                                                      : Arpeggiator::Pattern::up;
        }
        else if (! std::strcmp (a, "--chord"))
        {
            if (! parseChord (next(), arp)) { std::puts ("bad --chord"); return 2; }
        }
        else if (! std::strcmp (a, "-h") || ! std::strcmp (a, "--help")) { usage(); return 0; }
        else { printf ("unknown option: %s\n\n", a); usage(); return 2; }
    }

    if (! doListen && target == nullptr) { usage(); return 2; }
    if (clientSlots < 1)  clientSlots = 1;
    if (clientSlots > 32) clientSlots = 32;

    char derivedInstance[128];
    if (instanceName == nullptr)
    {
        // §4.2: the instance name is internal and should be stable, and the spec
        // suggests building it from the Product Instance Id. It is NOT the name to
        // show a user -- that is the UMP Endpoint Name, in TXT.
        std::snprintf (derivedInstance, sizeof derivedInstance, "%s", productId);
        instanceName = derivedInstance;
    }

    if (! isValidUmpEndpointName (endpointName))
    { printf ("--name is not a valid UMP Endpoint Name (<= %zu bytes UTF-8)\n",
              kMaxUmpEndpointNameBytes); return 2; }
    if (! isValidProductInstanceId (productId))
    { printf ("--pid is not a valid Product Instance Id (<= %zu bytes, ASCII 32-126)\n",
              kMaxProductInstanceIdBytes); return 2; }

    std::signal (SIGINT, onSigint);
    PosixClock clock;

#if NM2_HAVE_BONJOUR
    BonjourDiscovery discovery;
    IDiscovery* disco = &discovery;
#else
    IDiscovery* disco = nullptr;
    if (advertising || (target != nullptr && std::strchr (target, ':') == nullptr))
        std::puts ("  note: built without Bonjour -- mDNS unavailable, use host:port");
#endif

    say ("=== nm2_cli ===\n");
    say ("endpoint   : \"%s\"  pid \"%s\"\n", endpointName, productId);

    //-- Host half ------------------------------------------------------------
    PosixUdp      hostSock;  hostSock.tag = "host";
    CliListener*  hostListeners[32] = {};
    Session*      hostSessions[32]  = {};
    HostPort*     hostPort = nullptr;
    std::uint16_t hostBound = 0;
    Platform      hostPlat {};

    if (doListen)
    {
        if (! hostSock.bind (listenPort, hostBound)) { std::puts ("FAIL: host bind"); return 1; }
        hostPlat = Platform { &hostSock, &clock, disco, nullptr };

        for (int i = 0; i < clientSlots; ++i)
        {
            char tag[32]; std::snprintf (tag, sizeof tag, "host%d", i);
            hostListeners[i] = new CliListener (strdup (tag));
            hostSessions[i]  = new Session (hostPlat, Role::host, hostListeners[i],
                                            endpointName, productId);
            if (secret != nullptr) hostSessions[i]->requireAuthentication (secret);
        }

        hostPort = new HostPort (hostPlat, hostSessions, std::size_t (clientSlots));
        hostPort->listen();
        say ("host       : listening on :%u, %d client slot(s)\n", hostBound, clientSlots);

        if (advertising && disco != nullptr)
        {
            if (! hostPort->advertise (instanceName, hostBound, endpointName, productId))
                std::puts ("  warning: advertise refused (check --name / --pid limits)");
        }
    }

    //-- Client half ----------------------------------------------------------
    PosixUdp     clientSock; clientSock.tag = "clnt";
    CliListener  clientListener ("client");
    Session*     clientSession = nullptr;
    Platform     clientPlat {};
    bool         dialling = false, dialled = false;
    char         wantedInstance[128] = {};
    std::uint32_t browseStartedMs = 0;

    if (target != nullptr)
    {
        std::uint16_t clientBound = 0;
        if (! clientSock.bind (0, clientBound)) { std::puts ("FAIL: client bind"); return 1; }
        clientPlat = Platform { &clientSock, &clock, disco, nullptr };
        clientSession = new Session (clientPlat, Role::client, &clientListener,
                                     endpointName, productId);
        if (secret != nullptr) clientSession->setSharedSecret (secret);

        const char* colon = std::strrchr (target, ':');
        if (colon != nullptr && colon[1] != '\0')
        {
            Endpoint ep {};
            const std::size_t hostLen = std::size_t (colon - target);
            char hostPart[128];
            std::snprintf (hostPart, sizeof hostPart, "%.*s", (int) hostLen, target);

            addrinfo hints {}; hints.ai_family = AF_INET; hints.ai_socktype = SOCK_DGRAM;
            addrinfo* res = nullptr;
            if (::getaddrinfo (hostPart, nullptr, &hints, &res) != 0 || res == nullptr)
            { printf ("FAIL: cannot resolve %s\n", hostPart); return 1; }
            auto* a4 = (sockaddr_in*) res->ai_addr;
            ::inet_ntop (AF_INET, &a4->sin_addr, ep.address, sizeof ep.address);
            ::freeaddrinfo (res);
            ep.port = (std::uint16_t) std::atoi (colon + 1);

            say ("client     : inviting %s -> %s:%u from :%u\n", target, ep.address, ep.port, clientBound);
            clientSession->connect (ep);
            dialled = true;
        }
        else if (disco != nullptr)
        {
            std::snprintf (wantedInstance, sizeof wantedInstance, "%s", target);
            disco->startBrowsing();
            dialling = true;
            browseStartedMs = clock.nowMs();
            say ("client     : browsing for a device named \"%s\" from :%u\n",
                 wantedInstance, clientBound);
        }
        else
        { std::puts ("FAIL: --connect needs host:port when mDNS is unavailable"); return 1; }
    }

    say ("\n");

    //-- Run ------------------------------------------------------------------
    /*  Collect whichever Sessions are currently Established. The arpeggiator does
        not care which half of the program they came from -- a note goes to every
        peer that is listening. */
    auto forEachEstablished = [&] (void (*fn) (Session&, void*), void* ctx) {
        if (hostPort != nullptr)
            for (int i = 0; i < clientSlots; ++i)
                if (hostSessions[i]->state() == State::established) fn (*hostSessions[i], ctx);
        if (clientSession != nullptr && clientSession->state() == State::established)
            fn (*clientSession, ctx);
    };

    struct SendCtx { const std::uint32_t* words; std::uint8_t count; int sent; };
    auto sendToAll = [&] (const std::uint32_t* words, std::uint8_t count) {
        SendCtx ctx { words, count, 0 };
        forEachEstablished ([] (Session& s, void* c) {
            auto* x = static_cast<SendCtx*> (c);
            if (s.sendUmp (x->words, x->count)) ++x->sent;
        }, &ctx);
        return ctx.sent;
    };

    bool announcedPlaying = false;
    bool closing = false;
    std::uint32_t closeStartedMs = 0;

    for (;;)
    {
        if (hostPort != nullptr)   hostPort->tick();
        if (clientSession != nullptr) clientSession->tick();

        const std::uint32_t now = clock.nowMs();

        //-- discovery: find the host we were asked to dial --------------------
        if (dialling && ! dialled && disco != nullptr)
        {
            DiscoveryEvent kind; DiscoveredHost found;
            while (disco->poll (kind, found))
            {
                if (kind != DiscoveryEvent::found) continue;
                say ("  found: \"%s\" (%s) at %s:%u\n", found.umpEndpointName,
                     found.productInstanceId, found.address, found.port);

                if (std::strcmp (found.umpEndpointName, wantedInstance) == 0
                    || std::strcmp (found.productInstanceId, wantedInstance) == 0)
                {
                    say ("  -> matches \"%s\", inviting\n", wantedInstance);
                    clientSession->connect (found);
                    dialled = true;
                    break;
                }
            }
            if (! dialled && now - browseStartedMs > 15000)
            {
                printf ("FAIL: no host named \"%s\" appeared within 15s\n", wantedInstance);
                stopRequested = 1;
                dialling = false;
            }
        }

        //-- arpeggiate --------------------------------------------------------
        if (! silent && ! closing && ! stopRequested)
        {
            const bool anyone = [&] {
                bool found = false;
                if (hostPort != nullptr)
                    for (int i = 0; i < clientSlots; ++i)
                        if (hostSessions[i]->state() == State::established) found = true;
                if (clientSession != nullptr && clientSession->state() == State::established)
                    found = true;
                return found;
            }();

            if (anyone)
            {
                if (! announcedPlaying)
                {
                    say ("\n--- playing: %d note(s), %d octave(s), %d bpm, %s ---\n",
                         arp.noteCount, arp.octaves, arp.bpm, arp.midi1 ? "MIDI 1.0 UMP" : "MIDI 2.0 UMP");
                    announcedPlaying = true;
                    arp.nextEventMs = now;
                }

                if ((std::int32_t) (now - arp.nextEventMs) >= 0)
                {
                    std::uint32_t ump[2];
                    if (arp.noteIsOn)
                    {
                        const int n = arp.buildNote (ump, false, arp.soundingNote, 0);
                        sendToAll (ump, std::uint8_t (n));
                        arp.noteIsOn = false;
                        const std::uint32_t rest = arp.stepMs() - (arp.stepMs() * std::uint32_t (arp.gatePct) / 100);
                        arp.nextEventMs = now + (rest ? rest : 1);
                    }
                    else
                    {
                        arp.soundingNote = arp.noteAt (arp.step++);
                        const int n = arp.buildNote (ump, true, arp.soundingNote, 0xC000);
                        const int sent = sendToAll (ump, std::uint8_t (n));
                        arp.noteIsOn = true;
                        note ("  note on  %3u -> %d peer(s)\n", arp.soundingNote, sent);
                        arp.nextEventMs = now + arp.stepMs() * std::uint32_t (arp.gatePct) / 100;
                    }
                }
            }
            else
                announcedPlaying = false;
        }

        //-- Ctrl-C: stop the note, then close properly -------------------------
        if (stopRequested && ! closing)
        {
            say ("\n--- stopping ---\n");

            if (arp.noteIsOn)
            {
                std::uint32_t ump[2];
                const int n = arp.buildNote (ump, false, arp.soundingNote, 0);
                sendToAll (ump, std::uint8_t (n));
                arp.noteIsOn = false;
            }

            /*  close() is NOT synchronous: it enters Pending Bye and repeats the Bye
                until the peer replies or the timeout expires. Tearing the socket down
                here would leave every peer waiting out its own timeout, so the loop
                below keeps ticking until each session reaches `closed`. */
            if (hostPort != nullptr)
                for (int i = 0; i < clientSlots; ++i) hostSessions[i]->close();
            if (clientSession != nullptr) clientSession->close();

            if (disco != nullptr) { disco->stopAdvertising(); disco->stopBrowsing(); }

            closing = true;
            closeStartedMs = now;
        }

        if (closing)
        {
            bool allDone = true;
            if (hostPort != nullptr)
                for (int i = 0; i < clientSlots; ++i)
                {
                    const State s = hostSessions[i]->state();
                    if (s != State::closed && s != State::idle) allDone = false;
                }
            if (clientSession != nullptr)
            {
                const State s = clientSession->state();
                if (s != State::closed && s != State::idle) allDone = false;
            }

            if (allDone)            { say ("all sessions closed cleanly\n"); break; }
            if (now - closeStartedMs > 5000) { say ("giving up waiting for Bye replies\n"); break; }
        }

        ::usleep (1000);
    }

    //-- Teardown -------------------------------------------------------------
    delete hostPort;
    for (int i = 0; i < clientSlots; ++i) { delete hostSessions[i]; delete hostListeners[i]; }
    delete clientSession;
    if (hostSock.fd >= 0)   ::close (hostSock.fd);
    if (clientSock.fd >= 0) ::close (clientSock.fd);

    return 0;
}
