/*
    libnetmidi2 — Network MIDI 2.0 (UDP) session.

    The state machine on top of Protocol.h + Platform.h. Symmetric: a Session can
    be the UDP Host (accepts Invitations) or the UDP Client (sends an Invitation).
    Once Established, either side sends UMP Data. Handles ping keepalive, timeout,
    graceful Bye, and sequence-number dedup.

    Freestanding-friendly: no exceptions/RTTI/heap/STL containers. Callbacks are
    delivered through ISessionListener (no std::function). Drive it by calling
    tick() frequently from your run loop.

    See PROTOCOL.md §4 (states/roles) and M2-124-UM §6.
*/

#pragma once

#include "Platform.h"
#include "Protocol.h"

namespace netmidi2
{

enum class State { idle, inviting, established, closed };
enum class Role  { host, client };

class ISessionListener
{
public:
    virtual ~ISessionListener() = default;
    virtual void onUmpReceived (const std::uint32_t* words, std::uint8_t count) = 0;
    virtual void onStateChanged (State) {}
};

class Session
{
public:
    // Timing (ms). Conservative defaults; tune per transport.
    struct Timing
    {
        std::uint32_t inviteRetryMs = 500;
        std::uint32_t pingIntervalMs = 2000;
        std::uint32_t timeoutMs = 10000;
    };

    Session (const Platform& platform, Role role, ISessionListener* listener,
             const char* endpointName, const char* productInstanceId) noexcept
        : plat (platform), role (role), listener (listener),
          name (endpointName), productId (productInstanceId) {}

    void setTiming (Timing t) noexcept { timing = t; }

    State state() const noexcept { return st; }
    const Endpoint& remote() const noexcept { return peer; }

    // Client: start a session by inviting `remotePeer`.
    void connect (const Endpoint& remotePeer) noexcept
    {
        peer = remotePeer;
        touch();
        if (role == Role::client)
        {
            sendInvitation();
            setState (State::inviting);
        }
    }

    // Host: wait for an inbound Invitation (peer learned on receipt).
    void listen() noexcept { setState (State::idle); }

    // Graceful close.
    void close (ByeReason reason = ByeReason::undefined) noexcept
    {
        if (st == State::established || st == State::inviting)
            sendBye (reason);
        setState (State::closed);
    }

    // Send UMP words (host order) — only valid when Established.
    bool sendUmp (const std::uint32_t* words, std::uint8_t count) noexcept
    {
        if (st != State::established)
            return false;
        std::uint8_t buf[kMaxDatagram];
        Writer w (buf, sizeof buf);
        if (! (w.writeSignature() && writeUmpData (w, txSeq, words, count)))
            return false;
        ++txSeq;
        return plat.socket->send (peer, buf, w.size()) >= 0;
    }

    // Pump the session: drain the socket, run keepalive/retry/timeout timers.
    void tick() noexcept
    {
        std::uint8_t buf[kMaxDatagram];
        Endpoint from;
        for (int guard = 0; guard < 64; ++guard)   // bound work per tick
        {
            const int n = plat.socket->receive (buf, sizeof buf, from);
            if (n <= 0)
                break;
            handleDatagram (buf, std::size_t (n), from);
        }

        const std::uint32_t now = plat.clock->nowMs();

        if (st == State::inviting && now - lastInviteMs >= timing.inviteRetryMs)
            sendInvitation();

        if (st == State::established)
        {
            if (now - lastPingMs >= timing.pingIntervalMs)
                sendPing();
            if (now - lastRxMs >= timing.timeoutMs)
                close (ByeReason::timeout);
        }
    }

private:
    void setState (State s) noexcept
    {
        if (s != st) { st = s; if (listener) listener->onStateChanged (s); }
    }

    void touch() noexcept { lastRxMs = plat.clock->nowMs(); }

    void handleDatagram (const std::uint8_t* data, std::size_t len, const Endpoint& from) noexcept
    {
        parseDatagram (data, len, [&] (const ParsedCommand& c) { handleCommand (c, from); });
    }

    //== admission ============================================================
    // Is this datagram from the endpoint we are actually in a session with?
    bool isFromPeer (const Endpoint& from) const noexcept
    {
        return (st == State::inviting || st == State::established) && from == peer;
    }

