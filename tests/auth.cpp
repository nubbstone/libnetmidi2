/*
    libnetmidi2 — authentication (§6.7–6.10).

    A Host that wants a password challenges an Invitation with a 16-byte CryptoNonce;
    the Client returns SHA-256(nonce || secret); the Host recomputes and compares.

    The spec publishes worked examples for BOTH digests, which makes the part that
    has to be byte-identical between implementations directly checkable -- so those
    go first. The framing of the challenge commands (0x12/0x13) does not have that
    luxury: their tables are self-contradictory, and the reading used here is derived
    and argued in Protocol.h and PROTOCOL.md §3.7 rather than transcribed.

    ICrypto is injected, so this test supplies a reference SHA-256 and a seeded,
    deliberately predictable RNG. A predictable nonce would be a serious flaw in a
    real implementation -- it is exactly what the injected interface exists to let a
    platform get right -- but in a test it is what makes the digests reproducible.
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

//== a reference SHA-256, for the test only ===================================
namespace ref {
struct Sha256
{
    std::uint32_t h[8]; std::uint64_t len; std::uint8_t buf[64]; std::size_t n;
    static std::uint32_t ror (std::uint32_t x, int c) { return (x >> c) | (x << (32 - c)); }
    void init()
    {
        static const std::uint32_t iv[8] = { 0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
                                             0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19 };
        for (int i = 0; i < 8; ++i) h[i] = iv[i];
        len = 0; n = 0;
    }
    void block (const std::uint8_t* p)
    {
        static const std::uint32_t k[64] = {
          0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
          0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
          0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
          0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
          0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
          0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
          0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
          0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2 };
        std::uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = (std::uint32_t(p[i*4])<<24)|(std::uint32_t(p[i*4+1])<<16)|(std::uint32_t(p[i*4+2])<<8)|p[i*4+3];
        for (int i = 16; i < 64; ++i)
        {
            const std::uint32_t s0 = ror(w[i-15],7)^ror(w[i-15],18)^(w[i-15]>>3);
            const std::uint32_t s1 = ror(w[i-2],17)^ror(w[i-2],19)^(w[i-2]>>10);
            w[i] = w[i-16] + s0 + w[i-7] + s1;
        }
        std::uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
        for (int i = 0; i < 64; ++i)
        {
            const std::uint32_t S1 = ror(e,6)^ror(e,11)^ror(e,25);
            const std::uint32_t ch = (e&f)^((~e)&g);
            const std::uint32_t t1 = hh + S1 + ch + k[i] + w[i];
            const std::uint32_t S0 = ror(a,2)^ror(a,13)^ror(a,22);
            const std::uint32_t mj = (a&b)^(a&c)^(b&c);
            const std::uint32_t t2 = S0 + mj;
            hh=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
        }
        h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=hh;
    }
    void update (const std::uint8_t* p, std::size_t l)
    {
        len += l;
        while (l)
        {
            const std::size_t take = (64 - n < l) ? 64 - n : l;
            std::memcpy (buf + n, p, take); n += take; p += take; l -= take;
            if (n == 64) { block (buf); n = 0; }
        }
    }
    void final (std::uint8_t out[32])
    {
        const std::uint64_t bits = len * 8;
        std::uint8_t pad = 0x80; update (&pad, 1);
        std::uint8_t z = 0;
        while (n != 56) update (&z, 1);
        std::uint8_t be[8];
        for (int i = 0; i < 8; ++i) be[i] = std::uint8_t (bits >> (56 - i*8));
        update (be, 8);
        for (int i = 0; i < 8; ++i)
        {
            out[i*4]   = std::uint8_t (h[i] >> 24); out[i*4+1] = std::uint8_t (h[i] >> 16);
            out[i*4+2] = std::uint8_t (h[i] >> 8);  out[i*4+3] = std::uint8_t (h[i]);
        }
    }
};
} // namespace ref

struct TestCrypto : ICrypto
{
    std::uint32_t seed = 0x12345678u;
    bool entropyAvailable = true;
    void sha256 (const std::uint8_t* d, std::size_t n, std::uint8_t out[32]) override
    { ref::Sha256 s; s.init(); s.update (d, n); s.final (out); }
    bool randomBytes (std::uint8_t* out, std::size_t n) override
    {
        if (! entropyAvailable) return false;
        for (std::size_t i = 0; i < n; ++i)
        { seed = seed * 1664525u + 1013904223u; out[i] = std::uint8_t (seed >> 24); }
        return true;
    }
};

struct Users : IUserPasswordStore
{
    bool lookupPassword (const char* u, char* out, std::size_t cap) override
    {
        if (std::strcmp (u, "Rosa") == 0) { std::snprintf (out, cap, "RPBqBno"); return true; }
        return false;
    }
};

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
    int umpCount = 0;
    void onUmpReceived (const std::uint32_t*, std::uint8_t) override { ++umpCount; }
};

static int checks = 0, passes = 0;
static void check (bool ok, const char* label)
{ ++checks; if (ok) ++passes; printf ("  %s  %s\n", ok ? "OK  " : "FAIL", label); }

int main()
{
    setvbuf (stdout, nullptr, _IONBF, 0);
    std::puts ("Authentication (spec 6.7-6.10)\n");
    TestCrypto crypto;

    //== the published digest vectors =========================================
    std::puts ("Digest construction, against the spec's worked examples");
    {
        // §6.9: nonce "nUWrn*@#$hjfwnkL" + secret "5483"
        const char* n1 = "nUWrn*@#$hjfwnkL";
        std::uint8_t nonce[kCryptoNonceBytes];
        std::memcpy (nonce, n1, kCryptoNonceBytes);
        const std::uint8_t want1[32] = {
            0x67,0x6E,0xBE,0x82,0x58,0x7C,0xEC,0xA8, 0xF8,0x2F,0xC3,0x33,0xD7,0x87,0x95,0x1E,
            0xEC,0x2B,0x00,0xAD,0x31,0x61,0x3C,0xC4, 0xDB,0x18,0xCF,0x27,0x37,0x3A,0xFB,0x82 };
        std::uint8_t got[32];
        check (makeAuthDigest (crypto, nonce, "5483", got), "shared-secret digest builds");
        check (std::memcmp (got, want1, 32) == 0, "...and matches the spec's 6.9 example byte for byte");

        // §6.10: nonce "XI|~=NNRVaD;XCPL" + "Rosa" + "RPBqBno"
        const char* n2 = "XI|~=NNRVaD;XCPL";
        std::memcpy (nonce, n2, kCryptoNonceBytes);
        const std::uint8_t want2[32] = {
            0x3A,0x2D,0x5E,0xBE,0xEF,0x92,0xE8,0x46, 0x35,0x35,0xC2,0x7F,0xB4,0xB7,0xB3,0xE2,
            0xD1,0xAC,0xF5,0x7A,0x84,0x4C,0x6D,0x08, 0x08,0xE0,0x41,0xC1,0x02,0xB7,0xFF,0x1A };
        check (makeUserAuthDigest (crypto, nonce, "Rosa", "RPBqBno", got), "user digest builds");
        check (std::memcmp (got, want2, 32) == 0, "...and matches the spec's 6.10 example byte for byte");

        // The comparison must not short-circuit on the first differing byte.
        std::uint8_t a[32] = {}, b[32] = {};
        check (digestsEqual (a, b), "equal digests compare equal");
        b[31] = 1; check (! digestsEqual (a, b), "a difference in the LAST byte is caught");
        b[31] = 0; b[0] = 1; check (! digestsEqual (a, b), "...as is one in the first");
    }

    //== nonce generation =====================================================
    std::puts ("\nCryptoNonce");
    {
        std::uint8_t n1[kCryptoNonceBytes], n2[kCryptoNonceBytes];
        check (makeCryptoNonce (crypto, n1), "a nonce is produced");
        check (makeCryptoNonce (crypto, n2), "and another");
        check (std::memcmp (n1, n2, kCryptoNonceBytes) != 0, "...which differs from the first");

        bool printable = true;
        for (std::size_t i = 0; i < kCryptoNonceBytes; ++i)
            if (n1[i] < 33 || n1[i] > 126) printable = false;
        check (printable, "...and is printable ASCII, as Table 14 types the field");

        crypto.entropyAvailable = false;
        check (! makeCryptoNonce (crypto, n1),
               "no entropy means NO nonce -- never a guessable one");
        crypto.entropyAvailable = true;
    }

    //== end to end, shared secret ============================================
    std::puts ("\nShared-secret handshake, end to end");
    PosixClock clock;
    {
        PosixUdp hSock, cSock;
        std::uint16_t hp = 0, cp = 0;
        check (hSock.bind (0, hp) && cSock.bind (0, cp), "sockets bound");
        Platform hPlat { &hSock, &clock, nullptr, &crypto };
        Platform cPlat { &cSock, &clock, nullptr, &crypto };
        Recorder hRec, cRec;
        Session host   (hPlat, Role::host,   &hRec, "Secure Host", "SEC-HOST-1");
        Session client (cPlat, Role::client, &cRec, "Client",      "CLI-1");
        host.requireAuthentication ("5483");
        client.setSharedSecret ("5483");
        host.listen();

        Endpoint hEp {}; std::strcpy (hEp.address, "127.0.0.1"); hEp.port = hp;
        client.connect (hEp);
        auto pump = [&] (int n) { for (int i = 0; i < n; ++i) { host.tick(); client.tick(); usleep (500); } };

        bool sawAuthenticating = false;
        for (int i = 0; i < 1200 && client.state() != State::established; ++i)
        { pump (1); if (client.state() == State::authenticating) sawAuthenticating = true; }

        check (sawAuthenticating, "the client passes through Authentication Required");
        check (client.state()==State::established, "the correct secret establishes a session");
        check (host.state()==State::established, "...on both ends");

        std::uint32_t note[2] = { 0x40903C00u, 0xFFFF0000u };
        client.sendUmp (note, 2); pump (100);
        check (hRec.umpCount == 1, "...and UMP flows over the authenticated session");
    }

    //== the wrong secret =====================================================
    std::puts ("\nThe wrong secret");
    {
        PosixUdp hSock, cSock;
        std::uint16_t hp = 0, cp = 0;
        hSock.bind (0, hp); cSock.bind (0, cp);
        Platform hPlat { &hSock, &clock, nullptr, &crypto };
        Platform cPlat { &cSock, &clock, nullptr, &crypto };
        Recorder hRec, cRec;
        Session host   (hPlat, Role::host,   &hRec, "Secure Host", "SEC-HOST-1");
        Session client (cPlat, Role::client, &cRec, "Client",      "CLI-1");
        Session::Timing t; t.authFailDelayMs = 20; t.inviteTimeoutMs = 3000;
        host.setTiming (t); client.setTiming (t);
        host.requireAuthentication ("5483");
        client.setSharedSecret ("0000");          // wrong
        host.listen();

        Endpoint hEp {}; std::strcpy (hEp.address, "127.0.0.1"); hEp.port = hp;
        client.connect (hEp);
        for (int i = 0; i < 1500; ++i) { host.tick(); client.tick(); usleep (500); }

        check (client.state() != State::established, "the wrong secret does NOT establish");
        check (host.state() != State::established,   "...on the host either");
    }

    //== a client that cannot authenticate at all =============================
    std::puts ("\nA client with no credentials");
    {
        PosixUdp hSock, raw;
        std::uint16_t hp = 0, rp = 0;
        hSock.bind (0, hp); raw.bind (0, rp);
        Platform hPlat { &hSock, &clock, nullptr, &crypto };
        Recorder hRec;
        Session host (hPlat, Role::host, &hRec, "Secure Host", "SEC-HOST-1");
        host.requireAuthentication ("5483");
        host.listen();
        Endpoint hEp {}; std::strcpy (hEp.address, "127.0.0.1"); hEp.port = hp;

        // Invitation advertising NO auth capability (Capabilities = 0).
        std::uint8_t b[128]; Writer w (b, sizeof b);
        const char* nm = "Plain"; const char* pid = "PLAIN-1";
        w.writeSignature();
        writeInvitation (w, 0x00, nm, std::strlen (nm), pid, std::strlen (pid));
        raw.send (hEp, b, w.size());
        for (int i = 0; i < 400; ++i) { host.tick(); usleep (500); }

        bool sawBye45 = false, sawAccepted = false;
        std::uint8_t in[512]; Endpoint f; int n = 0;
        while ((n = raw.receive (in, sizeof in, f)) > 0)
            parseDatagram (in, std::size_t (n), [&] (const ParsedCommand& c) {
                if (c.code == Command::bye && c.data1() == std::uint8_t (ByeReason::noMatchingAuth))
                    sawBye45 = true;
                if (c.code == Command::invitationReplyAccepted) sawAccepted = true;
            });
        check (! sawAccepted, "a client that cannot authenticate is NOT accepted");
        check (sawBye45, "...it gets Bye 0x45, No Matching Authentication Method (6.4)");
        check (host.state() != State::established, "...and the host stays unestablished");
    }

    //== user authentication ==================================================
    std::puts ("\nUser authentication (6.8 / 6.10)");
    {
        Users users;
        PosixUdp hSock, cSock;
        std::uint16_t hp = 0, cp = 0;
        hSock.bind (0, hp); cSock.bind (0, cp);
        Platform hPlat { &hSock, &clock, nullptr, &crypto };
        Platform cPlat { &cSock, &clock, nullptr, &crypto };
        Recorder hRec, cRec;
        Session host   (hPlat, Role::host,   &hRec, "User Host", "USR-HOST-1");
        Session client (cPlat, Role::client, &cRec, "Client",    "CLI-1");
        host.requireUserAuthentication (&users);
        client.setUserCredentials ("Rosa", "RPBqBno");
        host.listen();

        Endpoint hEp {}; std::strcpy (hEp.address, "127.0.0.1"); hEp.port = hp;
        client.connect (hEp);
        for (int i = 0; i < 1500 && client.state() != State::established; ++i)
        { host.tick(); client.tick(); usleep (500); }
        check (client.state()==State::established, "the right user/password establishes");

        // ...and an unknown user does not.
        PosixUdp c2Sock; std::uint16_t c2p = 0; c2Sock.bind (0, c2p);
        Platform c2Plat { &c2Sock, &clock, nullptr, &crypto };
        Recorder c2Rec;
        Session client2 (c2Plat, Role::client, &c2Rec, "Client2", "CLI-2");
        Session::Timing t; t.authFailDelayMs = 20; t.inviteTimeoutMs = 2000;
        client2.setTiming (t);
        client2.setUserCredentials ("Mallory", "guess");
        client2.connect (hEp);
        for (int i = 0; i < 1500; ++i) { host.tick(); client2.tick(); usleep (500); }
        check (client2.state() != State::established, "an unknown user does not establish");
    }

    //== no crypto injected ===================================================
    std::puts ("\nWith no ICrypto supplied");
    {
        PosixUdp hSock, cSock;
        std::uint16_t hp = 0, cp = 0;
        hSock.bind (0, hp); cSock.bind (0, cp);
        Platform hPlat { &hSock, &clock, nullptr, nullptr };   // no crypto
        Platform cPlat { &cSock, &clock, nullptr, nullptr };
        Recorder hRec, cRec;
        Session host   (hPlat, Role::host,   &hRec, "Host", "H-1");
        Session client (cPlat, Role::client, &cRec, "Cli",  "C-1");
        host.requireAuthentication ("5483");    // asked for, but impossible
        host.listen();
        Endpoint hEp {}; std::strcpy (hEp.address, "127.0.0.1"); hEp.port = hp;
        client.connect (hEp);
        for (int i = 0; i < 800 && client.state() != State::established; ++i)
        { host.tick(); client.tick(); usleep (500); }
        check (client.state()==State::established,
               "a host that cannot make a nonce does not pretend to authenticate");
    }

    printf ("\n%s: auth (%d/%d)\n", passes == checks ? "PASS" : "FAIL", passes, checks);
    return passes == checks ? 0 : 1;
}
