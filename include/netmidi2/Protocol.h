/*
    libnetmidi2 — Network MIDI 2.0 (UDP) wire format.

    Portable, freestanding-friendly C++17: no exceptions, no RTTI, no heap, no STL
    containers, no platform headers. Just big-endian read/write over caller-owned
    byte buffers. This is the shared core both the macOS host and the Teensy/Zephyr
    firmware compile, so the framing is identical on both ends.

    See PROTOCOL.md (this folder) and MA spec M2-124-UM for field definitions.
*/

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "Auth.h"

namespace netmidi2
{

//==============================================================================
// Constants (PROTOCOL.md §1–2)

constexpr std::uint32_t kSignature   = 0x4D494449u; // "MIDI"
constexpr std::size_t   kMaxDatagram = 1400;        // §5.1.1 — never fragment
constexpr std::size_t   kHeaderBytes = 4;           // command packet header

// Max payload of ONE UMP Data command, in 32-bit words (§7.1, Table 29: pl is
// 0...64, "The length shall not exceed 64 words"). A receiver must enforce this on
// the way in as well as out: Payload Length is a byte off the wire, so an unchecked
// command can claim up to 255 words.
constexpr std::uint8_t  kMaxUmpWordsPerCommand = 64;

enum class Command : std::uint8_t
{
    umpData                 = 0xFF,
    invitation              = 0x01,
    invitationWithAuth      = 0x02,
    invitationWithUserAuth  = 0x03,
    invitationReplyAccepted = 0x10,
    invitationReplyPending  = 0x11,
    invitationReplyAuthReq  = 0x12,
    invitationReplyUserAuth = 0x13,
    ping                    = 0x20,
    pingReply               = 0x21,
    retransmitRequest       = 0x80,
    retransmitError         = 0x81,
    sessionReset            = 0x82,
    sessionResetReply       = 0x83,
    nak                     = 0x8F,
    bye                     = 0xF0,
    byeReply                = 0xF1,
};

/*  Bye reasons, complete, from §6.16 Table 27.

    Transcribed from the table rather than remembered: the previous short list had
    0x40 as "user rejected", which is wrong. 0x40 is "too many opened sessions" --
    the answer a full Host owes an Invitation it has no room for -- and the user's
    refusal is 0x42. The two are easy to confuse and mean opposite things to the
    peer: one says come back later, the other says you are not welcome.
*/
enum class ByeReason : std::uint8_t
{
    // — sent by either Client or Host —
    undefined             = 0x00,  // Unknown or Undefined
    userTerminated        = 0x01,  // User terminated session
    powerDown             = 0x02,
    tooManyMissingPackets = 0x03,  // cannot recover
    timeout               = 0x04,  // e.g. too many bad/missing ping responses
    sessionNotEstablished = 0x05,  // one end believes there is a Session, the other does not
    noPendingSession      = 0x06,
    protocolError         = 0x07,  // e.g. name / Product Instance Id missing from an Invitation

    // — Host to Client —
    tooManySessions       = 0x40,  // Invitation Failed: too many opened sessions
    authRejectedNoPrior   = 0x41,  // Invitation with Auth without a prior plain Invitation
    userDidNotAccept      = 0x42,  // Invitation Rejected: user did not accept session
    authFailed            = 0x43,  // Invitation Rejected: authentication failed
    usernameNotFound      = 0x44,  // Invitation Rejected: username not found
    noMatchingAuth        = 0x45,  // No Matching Authentication Method

