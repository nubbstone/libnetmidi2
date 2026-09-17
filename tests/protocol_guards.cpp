/*
    libnetmidi2 — Protocol.h guard tests.

    Companion to conformance_vectors.cpp. That file asks "do we agree with the spec
    on well-formed packets?"; this one asks "what do we do with input that is NOT
    well-formed?" — which matters just as much, because every byte here arrives from
    the network and a receiver does not get to assume any of it.

    These are the `return false` paths that no test reached before: Writer overflow,
    the UMP Data word-count limit, a bad signature, and a truncated command. A guard
    that is never exercised is a guard nobody knows is wired up.

    Pure unit test: Protocol.h only — no sockets, no clock, no Session.

    Related: the receive-side counterpart of the word-count limit lives in
    Session.h (see session_loopback.cpp, "oversized:"), where an unchecked Payload
    Length was a stack buffer overflow.
*/

#include "netmidi2/Protocol.h"

#include <cstdio>
#include <cstring>

using namespace netmidi2;

static int checks = 0, passes = 0;

static void check (bool ok, const char* label)
{
    ++checks;
    if (ok) ++passes;
    printf ("  %s  %s\n", ok ? "OK  " : "FAIL", label);
}

// A canary-wrapped buffer: proves a rejected write does not scribble past `cap`.
struct GuardedBuffer
{
    static constexpr std::size_t kPad = 16;
    static constexpr std::uint8_t kCanary = 0xCD;

    std::uint8_t storage[256];
    std::size_t  usable;

    explicit GuardedBuffer (std::size_t usableBytes) noexcept : usable (usableBytes)
    {
        std::memset (storage, kCanary, sizeof storage);
    }

    std::uint8_t* data() noexcept { return storage; }

    bool canaryIntact() const noexcept
    {
        for (std::size_t i = usable; i < usable + kPad && i < sizeof storage; ++i)
            if (storage[i] != kCanary)
                return false;
        return true;
    }
};

