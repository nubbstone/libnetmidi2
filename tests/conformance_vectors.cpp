/*
    libnetmidi2 conformance vectors — byte-for-byte checks against the example UDP
    packets published in MA M2-124-UM v1.0, Appendix A.1 (Figures 12-15, p.50-51).

    Why this test exists, when session_loopback.cpp already passes:

    The loopback test runs OUR client against OUR host, so a wire-format error is
    invisible to it -- both ends make the same mistake and it cancels out. Get the
    endianness backwards, mis-size a field, pad wrong, and the loopback still goes
    green while nothing on the LAN can talk to us. Only comparing against bytes the
    spec publishes can catch that, and interop with a DIFFERENT implementation (the
    Teensy's Zephyr netmidi2) is the entire reason this library is spec-faithful.

    Each vector is checked in BOTH directions: we must build exactly those bytes,
    and we must parse exactly those bytes back into the right fields. A real peer
    does both, so a one-directional check would only prove half the contract.

    Pure unit test: includes Protocol.h only -- no sockets, no clock, no Session.
*/

#include "netmidi2/Protocol.h"

#include <cstdio>
#include <cstring>

using namespace netmidi2;

//== tiny harness (same shape as session_loopback.cpp) ========================
static int checks = 0, passes = 0;

static void check (bool ok, const char* label)
{
    ++checks;
    if (ok) ++passes;
    printf ("  %s  %s\n", ok ? "OK  " : "FAIL", label);
}

// Byte-exact comparison that says WHICH byte differs -- a bare pass/fail on a
// 28-byte packet is not much help when it goes red.
static void checkBytes (const std::uint8_t* got, std::size_t gotLen,
                        const std::uint8_t* want, std::size_t wantLen,
                        const char* label)
{
    const bool ok = (gotLen == wantLen) && std::memcmp (got, want, wantLen) == 0;
    check (ok, label);

    if (ok)
        return;

    if (gotLen != wantLen)
        printf ("        length: got %zu, want %zu\n", gotLen, wantLen);

    const std::size_t n = gotLen < wantLen ? gotLen : wantLen;
    for (std::size_t i = 0; i < n; ++i)
        if (got[i] != want[i])
            printf ("        byte %2zu (word %zu, lane %zu): got 0x%02X, want 0x%02X\n",
                    i, i / 4, i % 4, got[i], want[i]);
}

//== the vectors ==============================================================
// Transcribed from the figures in Appendix A.1. Do not "fix" these to match the
// code -- the spec is normative (see CLAUDE.md prime directive 2). If one fails,
// the library is wrong.

// A.1.1 / Figure 12 -- Invitation.
//   Capabilities 0x00, UMP Endpoint Name "MyDev", Product Instance Id "8shYe3h5".
//   Note per the spec: the name carries its 0x00 terminator and is padded to a
//   word; the product id already ends on a word boundary, so it is NOT padded.
static const std::uint8_t kInvitation[] = {
    0x4D, 0x49, 0x44, 0x49,   // "MIDI" signature
    0x01, 0x04, 0x02, 0x00,   // Invitation | pl=4 words | csd1=2 (name words) | csd2=0 (caps)
    0x4D, 0x79, 0x44, 0x65,   // 'M' 'y' 'D' 'e'
    0x76, 0x00, 0x00, 0x00,   // 'v' + terminator + padding
    0x38, 0x73, 0x68, 0x59,   // '8' 's' 'h' 'Y'
    0x65, 0x33, 0x68, 0x35,   // 'e' '3' 'h' '5'
};

// A.1.2 / Figure 13 -- UMP Data, Seq 16 (0x0010), one UMP: Timing Clock, Group 0.
static const std::uint8_t kUmpDataSingle[] = {
    0x4D, 0x49, 0x44, 0x49,
    0xFF, 0x01, 0x00, 0x10,   // UMP Data | pl=1 word | seq=0x0010
    0x10, 0xF8, 0x00, 0x00,   // Timing Clock
};

