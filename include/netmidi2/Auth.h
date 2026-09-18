/*
    libnetmidi2 — authentication contract (§6.7–6.10).

    A Host that wants a password answers an Invitation with a challenge carrying a
    16-byte CryptoNonce; the Client hashes that nonce together with the shared secret
    and sends back a 32-byte SHA-256 digest; the Host recomputes it and compares.

    NO CRYPTO IS IMPLEMENTED HERE.

    SHA-256 and randomness both arrive through ICrypto, for the same reason sockets
    do (prime directive 3). Hand-rolling a hash into a freestanding core would ship
    one implementation to every target and make the hardware SHA engine on an MCU
    unreachable; injecting it lets macOS use CommonCrypto, Zephyr use mbedTLS or its
    accelerator, and a test use a known-good reference.

    Randomness is the part that actually carries the security here, and is the reason
    it cannot be defaulted. The spec's own example secret is "5483" -- it says the
    shared secret is "typically a 4- or 6-digit number" -- so the digest is only ever
    as strong as a PIN, which is why §6.7 asks a Host to slow down repeated failures
    rather than rely on the hash. What must not be weak is the nonce: a predictable
    one lets an attacker precompute, and then the PIN stops mattering at all. An
    implementation that cannot supply real entropy should refuse to offer auth rather
    than invent some.
*/

#pragma once

#include <cstddef>
#include <cstdint>

namespace netmidi2
{

//==============================================================================
constexpr std::size_t kCryptoNonceBytes = 16;   // §6.7 Table 14: 16 bytes, ASCII
constexpr std::size_t kAuthDigestBytes  = 32;   // §6.9 Table 18: SHA-256

// Bounds for the strings we will hash. The spec does not cap these, but a
// freestanding core has to build the input somewhere; oversized values are refused
// rather than truncated, because a silently shortened password would authenticate
// against the wrong digest.
constexpr std::size_t kMaxSharedSecretBytes = 128;
constexpr std::size_t kMaxUsernameBytes     = 128;

// §6.4 Table 11: the Capabilities bitmap a Client sends in its Invitation.
constexpr std::uint8_t kCapInvitationWithAuth     = 0x01;   // D0
constexpr std::uint8_t kCapInvitationWithUserAuth = 0x02;   // D1

// §6.7 Table 15 / §6.8 Table 17.
enum class AuthState : std::uint8_t
{
    firstRequest    = 0x00,
    digestIncorrect = 0x01,
};

//==============================================================================
/*  SHA-256 and entropy, injected. Both must be non-blocking enough to call from the
    run loop; on a platform where the entropy source can block or fail, randomBytes()
    should return false rather than hand back something predictable.
*/
class ICrypto
{
public:
    virtual ~ICrypto() = default;

    // Hash `len` bytes into a 32-byte digest.
    virtual void sha256 (const std::uint8_t* data, std::size_t len,
                         std::uint8_t out[kAuthDigestBytes]) = 0;