int main()
{
    std::puts ("Protocol.h guard tests (malformed / boundary input)\n");

    //-- Writer: refuses to overrun a caller-owned buffer ---------------------
    std::puts ("Writer — buffer overflow");
    {
        // Capacity 8: signature (4) + header (4) fits exactly, the next word does not.
        GuardedBuffer gb (8);
        Writer w (gb.data(), gb.usable);

        check (w.writeSignature(), "signature fits in an 8-byte buffer");
        check (w.writeHeader (Command::ping, 1, 0), "header fits too (8/8 used)");
        check (w.size() == 8, "exactly 8 bytes written");
        check (w.ok(), "no overflow flagged yet");

        check (! w.u32 (0xDEADBEEFu), "the 9th..12th byte is refused");
        check (! w.ok(), "overflow is now flagged via ok()");
        check (w.size() == 8, "a refused write does not advance the length");
        check (gb.canaryIntact(), "a refused write does not touch memory past capacity");

        // Once overflowed it must stay refusing, not recover on a smaller write.
        check (! w.u8 (0x01), "a later small write is still refused");
        check (gb.canaryIntact(), "...and still writes nothing past capacity");
    }

    //-- Writer: a partial command cannot be half-emitted ---------------------
    std::puts ("\nWriter — a command that cannot fit is rejected as a whole");
    {
        GuardedBuffer gb (10);              // signature + header + 2 bytes of a word
        Writer w (gb.data(), gb.usable);
        const std::uint32_t ump[2] = { 0x40904000u, 0x12340000u };

        const bool built = w.writeSignature() && writeUmpData (w, 0x0001, ump, 2);
        check (! built, "writeUmpData reports failure when the payload will not fit");
        check (! w.ok(), "the Writer is flagged overflowed");
        check (gb.canaryIntact(), "nothing was written past capacity");
    }

    //-- UMP Data: the §7.1 word-count limit ---------------------------------
    std::puts ("\nwriteUmpData — §7.1 Table 29 payload limit (0...64 words)");
    {
        static std::uint32_t words[256];
        for (std::size_t i = 0; i < 256; ++i)
            words[i] = 0xA5A5A5A5u;

        {
            std::uint8_t buf[kMaxDatagram];
            Writer w (buf, sizeof buf);
            check (w.writeSignature() && writeUmpData (w, 0, words, kMaxUmpWordsPerCommand),
                   "64 words (the maximum) is accepted");
        }
        {
            std::uint8_t buf[kMaxDatagram];
            Writer w (buf, sizeof buf);
            w.writeSignature();
            check (! writeUmpData (w, 0, words, kMaxUmpWordsPerCommand + 1),
                   "65 words is refused");
        }
        {
            std::uint8_t buf[kMaxDatagram];
            Writer w (buf, sizeof buf);
            w.writeSignature();
            check (! writeUmpData (w, 0, words, 255),
                   "255 words (the largest a Payload Length byte can encode) is refused");
        }
        {
            // Zero-length UMP Data is legal — §7.2.1 uses it to signal an idle period.
            std::uint8_t buf[kMaxDatagram];
            Writer w (buf, sizeof buf);
            const bool ok = w.writeSignature() && writeUmpData (w, 0x0005, words, 0);
            check (ok && w.size() == 8, "zero-length UMP Data is legal (§7.2.1) and is 8 bytes");
        }
    }

    //-- parseDatagram: signature -------------------------------------------
    std::puts ("\nparseDatagram — signature (§5.2)");
    {
        int called = 0;
        auto count = [&] (const ParsedCommand&) { ++called; };

        const std::uint8_t bad[] = { 0x4D, 0x49, 0x44, 0x4A,     // "MIDJ"
                                     0x20, 0x01, 0x00, 0x00,
                                     0x00, 0x00, 0x00, 0x01 };
        called = 0;
        check (! parseDatagram (bad, sizeof bad, count), "a wrong signature is rejected");
        check (called == 0, "...and no command is delivered from it");

        // Truthful prefix, but too short to even hold the signature.
        const std::uint8_t tiny[] = { 0x4D, 0x49, 0x44 };
        called = 0;
        check (! parseDatagram (tiny, sizeof tiny, count), "a 3-byte datagram is rejected");
        check (called == 0, "...and delivers nothing");

        called = 0;
        check (! parseDatagram (bad, 0, count), "a zero-length datagram is rejected");

        // Signature alone is valid framing: zero commands, but not an error.
        const std::uint8_t sigOnly[] = { 0x4D, 0x49, 0x44, 0x49 };
        called = 0;
        check (parseDatagram (sigOnly, sizeof sigOnly, count), "signature with no commands is accepted");
        check (called == 0, "...and delivers no commands");
    }

    //-- parseDatagram: truncation ------------------------------------------
    std::puts ("\nparseDatagram — truncated command (§5.4)");
    {
        int called = 0;
        auto count = [&] (const ParsedCommand&) { ++called; };

        // Claims 4 words of payload; only 2 words are present.
        const std::uint8_t truncated[] = {
            0x4D, 0x49, 0x44, 0x49,
            0xFF, 0x04, 0x00, 0x01,
            0x45, 0x90, 0x40, 0x00,
            0x12, 0x34, 0x00, 0x00,
        };
        called = 0;
        check (! parseDatagram (truncated, sizeof truncated, count),
               "a command claiming more payload than is present is rejected");
        check (called == 0, "...and the truncated command itself is NOT delivered");

        // A good command followed by a truncated one: the good one is delivered
        // before the tear is found, and the call still reports failure. Worth
        // pinning down, because a caller that ignores the return value still sees
        // the valid prefix -- which is the behaviour Session relies on.
        const std::uint8_t goodThenTruncated[] = {
            0x4D, 0x49, 0x44, 0x49,
            0xFF, 0x01, 0x00, 0x01,
            0x10, 0xF8, 0x00, 0x00,
            0xFF, 0x08, 0x00, 0x02,   // claims 8 words, none follow
        };
        called = 0;
        check (! parseDatagram (goodThenTruncated, sizeof goodThenTruncated, count),
               "good-then-truncated reports failure");
        check (called == 1, "...but the valid leading command was still delivered");

        // Trailing bytes too short to be a header are ignored, not an error.
        const std::uint8_t trailingStub[] = {
            0x4D, 0x49, 0x44, 0x49,
            0xFF, 0x01, 0x00, 0x01,
            0x10, 0xF8, 0x00, 0x00,
            0xAA, 0xBB,               // 2 stray bytes: cannot be a header
        };
        called = 0;
        check (parseDatagram (trailingStub, sizeof trailingStub, count),
               "trailing bytes shorter than a header are ignored, not an error");
        check (called == 1, "...and the valid command is still delivered");
    }

    //-- parseDatagram: a 255-word claim is framing-legal --------------------
    std::puts ("\nparseDatagram — a 255-word claim parses; bounding it is the caller's job");
    {
        // This is the exact shape that overflowed Session::deliverUmp. It is NOT
        // malformed framing -- the payload really is there -- so parseDatagram
        // accepts it, which is why the length check has to live in the command
        // handler. Pinned here so nobody "fixes" the wrong layer.
        static std::uint8_t big[4 + 4 + 255 * 4];
        Writer w (big, sizeof big);
        w.writeSignature();
        w.writeHeader (Command::umpData, 255, 0x7777);
        for (int i = 0; i < 255; ++i)
            w.u32 (0xDEADBEEFu);

        std::uint8_t sawWords = 0;
        const bool ok = parseDatagram (big, w.size(),
                                       [&] (const ParsedCommand& c) { sawWords = c.payloadWords; });
        check (ok, "a truthful 255-word command is well-formed framing");
        check (sawWords == 255, "parseDatagram reports the full 255, unclamped");
        check (sawWords > kMaxUmpWordsPerCommand,
               "...which exceeds the §7.1 limit — the handler must reject it");
    }

    printf ("\n%s: protocol guards (%d/%d)\n",
            passes == checks ? "PASS" : "FAIL", passes, checks);
    return passes == checks ? 0 : 1;
}