// A.1.3 / Figure 14 -- ONE UMP Data command carrying TWO UMP messages, Seq 17.
//   MIDI 2.0 Note On then Note Off, Group 6, Channel 1, note 0x40,
//   velocities 0x1234 and 0x0100.
static const std::uint8_t kUmpDataTwoMessages[] = {
    0x4D, 0x49, 0x44, 0x49,
    0xFF, 0x04, 0x00, 0x11,   // UMP Data | pl=4 words | seq=0x0011
    0x45, 0x90, 0x40, 0x00,   // Note On  word 0
    0x12, 0x34, 0x00, 0x00,   // Note On  word 1 (velocity 0x1234)
    0x45, 0x80, 0x40, 0x00,   // Note Off word 0
    0x01, 0x00, 0x00, 0x00,   // Note Off word 1 (velocity 0x0100)
};

// A.1.4 / Figure 15 -- the same two messages as TWO UMP Data commands packed into
//   one datagram (seq 0x3456 and 0x3457). This is the shape FEC uses, and it is
//   the case a parser that stops after the first command gets wrong.
static const std::uint8_t kUmpDataTwoCommands[] = {
    0x4D, 0x49, 0x44, 0x49,
    0xFF, 0x02, 0x34, 0x56,   // UMP Data | pl=2 words | seq=0x3456
    0x45, 0x90, 0x40, 0x00,
    0x12, 0x34, 0x00, 0x00,
    0xFF, 0x02, 0x34, 0x57,   // UMP Data | pl=2 words | seq=0x3457
    0x45, 0x80, 0x40, 0x00,
    0x01, 0x00, 0x00, 0x00,
};

// The UMP words above, in host order, for the build side.
static const std::uint32_t kTimingClock = 0x10F80000u;
static const std::uint32_t kNoteOn[2]   = { 0x45904000u, 0x12340000u };
static const std::uint32_t kNoteOff[2]  = { 0x45804000u, 0x01000000u };

//== parse-side helper ========================================================
// Collects what parseDatagram hands back, so the assertions can inspect it.
struct Collected
{
    static constexpr int kMax = 8;
    int           count = 0;
    Command       code[kMax] {};
    std::uint8_t  payloadWords[kMax] {};
    std::uint16_t cmdSpecific[kMax] {};
    std::uint32_t firstWord[kMax] {};
    std::uint8_t  data1[kMax] {};
    std::uint8_t  data2[kMax] {};

    void add (const ParsedCommand& c) noexcept
    {
        if (count >= kMax)
            return;
        code[count]         = c.code;
        payloadWords[count] = c.payloadWords;
        cmdSpecific[count]  = c.cmdSpecific;
        data1[count]        = c.data1();
        data2[count]        = c.data2();
        firstWord[count]    = c.payload ? get32 (c.payload) : 0u;
        ++count;
    }
};

static Collected parseAll (const std::uint8_t* datagram, std::size_t len, bool& okOut)
{
    Collected out;
    okOut = parseDatagram (datagram, len, [&] (const ParsedCommand& c) { out.add (c); });
    return out;
}