    // No session is at stake in these states, so there is nothing to protect.
    bool isSessionless() const noexcept
    {
        return st == State::idle || st == State::closed;
    }

    /* Everything that belongs to a session must come FROM that session's peer.
     * Without this check any box on the LAN can operate on a session it is not part
     * of: one spoofed 8-byte Bye closes somebody else's session, stray UMP_DATA is
     * delivered as if the peer had sent it, and a stranger's traffic refreshes the
     * liveness timer so a peer that is really gone goes on looking alive.
     *
     * Measured on the bench, and the reason this was found: a client stuck at
     * `inviting` against a host that was busy with a third box would connect the
     * moment its own router was RESTARTED. The restart was not fixing the handshake
     * -- the parting Bye of the closing session was knocking the innocent third
     * party off that host, freeing the one slot. "Restart it and it connects"
     * looked like flakiness; it was this.
     *
     * Two things may still arrive from anywhere. An Invitation, because that is how
     * a peer is learned in the first place (one aimed at a session that is already
     * established is still ignored, in onInvitation). And a Ping while no session is
     * at stake, which peers use to probe liveness before inviting -- answering that
     * costs nothing and refusing it would make an idle host look dead. */
    static bool admits (Command code, bool fromPeer, bool sessionless) noexcept
    {
        if (fromPeer)
            return true;
        if (code == Command::invitation)
            return true;
        return sessionless && code == Command::ping;
    }

    //== dispatch =============================================================
    void handleCommand (const ParsedCommand& c, const Endpoint& from) noexcept
    {
        const bool fromPeer    = isFromPeer (from);
        const bool sessionless = isSessionless();

        if (! admits (c.code, fromPeer, sessionless))
            return;

        /* Only our actual peer keeps the session alive. Notably this excludes an
         * Invitation from a stranger to an established session: it is admitted
         * above (to be ignored), but it must not renew the timer that is the only
         * thing telling us the real peer went away. */
        if (fromPeer || sessionless)
            touch();

        switch (c.code)
        {
            case Command::invitation:              onInvitation (from);    break;
            case Command::invitationReplyAccepted: onInvitationAccepted(); break;
            case Command::ping:                    onPing (c);             break;
            case Command::pingReply:               break; // liveness already refreshed
            case Command::umpData:                 onUmpData (c);          break;
            case Command::bye:                     onBye();                break;
            case Command::byeReply:                setState (State::closed); break;
            case Command::nak:                     onNak();                break;
            default:                               break; // ignore for Phase 1
        }
    }

    //== per-command handlers =================================================
    void onInvitation (const Endpoint& from) noexcept
    {
        if (role != Role::host)
            return;

        if (st != State::established)
        {
            peer = from;
            sendInvitationAccepted();
            setState (State::established);
            return;
        }

        /* Already established. If it is the SAME peer, it is still inviting -- so
         * our InvitationAccepted never arrived. Send it again.
         *
         * Without this the handshake is unrecoverable in one specific way: a host
         * that ignores repeat Invitations leaves the client retrying forever against
         * a peer that considers the session open. The two ends disagree
         * permanently, the host shows `established` and the client shows
         * `inviting`, and nothing times out because the host keeps hearing the
         * invitations and treats them as liveness. Observed between two Raspberry
         * Pis: host established with 192.168.2.134:47605 while the client owning
         * that very port still reported `inviting`, minutes later.
         *
         * Accepting is idempotent, so re-answering costs one datagram per client
         * retry and converges as soon as one gets through. An Invitation from a
         * DIFFERENT endpoint is ignored: one session carries one peer, and
         * answering a second would silently steal the session from the box already
         * using it. */
        if (from == peer)
            sendInvitationAccepted();
    }

    void onInvitationAccepted() noexcept
    {
        if (role == Role::client && st == State::inviting)
            setState (State::established);
    }

    void onPing (const ParsedCommand& c) noexcept
    {
        if (c.payload)
            sendPingReply (get32 (c.payload));
    }

