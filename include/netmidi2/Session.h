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

/*  Session states, in lifecycle order.

    `inviting` is the spec's "Pending Invitation" and `closing` its "Pending Bye"
    (§6.1): we have sent a Bye and are repeating it until the peer answers with a
    Bye Reply or we give up. `closed` is ours rather than the spec's -- §6.3.1 has
    both ends return to Idle after a teardown, but a library is more useful if a
    finished Session stays finished and says so, instead of silently looking ready
    to accept the next stranger that comes along.

    The auth and session-reset states (§6.1) are Phase 2/3 and absent.
*/
enum class State { idle, inviting, established, closing, closed };
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
    /*  §6.2 recommends 300ms-2s between repeats of a repeated Command, and requires
        that a Device which reaches its timeout without a suitable reply stops
        repeating and sends a Bye.

        New fields go on the END of this struct: consumers may be initialising the
        original three positionally, and inserting a field in the middle would
        silently change what their numbers mean.
    */
    struct Timing
    {
        std::uint32_t inviteRetryMs = 500;
        std::uint32_t pingIntervalMs = 2000;
        std::uint32_t timeoutMs = 10000;

        std::uint32_t inviteTimeoutMs = 10000; // stop inviting, send Bye (§6.2)
        std::uint32_t byeRetryMs = 500;        // repeat an unanswered Bye (§6.16)
        std::uint32_t byeTimeoutMs = 3000;     // give up waiting for the Bye Reply
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
            inviteStartMs = plat.clock->nowMs();   // when to stop trying (§6.2)
            sendInvitation();
            setState (State::inviting);
        }
    }

    // Host: wait for an inbound Invitation (peer learned on receipt).
    void listen() noexcept { setState (State::idle); }

    /*  Graceful close. Enters `closing` (the spec's Pending Bye) and repeats the Bye
        until the peer answers or byeTimeoutMs elapses -- §6.16: "The Bye Command
        should be sent repeatedly until a Bye Reply Command is received, or until a
        timeout occurs."

        The session is NOT closed the moment this returns. It used to be: one Bye
        went out and the state went straight to `closed`, so a single lost datagram
        left the peer holding a session we had already forgotten, waiting out its own
        idle timeout with no idea we had gone. That is the teardown-path twin of the
        lost-Accepted bug on the setup path.
    */
    void close (ByeReason reason = ByeReason::undefined) noexcept
    {
        if (st == State::closing || st == State::closed)
            return;                                  // already tearing down

        if (st == State::established || st == State::inviting)
        {
            byeReason  = reason;
            byeStartMs = plat.clock->nowMs();
            sendByeNow();
            setState (State::closing);
            return;
        }

        setState (State::closed);                    // idle: nothing to tear down
    }

    // Send UMP words (host order) — only valid when Established.
    bool sendUmp (const std::uint32_t* words, std::uint8_t count) noexcept
    {
        if (st != State::established)
            return false;
        std::uint8_t buf[kMaxDatagram];
        Writer w (buf, sizeof buf);
        if (! (w.writeSignature() && writeUmpData (w, txSeq, words, count) && w.ok()))
            return false;
        ++txSeq;
        return plat.socket->send (peer, buf, w.size()) >= 0;
    }

    /*  Pump the session: drain the socket, then run the clocks.

        This is the standalone form, for a Session that has a socket to itself. When
        several Sessions share one UDP port -- which is how a Host serves more than
        one Client (§3.2) -- exactly one thing may drain that socket, or they steal
        each other's datagrams. In that arrangement HostPort does the draining and
        calls deliver() and tickTimers() instead; do not also call tick().
    */
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

        tickTimers();
    }

    // Hand this Session one datagram that was received for it. Pairs with
    // tickTimers(); see tick() for why they are separable.
    void deliver (const std::uint8_t* data, std::size_t len, const Endpoint& from) noexcept
    {
        handleDatagram (data, len, from);
    }

    // Run the retry / keepalive / timeout clocks, without touching the socket.
    void tickTimers() noexcept
    {
        const std::uint32_t now = plat.clock->nowMs();

        if (st == State::inviting)
        {
            /* §6.2: a repeated Command is not repeated forever. "If a Device reaches
             * its preferred timeout without receiving a suitable reply, then the
             * Device shall cease repeating the Command and send a Bye Command."
             *
             * Checked before the retry, so the last act of an expiring invitation is
             * the Bye, not another Invitation. Without this a client that invited a
             * host which never answered sat in `inviting` forever -- no timeout, no
             * error, no Bye. That is the "client retries forever, nothing times out"
             * half of the handshake deadlock: the host's side was fixed in 141a075,
             * but the client's inability to ever give up was left in place. */
            if (now - inviteStartMs >= timing.inviteTimeoutMs)
                close (ByeReason::timeout);
            else if (now - lastInviteMs >= timing.inviteRetryMs)
                sendInvitation();
        }

        if (st == State::established)
        {
            if (now - lastPingMs >= timing.pingIntervalMs)
                sendPing();
            if (now - lastRxMs >= timing.timeoutMs)
                close (ByeReason::timeout);
        }

        if (st == State::closing)
        {
            /* Timeout first, so giving up never emits one last Bye on its way out --
             * §6.2 excepts a repeated Bye from the "send a Bye on timeout" rule, for
             * the obvious reason. */
            if (now - byeStartMs >= timing.byeTimeoutMs)
                setState (State::closed);
            else if (now - lastByeMs >= timing.byeRetryMs)
                sendByeNow();
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
    // `closing` counts: we are waiting on that peer's Bye Reply, and if this did not
    // include it the very reply the teardown is blocking on would be refused at the
    // door and every close would have to wait out its full timeout.
    bool isFromPeer (const Endpoint& from) const noexcept
    {
        return (st == State::inviting || st == State::established || st == State::closing)
               && from == peer;
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
     * Four things may still arrive from anywhere, and each is answered without
     * being accepted -- the handler decides what, if anything, it does to our
     * session:
     *
     *   Invitation, because that is how a peer is learned in the first place (one
     *   aimed at a session that is already established is still ignored, in
     *   onInvitation).
     *
     *   Bye, because §6.16 requires a Bye Reply "even if there is no Pending or
     *   Established Session" -- a Bye is repeated until acknowledged, so a sender we
     *   know nothing about is exactly the one stuck retransmitting. onBye
     *   acknowledges every Bye but closes only for our own peer.
     *
     *   Invitation Reply: Accepted, because §6.5 requires Bye 0x06 when there is no
     *   pending invitation -- which by definition means it did not come from a peer
     *   we are mid-handshake with. onInvitationAccepted establishes ONLY for the
     *   endpoint we actually invited; for anyone else it just answers.
     *
     *   Ping while no session is at stake, which peers use to probe liveness before
     *   inviting -- answering costs nothing and refusing would make an idle host
     *   look dead. */
    static bool admits (Command code, bool fromPeer, bool sessionless) noexcept
    {
        if (fromPeer)
            return true;
        if (code == Command::invitation
            || code == Command::bye
            || code == Command::invitationReplyAccepted)
            return true;
        return sessionless && code == Command::ping;
    }

    // The commands this implementation actually acts on. Anything else -- including
    // codes the spec defines but we have not implemented (auth, retransmit, session
    // reset) -- is "not supported" as far as a peer is concerned, and §5.5 says to
    // say so rather than stay silent.
    static bool isSupported (Command code) noexcept
    {
        switch (code)
        {
            case Command::umpData:
            case Command::invitation:
            case Command::invitationReplyAccepted:
            case Command::ping:
            case Command::pingReply:
            case Command::bye:
            case Command::byeReply:
            case Command::nak:
                return true;
            default:
                return false;
        }
    }

    /* Two replies are owed to the SENDER regardless of whether we have a session
     * with them, so they are answered ahead of the peer-admission gate. Returns
     * true if the command was dealt with here and must not be dispatched.
     *
     * Neither reply may call touch() or change state: a stranger must not be able
     * to hold our liveness timer open (that was a real bug -- see the "stranger:"
     * checks), and answering is not the same as accepting.
     *
     * Why answer a stranger at all? Because silence is what made the mirror-image
     * bug so hard to find. A peer that still believes in a session we have
     * forgotten -- because we restarted -- will otherwise send UMP Data into the
     * void forever, with nothing to tell it to re-invite. That is exactly the
     * failure the NAK handler was added to fix, seen from the other side. */
    bool answerProtocolError (const ParsedCommand& c, const Endpoint& from,
                              bool fromPeer) noexcept
    {
        // §5.5: unknown/unsupported Command Code -> NAK "Command not supported".
        if (! isSupported (c.code))
        {
            sendNakTo (from, NakReason::commandNotSupported, c);
            return true;
        }

        // §7.1: UMP Data outside an Established Session -> Bye reason 0x05.
        if (c.code == Command::umpData && ! (fromPeer && st == State::established))
        {
            sendByeTo (from, ByeReason::sessionNotEstablished);
            return true;
        }

        return false;
    }

    //== dispatch =============================================================
    void handleCommand (const ParsedCommand& c, const Endpoint& from) noexcept
    {
        const bool fromPeer    = isFromPeer (from);
        const bool sessionless = isSessionless();

        if (answerProtocolError (c, from, fromPeer))
            return;

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
            case Command::invitationReplyAccepted: onInvitationAccepted (from, fromPeer); break;
            case Command::ping:                    onPing (c, from);       break;
            case Command::pingReply:               break; // liveness already refreshed
            case Command::umpData:                 onUmpData (c);          break;
            case Command::bye:                     onBye (from, fromPeer); break;
            case Command::byeReply:                onByeReply();           break;
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
         * Pis: host established with 203.0.113.34:47605 while the client owning
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

    void onInvitationAccepted (const Endpoint& from, bool fromPeer) noexcept
    {
        // The normal case: our own pending invitation was accepted.
        if (fromPeer && role == Role::client && st == State::inviting)
        {
            setState (State::established);
            return;
        }

        /* §6.5: "If a Client receives this Command when it is already in an
         * Established Session with the Host, then it shall ignore it."
         *
         * This branch must come before the Bye below, and is the reason the order
         * matters: a host repeats its Accepted until it sees traffic (see
         * onInvitation), so a duplicate arriving just after we established is
         * routine. Answering that with a Bye would tear down the session we just
         * successfully opened, using the host's own retransmission to do it. */
        if (fromPeer && st == State::established)
            return;

        /* §6.5: "If a Client receives this Command when it is not in a Pending
         * Session with the Host, then the Client shall send a Bye Command to the
         * Host with reason 0x06 (No Pending Invitation)."
         *
         * We have no invitation outstanding with this sender -- we never sent one,
         * or we have since given up or closed. Either way the sender believes it is
         * opening a session that does not exist on our side, and will go on
         * retransmitting until told. Note this deliberately does NOT establish
         * anything: an Accepted from an endpoint we did not invite must never open a
         * session, or anyone on the LAN could hand us one unasked. */
        sendByeTo (from, ByeReason::noPendingSession);
    }

    void onPing (const ParsedCommand& c, const Endpoint& from) noexcept
    {
        /* Reply to whoever pinged, not to `peer`. An idle host is allowed to answer
         * a Ping from anyone (that is how a peer probes us before inviting), but it
         * has no `peer` yet -- so replying to `peer` sent the Ping Reply to an empty
         * endpoint and the prober heard nothing, which is exactly the "looks dead"
         * failure answering was meant to avoid. When a session does exist, `from`
         * has already been checked to be that peer, so this is the same address. */
        if (c.payload)
            sendOneTo (from, [id = get32 (c.payload)] (Writer& w) { return writePingReply (w, id); });
    }

    /*  Has this Sequence Number already been processed? Records it if not.

        §7.2 makes this a receiver's job, not an optional optimisation: "Receivers
        ignore UMP Data Commands with a Sequence Number which has already been
        received and processed", and §7.2.2 "Every Device receiving a UDP packet
        with UMP data shall be able to skip previously received UMP Data Commands."

        Remembering only the newest Sequence Number is not enough, which is the bug
        this replaced. A FEC sender prepends its previous commands oldest-first
        (§7.2.2 "FEC Packet Order"), so a datagram carries [N-2, N-1, N]. Compared
        against the newest alone, N-2 does not match, gets delivered a second time,
        AND drags the newest marker backwards so N-1 fails to match either. Measured
        against a standard two-repeat sender: every message delivered three times --
        every note-on fired three times.

        So: a 64-entry replay window, the same shape IPsec uses. `lastRx` is the
        highest Sequence Number processed and `rxWindow` bit i means (lastRx - i)
        has been processed, bit 0 being lastRx itself. All comparisons are in 16-bit
        wrapping arithmetic, so 0xFFFF -> 0x0000 needs no special case (§5.6), and a
        sender that restarts its numbering simply jumps the window forward.

        Note what this deliberately does NOT do: it does not hold packets back to
        put them in order. An unseen Sequence Number is delivered whatever its
        position, because that is exactly how FEC repairs a gap -- the missing
        command arrives inside a LATER datagram, and refusing it for being out of
        order would throw away the recovery FEC exists to provide. The cost is that
        a repaired message reaches the listener after ones that followed it.
        Reordering with a jitter buffer belongs above this layer.
    */
    static constexpr std::uint16_t kRxWindowBits = 64;

    bool acceptSequence (std::uint16_t seq) noexcept
    {
        if (! haveRx)
        {
            haveRx   = true;
            lastRx   = seq;
            rxWindow = 1;
            return true;
        }

        const std::uint16_t ahead = std::uint16_t (seq - lastRx);   // wraps at 0xFFFF

        if (ahead == 0)
            return false;                       // the newest one, again

        if (ahead < 0x8000u)                    // newer: slide the window forward
        {
            rxWindow = (ahead >= kRxWindowBits) ? std::uint64_t (0)
                                                : std::uint64_t (rxWindow << ahead);
            rxWindow |= std::uint64_t (1);
            lastRx = seq;
            return true;
        }

        const std::uint16_t behind = std::uint16_t (lastRx - seq);  // 1..0x7FFF
        if (behind >= kRxWindowBits)
            return false;                       // older than the window remembers

        const std::uint64_t bit = std::uint64_t (1) << behind;
        if ((rxWindow & bit) != 0)
            return false;                       // already processed

        rxWindow |= bit;
        return true;
    }

    void onUmpData (const ParsedCommand& c) noexcept
    {
        /* Payload Length is one byte off the wire, so it can claim up to 255 words
         * -- but §7.1 Table 29 bounds a UMP Data command at 64. A command over that
         * is malformed by definition; drop it rather than hand a 255-word count to a
         * 64-word buffer. (Found by asking why this length was never range-checked
         * on the way in when writeUmpData has always checked it on the way out.)
         * Rejected before the window sees it, so a malformed command cannot burn a
         * Sequence Number that the real one still needs. */
        if (st != State::established || c.payloadWords > kMaxUmpWordsPerCommand)
            return;

        /* Zero-length UMP Data carries a Sequence Number like any other (§7.2.1), so
         * the window must see it even though there is nothing to hand over -- it is
         * how a sender declares an idle period, and it is FEC-repeated too. */
        if (! acceptSequence (c.cmdSpecific))
            return;

        if (c.payloadWords > 0 && c.payload)
            deliverUmp (c.payload, c.payloadWords);
    }

    void onByeReply() noexcept
    {
        /* §6.17: "If a Device receives a Bye Reply Command, the Device shall stop
         * repeatedly sending the Bye Command. If there is no Established Session
         * with the sender of the Bye Reply Command, the receiver shall ignore the
         * Bye Reply Command."
         *
         * `closing` is the only state in which we are waiting for one, so it is the
         * only state in which one means anything. Previously any Bye Reply from our
         * peer closed us -- including one arriving while we were still `inviting`,
         * where there is no session to end and the spec says to ignore it. */
        if (st == State::closing)
            setState (State::closed);
    }

    void onBye (const Endpoint& from, bool fromPeer) noexcept
    {
        /* Acknowledge every Bye, to the sender. §6.16: "Because the Bye Command
         * might be repeated, the Bye Reply shall also be sent if there is no Pending
         * or Established Session." A sender we have no session with is precisely the
         * one that will otherwise keep retransmitting until it times out -- the
         * silence, not the Bye, is the problem. One reply per Bye received. */
        sendOneTo (from, [] (Writer& w) { return writeByeReply (w); });

        /* ...but only OUR peer's Bye ends OUR session. Acknowledging a stranger is
         * courtesy; letting one close a session it is not part of was the bug the
         * "stranger:" checks were written for. */
        if (fromPeer)
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
    void sendOneTo (const Endpoint& to, Build&& build) noexcept
    {
        std::uint8_t buf[kMaxDatagram];
        Writer w (buf, sizeof buf);
        // w.ok() last: the backstop against a builder that fails to report a write
        // that did not fit. A truncated datagram must never reach the wire.
        if (w.writeSignature() && build (w) && w.ok())
            plat.socket->send (to, buf, w.size());
    }

    template <typename Build>
    void sendOne (Build&& build) noexcept
    {
        sendOneTo (peer, static_cast<Build&&> (build));
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

    // One repeat of the Bye we are currently trying to deliver, stamping the retry
    // clock. The reason is resent verbatim so every copy is identical.
    void sendByeNow() noexcept
    {
        lastByeMs = plat.clock->nowMs();
        sendBye (byeReason);
    }
    // (No sendByeReply() to `peer`: a Bye Reply always goes to whoever sent the Bye,
    //  which is not necessarily our peer — see onBye.)

    // Addressed at an arbitrary sender rather than `peer` — these answer whoever
    // sent the offending command, which is not necessarily anyone we know.
    void sendByeTo (const Endpoint& to, ByeReason r) noexcept
    {
        sendOneTo (to, [&] (Writer& w) { return writeBye (w, r); });
    }

    void sendNakTo (const Endpoint& to, NakReason r, const ParsedCommand& c) noexcept
    {
        const std::uint32_t echoed = c.headerWord();
        sendOneTo (to, [&] (Writer& w) { return writeNak (w, r, echoed); });
    }

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
    std::uint16_t     lastRx = 0;      // highest Sequence Number processed
    std::uint64_t     rxWindow = 0;    // bit i => (lastRx - i) already processed

    std::uint32_t     lastRxMs = 0, lastPingMs = 0, lastInviteMs = 0, pingId = 0;

    std::uint32_t     inviteStartMs = 0;                  // when this invitation began
    std::uint32_t     byeStartMs = 0, lastByeMs = 0;      // Pending Bye timers
    ByeReason         byeReason = ByeReason::undefined;   // repeated verbatim
};

} // namespace netmidi2