    // — Client to Host —
    invitationCanceled    = 0x80,
};

// §7.2.4, Table 32.
enum class RetransmitError : std::uint8_t
{
    unknown             = 0x00,
    notInTransmitBuffer = 0x01,   // the requested Sequence Number is no longer held
};

// §6.15, Table 25.
enum class NakReason : std::uint8_t
{
    other                = 0x00,   // reason is in the Text Message field
    commandNotSupported  = 0x01,   // we do not implement that command (§5.5)
    commandNotExpected   = 0x02,   // supported, but not valid right now
    commandMalformed     = 0x03,   // missing payload / unparseable values
    badPingReply         = 0x20,   // Ping Reply carried the wrong Ping Id
};

//==============================================================================
// Big-endian scalar helpers (network byte order, §5.3).

inline void put16 (std::uint8_t* p, std::uint16_t v) noexcept
{
    p[0] = std::uint8_t (v >> 8); p[1] = std::uint8_t (v);
}

inline void put32 (std::uint8_t* p, std::uint32_t v) noexcept
{
    p[0] = std::uint8_t (v >> 24); p[1] = std::uint8_t (v >> 16);
    p[2] = std::uint8_t (v >> 8);  p[3] = std::uint8_t (v);
}

inline std::uint16_t get16 (const std::uint8_t* p) noexcept
{
    return std::uint16_t ((std::uint16_t (p[0]) << 8) | p[1]);
}

inline std::uint32_t get32 (const std::uint8_t* p) noexcept
{
    return (std::uint32_t (p[0]) << 24) | (std::uint32_t (p[1]) << 16)
         | (std::uint32_t (p[2]) << 8)  |  std::uint32_t (p[3]);
}

//==============================================================================
// Bounded writer — appends into a caller-owned buffer, never overruns.

class Writer
{
public:
    Writer (std::uint8_t* buffer, std::size_t capacity) noexcept
        : data (buffer), cap (capacity) {}

    std::size_t size() const noexcept { return len; }

    /*  True until some write did not fit. Every write also returns a bool, so this
        is a second, sticky channel for the same fact, and it earns its place twice:

        - It allows build-then-check for a datagram assembled from several commands,
          instead of threading a bool through each one. FEC (§7.2.2) packs repeated
          UMP Data commands into one datagram and will want exactly that.

        - It is the backstop. `overflowed` is set inside raw(), so a builder that
          forgets to propagate a failed write still cannot produce a datagram that
          passes this. Session's send paths check it last, so a truncated packet
          cannot reach the wire even if a future builder is careless.
    */
    bool        ok()   const noexcept { return ! overflowed; }

    // A datagram must open with the signature exactly once (§5.2).
    bool writeSignature() noexcept { return u32 (kSignature); }

    // Command Packet header (§5.4). payloadWords = payload length in 32-bit words.
    bool writeHeader (Command code, std::uint8_t payloadWords, std::uint16_t cmdSpecific) noexcept
    {
        return u8 (std::uint8_t (code)) && u8 (payloadWords) && u16 (cmdSpecific);
    }

    bool u8  (std::uint8_t v) noexcept  { return raw (&v, 1); }
    bool u16 (std::uint16_t v) noexcept { std::uint8_t b[2]; put16 (b, v); return raw (b, 2); }
    bool u32 (std::uint32_t v) noexcept { std::uint8_t b[4]; put32 (b, v); return raw (b, 4); }

    // Copy `n` bytes, then zero-pad up to the next 4-byte word boundary (§5.3).
    bool bytesPadded (const void* src, std::size_t n) noexcept
    {
        if (! raw (src, n))
            return false;
        while ((len & 3u) != 0)
            if (! u8 (0))
                return false;
        return true;
    }