    void onUmpData (const ParsedCommand& c) noexcept
    {
        /* Payload Length is one byte off the wire, so it can claim up to 255 words
         * -- but §7.1 Table 29 bounds a UMP Data command at 64. A command over that
         * is malformed by definition; drop it rather than hand a 255-word count to a
         * 64-word buffer. (Found by asking why this length was never range-checked
         * on the way in when writeUmpData has always checked it on the way out.) */
        if (st != State::established
            || c.payloadWords == 0
            || c.payloadWords > kMaxUmpWordsPerCommand
            || ! c.payload)
            return;

        const std::uint16_t seq = c.cmdSpecific;
        if (haveRx && seq == lastRx)        // ignore exact duplicate
            return;

        haveRx = true;
        lastRx = seq;
        deliverUmp (c.payload, c.payloadWords);
    }

    void onBye() noexcept
    {
        sendByeReply();
        setState (State::closed);
    }

    void onNak() noexcept
    {
        // The peer rejected something we sent under the assumption that we were
        // Established -- almost always UMP_DATA after the peer restarted and has no
        // record of us. This is the client's ONLY reliable signal that has happened:
        // our own idle-timeout cannot catch it, because a peer that answers Ping
        // unconditionally (session-independent, e.g. Zephyr's netmidi2.c) keeps
        // refreshing touch() forever even with no session at all. Measured against
        // the Teensy: a NAK arrived for every rejected UMP_DATA, but sat in the
        // "Phase 1: ignore" default case, so the client stayed `established` and
        // kept sending into the void indefinitely.
        //
        // Re-inviting is safe even if the NAK was actually about something else
        // (host role sends none today, so in practice this only fires for a
        // client): worst case is one extra, harmless handshake round-trip.
        if (role == Role::client && st == State::established)
        {
            setState (State::idle);
            connect (peer);
        }
    }

    void deliverUmp (const std::uint8_t* payload, std::uint8_t words) noexcept
    {
        if (! listener) return;

        /* Belt and braces: the caller already rejects an over-long command, but this
         * buffer is sized by the same spec constant that bounds it, and Payload
         * Length arrives as a raw byte from the network. Never let the two drift. */
        if (words > kMaxUmpWordsPerCommand)
            return;

        std::uint32_t out[kMaxUmpWordsPerCommand];
        for (std::uint8_t i = 0; i < words; ++i)
            out[i] = get32 (payload + std::size_t (i) * 4);
        listener->onUmpReceived (out, words);
    }

    //== outbound single-command datagrams =====================================
    template <typename Build>
    void sendOne (Build&& build) noexcept
    {
        std::uint8_t buf[kMaxDatagram];
        Writer w (buf, sizeof buf);
        if (w.writeSignature() && build (w))
            plat.socket->send (peer, buf, w.size());
    }

    void sendInvitation() noexcept
    {
        lastInviteMs = plat.clock->nowMs();
        sendOne ([&] (Writer& w) { return writeInvitation (w, 0x00, name, cstrlen (name), productId, cstrlen (productId)); });
    }
    void sendInvitationAccepted() noexcept
    {
        sendOne ([&] (Writer& w) { return writeInvitationAccepted (w, name, cstrlen (name), productId, cstrlen (productId)); });
    }
    void sendPing() noexcept
    {
        lastPingMs = plat.clock->nowMs();
        const std::uint32_t id = ++pingId;
        sendOne ([&] (Writer& w) { return writePing (w, id); });
    }
    void sendPingReply (std::uint32_t id) noexcept { sendOne ([&] (Writer& w) { return writePingReply (w, id); }); }
    void sendBye (ByeReason r) noexcept            { sendOne ([&] (Writer& w) { return writeBye (w, r); }); }
    void sendByeReply() noexcept                   { sendOne ([&] (Writer& w) { return writeByeReply (w); }); }

    static std::size_t cstrlen (const char* s) noexcept
    {
        std::size_t n = 0; while (s && s[n]) ++n; return n;
    }

    //==========================================================================
    Platform          plat;
    Role              role;
    ISessionListener* listener;
    const char*       name;
    const char*       productId;
    Timing            timing;

    Endpoint          peer;
    State             st = State::idle;

    std::uint16_t     txSeq = 0;
    bool              haveRx = false;
    std::uint16_t     lastRx = 0;

    std::uint32_t     lastRxMs = 0, lastPingMs = 0, lastInviteMs = 0, pingId = 0;
};

} // namespace netmidi2
