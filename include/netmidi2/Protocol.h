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

namespace netmidi2
{

//==============================================================================
// Constants (PROTOCOL.md §1–2)

constexpr std::uint32_t kSignature   = 0x4D494449u; // "MIDI"
constexpr std::size_t   kMaxDatagram = 1400;        // §5.1.1 — never fragment
constexpr std::size_t   kHeaderBytes = 4;           // command packet header

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

// A few well-known reason codes (PROTOCOL.md §3.5–3.6). Not exhaustive.
enum class ByeReason : std::uint8_t
{
    undefined            = 0x00,
    timeout              = 0x04,
    sessionNotEstablished = 0x05,
    noPendingInvitation  = 0x06,
    userRejected         = 0x40,
    rejectedNoPrior      = 0x41,
    authFailed           = 0x43,
    noMatchingAuth       = 0x45,
    invitationCanceled   = 0x80,
};

enum class NakReason : std::uint8_t
{
    commandNotExpected   = 0x02,
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
    if (wordCount > 64)
        return false;
    if (! w.writeHeader (Command::umpData, wordCount, sequenceNumber))
        return false;
    for (std::uint8_t i = 0; i < wordCount; ++i)
        if (! w.u32 (words[i]))
            return false;
    return true;
}

// Invitation (§6.4). name/productId are UTF-8/ASCII, not necessarily word-aligned;
// they are null-padded to word boundaries here.
inline bool writeInvitation (Writer& w, std::uint8_t capabilities,
                             const char* name, std::size_t nameLen,
                             const char* productId, std::size_t productLen) noexcept
{
    const std::uint8_t nameWords    = std::uint8_t ((nameLen    + 3) / 4);
    const std::uint8_t productWords = std::uint8_t ((productLen + 3) / 4);
    const std::uint8_t payloadWords = std::uint8_t (nameWords + productWords);
    const std::uint16_t csd = std::uint16_t ((std::uint16_t (nameWords) << 8) | capabilities);

    return w.writeHeader (Command::invitation, payloadWords, csd)
        && w.bytesPadded (name, nameLen)
        && w.bytesPadded (productId, productLen);
}

// Invitation Reply: Accepted (§6.5).
inline bool writeInvitationAccepted (Writer& w,
                                     const char* name, std::size_t nameLen,
                                     const char* productId, std::size_t productLen) noexcept
{
    const std::uint8_t nameWords    = std::uint8_t ((nameLen    + 3) / 4);
    const std::uint8_t productWords = std::uint8_t ((productLen + 3) / 4);
    const std::uint8_t payloadWords = std::uint8_t (nameWords + productWords);
    const std::uint16_t csd = std::uint16_t (std::uint16_t (nameWords) << 8);

    return w.writeHeader (Command::invitationReplyAccepted, payloadWords, csd)
        && w.bytesPadded (name, nameLen)
        && w.bytesPadded (productId, productLen);
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