//== the test =================================================================
int main()
{
    std::puts ("M2-124-UM Appendix A.1 conformance vectors\n");

    //-- A.1.1 Invitation ----------------------------------------------------
    std::puts ("A.1.1 / Figure 12 — Invitation");
    {
        const char* name    = "MyDev";
        const char* product = "8shYe3h5";

        std::uint8_t buf[kMaxDatagram];
        Writer w (buf, sizeof buf);
        const bool built = w.writeSignature()
                        && writeInvitation (w, 0x00, name, std::strlen (name),
                                            product, std::strlen (product));
        check (built, "builds without overflow");
        checkBytes (buf, w.size(), kInvitation, sizeof kInvitation, "bytes match the spec");

        bool parsedOk = false;
        const Collected got = parseAll (kInvitation, sizeof kInvitation, parsedOk);
        check (parsedOk && got.count == 1, "parses to exactly one command");
        check (got.count == 1 && got.code[0] == Command::invitation, "parsed code is Invitation");
        check (got.count == 1 && got.payloadWords[0] == 4, "parsed payload length is 4 words");
        check (got.count == 1 && got.data1[0] == 2, "parsed csd1 is the name length (2 words)");
        check (got.count == 1 && got.data2[0] == 0, "parsed csd2 is Capabilities 0x00");
    }

    //-- A.1.2 UMP Data, one message ----------------------------------------
    std::puts ("\nA.1.2 / Figure 13 — UMP Data, one UMP message");
    {
        std::uint8_t buf[kMaxDatagram];
        Writer w (buf, sizeof buf);
        const bool built = w.writeSignature() && writeUmpData (w, 0x0010, &kTimingClock, 1);
        check (built, "builds without overflow");
        checkBytes (buf, w.size(), kUmpDataSingle, sizeof kUmpDataSingle, "bytes match the spec");

        bool parsedOk = false;
        const Collected got = parseAll (kUmpDataSingle, sizeof kUmpDataSingle, parsedOk);
        check (parsedOk && got.count == 1, "parses to exactly one command");
        check (got.count == 1 && got.code[0] == Command::umpData, "parsed code is UMP Data");
        check (got.count == 1 && got.cmdSpecific[0] == 0x0010, "parsed sequence number is 0x0010");
        check (got.count == 1 && got.firstWord[0] == kTimingClock, "parsed UMP word is the Timing Clock");
    }

    //-- A.1.3 one command, two UMP messages ---------------------------------
    std::puts ("\nA.1.3 / Figure 14 — one UMP Data command, two UMP messages");
    {
        const std::uint32_t both[4] = { kNoteOn[0], kNoteOn[1], kNoteOff[0], kNoteOff[1] };

        std::uint8_t buf[kMaxDatagram];
        Writer w (buf, sizeof buf);
        const bool built = w.writeSignature() && writeUmpData (w, 0x0011, both, 4);
        check (built, "builds without overflow");
        checkBytes (buf, w.size(), kUmpDataTwoMessages, sizeof kUmpDataTwoMessages,
                    "bytes match the spec");

        bool parsedOk = false;
        const Collected got = parseAll (kUmpDataTwoMessages, sizeof kUmpDataTwoMessages, parsedOk);
        check (parsedOk && got.count == 1, "two UMPs arrive as ONE command, not two");
        check (got.count == 1 && got.cmdSpecific[0] == 0x0011, "parsed sequence number is 0x0011");
        check (got.count == 1 && got.payloadWords[0] == 4, "parsed payload length is 4 words");
    }

    //-- A.1.4 two commands in one datagram ----------------------------------
    std::puts ("\nA.1.4 / Figure 15 — two UMP Data commands in one datagram");
    {
        std::uint8_t buf[kMaxDatagram];
        Writer w (buf, sizeof buf);
        const bool built = w.writeSignature()
                        && writeUmpData (w, 0x3456, kNoteOn,  2)
                        && writeUmpData (w, 0x3457, kNoteOff, 2);
        check (built, "builds without overflow");
        checkBytes (buf, w.size(), kUmpDataTwoCommands, sizeof kUmpDataTwoCommands,
                    "bytes match the spec");

        bool parsedOk = false;
        const Collected got = parseAll (kUmpDataTwoCommands, sizeof kUmpDataTwoCommands, parsedOk);
        check (parsedOk && got.count == 2, "both commands are walked, not just the first");
        check (got.count == 2 && got.cmdSpecific[0] == 0x3456, "first sequence number is 0x3456");
        check (got.count == 2 && got.cmdSpecific[1] == 0x3457, "second sequence number is 0x3457");
        check (got.count == 2 && got.firstWord[0] == kNoteOn[0],  "first command carries the Note On");
        check (got.count == 2 && got.firstWord[1] == kNoteOff[0], "second command carries the Note Off");
    }

    printf ("\n%s: conformance vectors (%d/%d)\n",
            passes == checks ? "PASS" : "FAIL", passes, checks);
    return passes == checks ? 0 : 1;
}