    // Fill `out` with cryptographically secure random bytes. False if unavailable --
    // see the header comment on why guessing is not an option here.
    virtual bool randomBytes (std::uint8_t* out, std::size_t len) = 0;
};

//==============================================================================
inline std::size_t authCstrBytes (const char* s) noexcept
{
    std::size_t n = 0;
    while (s && s[n]) ++n;
    return n;
}

/*  §6.9: "generate the SHA-256 Authentication Digest by concatenating the
    CryptoNonce and the shared secret into one string which is hashed using SHA-256",
    i.e. sha256(nonce || secret). The spec publishes a worked example, which
    tests/auth.cpp checks byte for byte.
*/
inline bool makeAuthDigest (ICrypto& crypto,
                            const std::uint8_t nonce[kCryptoNonceBytes],
                            const char* sharedSecret,
                            std::uint8_t out[kAuthDigestBytes]) noexcept
{
    const std::size_t secretLen = authCstrBytes (sharedSecret);
    if (secretLen == 0 || secretLen > kMaxSharedSecretBytes)
        return false;

    std::uint8_t buf[kCryptoNonceBytes + kMaxSharedSecretBytes];
    for (std::size_t i = 0; i < kCryptoNonceBytes; ++i)
        buf[i] = nonce[i];
    for (std::size_t i = 0; i < secretLen; ++i)
        buf[kCryptoNonceBytes + i] = std::uint8_t (sharedSecret[i]);

    crypto.sha256 (buf, kCryptoNonceBytes + secretLen, out);
    return true;
}

/*  §6.10: sha256(nonce || username || password). Also has a published example. */
inline bool makeUserAuthDigest (ICrypto& crypto,
                                const std::uint8_t nonce[kCryptoNonceBytes],
                                const char* username,
                                const char* password,
                                std::uint8_t out[kAuthDigestBytes]) noexcept
{
    const std::size_t userLen = authCstrBytes (username);
    const std::size_t passLen = authCstrBytes (password);
    if (userLen == 0 || userLen > kMaxUsernameBytes)
        return false;
    if (passLen == 0 || passLen > kMaxSharedSecretBytes)
        return false;

    std::uint8_t buf[kCryptoNonceBytes + kMaxUsernameBytes + kMaxSharedSecretBytes];
    std::size_t n = 0;
    for (std::size_t i = 0; i < kCryptoNonceBytes; ++i) buf[n++] = nonce[i];
    for (std::size_t i = 0; i < userLen; ++i)           buf[n++] = std::uint8_t (username[i]);
    for (std::size_t i = 0; i < passLen; ++i)           buf[n++] = std::uint8_t (password[i]);

    crypto.sha256 (buf, n, out);
    return true;
}

/*  Compare two digests without leaking where they first differ.

    memcmp returns as soon as it finds a difference, so how long a rejection takes
    reveals how much of the digest was right -- enough, given retries, to recover it
    a byte at a time without ever knowing the secret. Always compare all 32 bytes.
*/
inline bool digestsEqual (const std::uint8_t a[kAuthDigestBytes],
                          const std::uint8_t b[kAuthDigestBytes]) noexcept
{
    std::uint8_t diff = 0;
    for (std::size_t i = 0; i < kAuthDigestBytes; ++i)
        diff = std::uint8_t (diff | (a[i] ^ b[i]));
    return diff == 0;
}

/*  A fresh 16-byte CryptoNonce.

    §6.7 Table 14 types the field as ASCII, and both of the spec's worked examples
    are printable ("nUWrn*@#$hjfwnkL", "XI|~=NNRVaD;XCPL"), so random bytes are
    mapped into printable ASCII 33..126 by rejection rather than by modulo -- the
    modulo version biases the low end of the range, which costs entropy for nothing.

    §6.7: "Every new Session shall use a new CryptoNonce, even for the same Client",
    while duplicate Invitations from a Client mid-handshake get the SAME nonce back.
*/
inline bool makeCryptoNonce (ICrypto& crypto, std::uint8_t out[kCryptoNonceBytes]) noexcept
{
    constexpr std::uint8_t kLow = 33, kHigh = 126;      // printable, no space
    constexpr std::uint8_t kSpan = kHigh - kLow + 1;    // 94
    constexpr std::uint8_t kLimit = std::uint8_t (256 / kSpan * kSpan);  // 188

    std::size_t filled = 0;
    for (int attempt = 0; attempt < 32 && filled < kCryptoNonceBytes; ++attempt)
    {
        std::uint8_t raw[kCryptoNonceBytes * 2];
        if (! crypto.randomBytes (raw, sizeof raw))
            return false;

        for (std::size_t i = 0; i < sizeof raw && filled < kCryptoNonceBytes; ++i)
            if (raw[i] < kLimit)                        // reject the biased tail
                out[filled++] = std::uint8_t (kLow + (raw[i] % kSpan));
    }
    return filled == kCryptoNonceBytes;
}

} // namespace netmidi2