    bool raw (const void* src, std::size_t n) noexcept
    {
        if (len + n > cap) { overflowed = true; return false; }
        std::memcpy (data + len, src, n);
        len += n;
        return true;
    }

private:
    std::uint8_t* data;
    std::size_t   cap;
    std::size_t   len = 0;
    bool          overflowed = false;
};

//==============================================================================
// Phase-1 command builders. Each assumes the signature was already written, and
// returns false if the buffer would overflow.

// UMP Data (§7.1). `ump` points at `wordCount` 32-bit words (already big-endian
// on the wire is handled here: we accept host-order words and serialise them BE).
inline bool writeUmpData (Writer& w, std::uint16_t sequenceNumber,
                          const std::uint32_t* words, std::uint8_t wordCount) noexcept
{
    if (wordCount > kMaxUmpWordsPerCommand)
        return false;
    if (! w.writeHeader (Command::umpData, wordCount, sequenceNumber))
        return false;
    for (std::uint8_t i = 0; i < wordCount; ++i)
        if (! w.u32 (words[i]))
            return false;
    return true;
}

/*  Several commands carry the same payload: a UMP Endpoint Name followed by a
    Product Instance Id, with csd1 = the name's length in 32-bit words. Invitation
    (§6.4 Table 10) and Invitation Reply: Accepted (§6.5 Table 12) are structurally
    identical; they differ only in the command code and in what csd2 means --
    Capabilities for the Invitation, Reserved (0) for the Reply. Invitation Reply:
    Pending (§6.6) has the same shape again when it arrives.

    Strings are UTF-8 (name) / ASCII (product id), not necessarily word-aligned;
    each is null-padded up to a word boundary here (§5.3).
*/
inline bool writeEndpointIdentity (Writer& w, Command code, std::uint8_t csd2,
                                   const char* name, std::size_t nameLen,
                                   const char* productId, std::size_t productLen) noexcept
{
    const std::uint8_t nameWords    = std::uint8_t ((nameLen    + 3) / 4);
    const std::uint8_t productWords = std::uint8_t ((productLen + 3) / 4);
    const std::uint8_t payloadWords = std::uint8_t (nameWords + productWords);
    const std::uint16_t csd = std::uint16_t ((std::uint16_t (nameWords) << 8) | csd2);

    return w.writeHeader (code, payloadWords, csd)
        && w.bytesPadded (name, nameLen)
        && w.bytesPadded (productId, productLen);
}

// Invitation (§6.4). csd2 is the Capabilities bitmap (Table 11).
inline bool writeInvitation (Writer& w, std::uint8_t capabilities,
                             const char* name, std::size_t nameLen,
                             const char* productId, std::size_t productLen) noexcept
{
    return writeEndpointIdentity (w, Command::invitation, capabilities,
                                  name, nameLen, productId, productLen);
}

// Invitation Reply: Accepted (§6.5). csd2 is Reserved and shall be 0 (Table 12).
inline bool writeInvitationAccepted (Writer& w,
                                     const char* name, std::size_t nameLen,
                                     const char* productId, std::size_t productLen) noexcept
{
    return writeEndpointIdentity (w, Command::invitationReplyAccepted, 0,
                                  name, nameLen, productId, productLen);
}

inline bool writePing (Writer& w, std::uint32_t pingId) noexcept
{
    return w.writeHeader (Command::ping, 1, 0) && w.u32 (pingId);
}

inline bool writePingReply (Writer& w, std::uint32_t pingId) noexcept
{
    return w.writeHeader (Command::pingReply, 1, 0) && w.u32 (pingId);
}

inline bool writeBye (Writer& w, ByeReason reason) noexcept
{
    return w.writeHeader (Command::bye, 0, std::uint16_t (std::uint16_t (std::uint8_t (reason)) << 8));
}

inline bool writeByeReply (Writer& w) noexcept
{
    return w.writeHeader (Command::byeReply, 0, 0);
}

/*  Invitation with Authentication (§6.9, Table 18). pl = 8, csd = 0 (Reserved),
    payload = the 32-byte SHA-256 digest. Clean table, nothing to interpret. */
inline bool writeInvitationWithAuth (Writer& w,
                                     const std::uint8_t digest[kAuthDigestBytes]) noexcept
{
    return w.writeHeader (Command::invitationWithAuth, 8, 0)
        && w.raw (digest, kAuthDigestBytes);
}

/*  Invitation with User Authentication (§6.10, Table 19). pl = 8..255, csd = 0,
    payload = 32-byte digest followed by the username (UTF-8), padded to a word. */
inline bool writeInvitationWithUserAuth (Writer& w,
                                         const std::uint8_t digest[kAuthDigestBytes],
                                         const char* username, std::size_t usernameLen) noexcept
{
    const std::size_t userWords = (usernameLen + 3) / 4;
    const std::size_t payloadWords = 8 + userWords;
    if (payloadWords > 255)
        return false;

    return w.writeHeader (Command::invitationWithUserAuth, std::uint8_t (payloadWords), 0)
        && w.raw (digest, kAuthDigestBytes)
        && w.bytesPadded (username, usernameLen);
}

/*  Invitation Reply: Authentication Required (§6.7 Table 14) and its User variant
    (§6.8 Table 16). Same shape; `code` selects which.

        csd1           = length of the UMP Endpoint Name, in 32-bit words
        csd2           = Authentication State (Table 15 / 17)
        payload        = 16-byte CryptoNonce, then the name, then the Product
                         Instance Id, each padded to a word

    THE TABLES ARE WRONG ABOUT THE LENGTHS, and this is the reading they actually
    support. Their Size column gives the name as `(csd1*4) - 16` and the product id
    as `(pl-csd1)*4 - 16`, which subtracts the 16-byte nonce twice and leaves the
    payload 16 bytes short of the pl*4 it must occupy -- at pl=6 that is 8 bytes of
    content in a 24-byte payload.

    The Description column instead says csd1 is "Length, in 32-bit words, of the UMP
    Endpoint Name", which with the nonce occupying the first 4 words gives
    pl = 4 + nameWords + productWords. That is exactly self-consistent, and the
    table's own stated ranges confirm it: minimum pl 6 = 4 + 1 + 1, and maximum
    pl 40 = 4 + 25 + 11, with csd1 1..25 and a 42-byte Product Instance Id being
    11 words. Neither number works under the Size-column reading.

    Table 14 also lists Payload Length as 2 bytes, which cannot be: §5.4 Table 7
    fixes every command header at 4 bytes as code(1) + pl(1) + csd(2).

    PROTOCOL.md §3.7 records all of this. Flagged rather than quietly chosen, because
    a peer that read the Size column literally will not interoperate, and when that
    happens this is the first place to look.
*/
inline bool writeAuthRequired (Writer& w, Command code, AuthState state,
                               const std::uint8_t nonce[kCryptoNonceBytes],
                               const char* name, std::size_t nameLen,
                               const char* productId, std::size_t productLen) noexcept
{
    if (code != Command::invitationReplyAuthReq
        && code != Command::invitationReplyUserAuth)
        return false;

    const std::uint8_t nonceWords   = std::uint8_t (kCryptoNonceBytes / 4);   // 4
    const std::uint8_t nameWords    = std::uint8_t ((nameLen    + 3) / 4);
    const std::uint8_t productWords = std::uint8_t ((productLen + 3) / 4);
    const std::size_t  payloadWords = std::size_t (nonceWords) + nameWords + productWords;
    if (payloadWords > 255 || nameWords == 0)
        return false;

    const std::uint16_t csd = std::uint16_t ((std::uint16_t (nameWords) << 8)
                                             | std::uint8_t (state));

    return w.writeHeader (code, std::uint8_t (payloadWords), csd)
        && w.raw (nonce, kCryptoNonceBytes)
        && w.bytesPadded (name, nameLen)
        && w.bytesPadded (productId, productLen);
}

/*  Retransmit Request (§7.2.3, Table 30). pl = 1.

        csd            = Sequence Number of the first UMP Data command wanted
        payload word 0 = Number of UMP Commands (16) | Reserved (16)

    `umpCommandCount` 0 means "send all previously sent UMP Data Commands starting
    from Sequence Number", and is the only value the table defines. A responder "may
    ignore the Number of UMP Commands field and send all packets since the specified
    Sequence Number" regardless, so do not rely on it being honoured.
*/
inline bool writeRetransmitRequest (Writer& w, std::uint16_t firstSeq,
                                    std::uint16_t umpCommandCount = 0) noexcept
{
    return w.writeHeader (Command::retransmitRequest, 1, firstSeq)
        && w.u16 (umpCommandCount)
        && w.u16 (0);
}

/*  Retransmit Error (§7.2.4, Table 31). pl = 1.

        csd1           = Error Reason (Table 32), csd2 = 0 (Reserved)
        payload word 0 = Sequence Number (16) | Reserved (16)

    Table 31 defines that Sequence Number as "the first UMP Data Command that COULD be
    retransmitted" -- i.e. where the requester should re-ask from. §7.2.3's prose
    instead says the Error specifies "the first missing Sequence Number", which is a
    different value. The table wins here, as it has every other time the two have
    disagreed in this spec (see NAK csd2 and the Bye payload), and it is also the more
    actionable of the two: a requester can do something with "here is what I still
    have" and nothing with "here is what you already knew you lost". PROTOCOL.md §5.4
    records the conflict.
*/
inline bool writeRetransmitError (Writer& w, RetransmitError reason,
                                  std::uint16_t firstAvailableSeq) noexcept
{
    const std::uint16_t csd = std::uint16_t (std::uint16_t (std::uint8_t (reason)) << 8);
    return w.writeHeader (Command::retransmitError, 1, csd)
        && w.u16 (firstAvailableSeq)
        && w.u16 (0);
}

// Session Reset (§6.11, Table 20) and its reply (§6.12, Table 21). Both are header
// only: pl = 0, Command Specific Data Reserved and zero.
inline bool writeSessionReset (Writer& w) noexcept
{
    return w.writeHeader (Command::sessionReset, 0, 0);
}

inline bool writeSessionResetReply (Writer& w) noexcept
{
    return w.writeHeader (Command::sessionResetReply, 0, 0);
}

// NAK (§6.15, Table 24). csd1 = NAK Reason, csd2 = 0 (Reserved — NOT the offending
// command code). The payload's first word is the header word of the command being
// NAK'ed, copied verbatim; an optional UTF-8 Text Message may follow, which we do
// not send. pl is therefore 1.
inline bool writeNak (Writer& w, NakReason reason, std::uint32_t nakedHeaderWord) noexcept
{
    const std::uint16_t csd = std::uint16_t (std::uint16_t (std::uint8_t (reason)) << 8);
    return w.writeHeader (Command::nak, 1, csd) && w.u32 (nakedHeaderWord);
}

//==============================================================================
// Parsing. A received datagram is walked command-by-command; the caller handles
// each via the callbacks it cares about.

struct ParsedCommand
{
    Command        code;
    std::uint8_t   payloadWords;
    std::uint16_t  cmdSpecific;
    const std::uint8_t* payload;   // payloadWords*4 bytes, or nullptr if none
    std::uint8_t   data1() const noexcept { return std::uint8_t (cmdSpecific >> 8); }
    std::uint8_t   data2() const noexcept { return std::uint8_t (cmdSpecific); }

    // The command's own 32-bit header, rebuilt from the parsed fields. NAK echoes
    // this back verbatim to say which command it is complaining about (§6.15).
    std::uint32_t  headerWord() const noexcept
    {
        return (std::uint32_t (std::uint8_t (code)) << 24)
             | (std::uint32_t (payloadWords)        << 16)
             |  std::uint32_t (cmdSpecific);
    }
};

// Verifies the signature, then invokes `fn(const ParsedCommand&)` for each command
// in the datagram. Returns false on a bad signature or a truncated/overrun command.
template <typename Fn>
inline bool parseDatagram (const std::uint8_t* datagram, std::size_t length, Fn&& fn)
{
    if (length < kHeaderBytes || get32 (datagram) != kSignature)
        return false;

    std::size_t pos = kHeaderBytes;
    while (pos + kHeaderBytes <= length)
    {
        ParsedCommand c;
        c.code         = Command (datagram[pos]);
        c.payloadWords = datagram[pos + 1];
        c.cmdSpecific  = get16 (datagram + pos + 2);

        const std::size_t payloadBytes = std::size_t (c.payloadWords) * 4;
        if (pos + kHeaderBytes + payloadBytes > length)
            return false; // truncated

        c.payload = payloadBytes ? datagram + pos + kHeaderBytes : nullptr;
        fn (c);
        pos += kHeaderBytes + payloadBytes;
    }
    return true;
}

} // namespace netmidi2
