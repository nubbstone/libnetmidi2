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

/*  One previously-sent UMP Data command, kept so it can be sent again.

    Two features share this history, which is why it is not called a FEC slot: FEC
    (§7.2.2) repeats the most recent few inside every new datagram, and Retransmit
    (§7.2.3) serves them on request. They want different depths -- FEC wants two,
    a retransmit buffer wants as many as you can afford -- so the array length sets
    the retransmit depth and setFecRepeats() sets how many of those get prepended.

    Caller-owned, like every other buffer here (prime directive 1). A slot is sized
    for the largest legal command (64 words), so this is real memory and a Session
    that opts out pays none of it.
*/
struct SentUmpSlot
{
    std::uint16_t seq = 0;
    std::uint8_t  wordCount = 0;
    std::uint32_t words[kMaxUmpWordsPerCommand] = {};
};

class ISessionListener
{
public:
    virtual ~ISessionListener() = default;
    virtual void onUmpReceived (const std::uint32_t* words, std::uint8_t count) = 0;
    virtual void onStateChanged (State) {}

    /*  UMP with this Sequence Number is gone for good: the peer answered a Retransmit
        Request with a Retransmit Error, or never answered at all.

        Worth acting on rather than logging. §7.2.4: "the requested UMP data is
        probably lost and cannot be retrieved. The Device may determine a recovery
        process appropriate to its own implementation and the unique circumstances,
        such as triggering an all-notes off" -- because the datagram that went missing
        may well have carried the Note Off. Default is to do nothing. */
    virtual void onUmpLost (std::uint16_t /*sequenceNumber*/) {}
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

        /*  Idle declaration (§7.2.1). The first zero-length UMP Data "shall be sent
            within 300ms after the most recent UMP Data Command which had a non-zero
            length"; after that the interval should grow and eventually stop. Each
            successive gap is idleDeclareMs << n, so 250 / 500 / 1000 / 2000 / 4000
            by default, then silence.

            Set idleDeclareCount to 0 to send none -- the spec asks a Sender to
            consider that "the Receiver may have restrictions such as battery
            operation or limited processing in which it would prefer to not
            consistently receive data". */
        std::uint32_t idleDeclareMs = 250;
        std::uint8_t  idleDeclareCount = 5;

        /*  Retransmit (§7.2.3). "The Device should delay sending the Retransmit
            Request Command for a short duration, for example 10 milliseconds. That
            will help recovering from out of order packets and it prevents sending
            Retransmit Requests too often." Then "repeated (with increasing delay)"
            -- 10, 20, 40 here -- until the data arrives, an Error or NAK comes back,
            or we give up and report the loss. */
        std::uint32_t retransmitDelayMs = 10;
        std::uint8_t  retransmitMaxRequests = 3;
    };

    Session (const Platform& platform, Role role, ISessionListener* listener,
             const char* endpointName, const char* productInstanceId) noexcept
        : plat (platform), role (role), listener (listener),
          name (endpointName), productId (productInstanceId) {}

    void setTiming (Timing t) noexcept { timing = t; }

    /*  Lend the Session somewhere to keep recently sent UMP Data commands. This one
        array powers both resend features, and `count` is how deep it goes:

          - FEC (§7.2.2) repeats the most recent setFecRepeats() of them in every new
            datagram. Defaults to 2, the spec's recommendation.
          - Retransmit (§7.2.3) answers a peer's request from anything still held, so
            a longer history can satisfy older requests.

        No slots (the default) disables both: we send no FEC repeats, and a Retransmit
        Request gets a Retransmit Error because the buffer is empty. Off by default
        because it is the caller's memory and the embedded target is the one that
        cares -- RECEIVING FEC has always worked and needs no opt-in, since a peer may
        send repeats whatever we do (§7.2.2 makes coping with them a receiver `shall`).
    */
    void setSentUmpHistory (SentUmpSlot* slots, std::uint8_t count) noexcept
    {
        history         = slots;
        historyCapacity = (slots != nullptr) ? count : std::uint8_t (0);
        historyUsed     = 0;
        if (fecRepeats > historyCapacity)
            fecRepeats = historyCapacity;
    }

    /*  How many recent commands to prepend to each datagram for FEC (§7.2.2).
        Clamped to the history length. 0 keeps the history for Retransmit only and
        sends no repeats. */
    void setFecRepeats (std::uint8_t repeats) noexcept
    {
        fecRepeats = (repeats < historyCapacity) ? repeats : historyCapacity;
    }

    State state() const noexcept { return st; }
    const Endpoint& remote() const noexcept { return peer; }

    /*  Client: invite a Host found by discovery (§4).

        The only difference from the explicit-address form is where the address came
        from -- discovery resolves SRV+A into exactly the host:port a user could have
        typed. Deliberately does NOT adopt the advertised UMP Endpoint Name as our
        own: that string is the HOST's identity, and this Session's `name` is ours.
    */
    void connect (const DiscoveredHost& host) noexcept
    {
        Endpoint e {};
        std::size_t i = 0;
        for (; i + 1 < sizeof e.address && host.address[i]; ++i)
            e.address[i] = host.address[i];
        e.address[i] = '\0';
        e.port = host.port;
        connect (e);
    }

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
        if (count > kMaxUmpWordsPerCommand)
            return false;                       // §7.1 Table 29

        std::uint8_t buf[kMaxDatagram];
        Writer w (buf, sizeof buf);
        if (! w.writeSignature())
            return false;

        prependFecRepeats (w, kHeaderBytes + std::size_t (count) * 4);

        if (! (writeUmpData (w, txSeq, words, count) && w.ok()))
            return false;

        retainForFec (txSeq, words, count);
        ++txSeq;

        // Real data restarts the idle clock and the backoff (§7.2.1): the first
        // zero-length declaration is measured from the last NON-zero-length command.
        if (count > 0)
        {
            haveSentUmp = true;
            idleSent    = 0;
        }
        lastUmpTxMs = plat.clock->nowMs();

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
            declareIdleIfDue (now);
            requestRetransmitIfDue (now);
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
     *   Ping, in ANY state and from anyone. §6.1 Table 9 lists Ping among the
     *   commands valid in "Every State", and §6.13 says "A Host or Client may send a
     *   Ping Command at any time and in any Session State" -- it is how a peer
     *   checks we are alive before bothering to invite us. We used to answer only
     *   while sessionless, which meant a Host busy with one Client looked dead to
     *   everybody else, exactly when a prober most wants to know otherwise.
     *
     *   Answering is safe purely because touch() is gated below: a stranger's Ping
     *   is replied to but does NOT refresh the liveness timer, so it cannot hold a
     *   dead session open. That gate is the load-bearing part -- the two used to be
     *   welded together, and refusing the Ping was how the timer was protected. The
     *   `liveness:` checks exist to keep them separate. */
    static bool admits (Command code, bool fromPeer, bool sessionless) noexcept
    {
        (void) sessionless;
        if (fromPeer)
            return true;
        return code == Command::invitation
            || code == Command::bye
            || code == Command::invitationReplyAccepted
            || code == Command::ping;
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
            case Command::retransmitRequest:
            case Command::retransmitError:
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

        /* §7.1 for UMP Data, and §7.2.3 / §7.2.4 say the same of both Retransmit
         * commands: received "outside of an Established Session... shall respond with
         * a Bye Command with reason 0x05". Same rule, same three commands. */
        const bool needsSession = (c.code == Command::umpData
                                   || c.code == Command::retransmitRequest
                                   || c.code == Command::retransmitError);
        if (needsSession && ! (fromPeer && st == State::established))
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
            case Command::pingReply:               onPingReply (c);        break;
            case Command::umpData:                 onUmpData (c);          break;
            case Command::bye:                     onBye (from, fromPeer); break;
            case Command::byeReply:                onByeReply();           break;
            case Command::nak:                     onNak (c);              break;
            case Command::retransmitRequest:       onRetransmitRequest (c); break;
            case Command::retransmitError:         onRetransmitError();     break;
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

    /*  §6.14: the Ping Id exists "to match a received Ping Reply Command to a
        previously sent Ping Command", so match it rather than taking any Ping Reply
        as proof of life. Liveness itself is refreshed by touch() on the way in, as
        it is for every command from our peer; this is the narrower question of
        whether the peer answered the specific Ping we asked.

        We deliberately do NOT send NAK 0x20 (Bad Ping Reply) on a mismatch, though
        §6.14 permits it. It is a "may", and every way of triggering it here is
        something UDP does routinely rather than something a peer did wrong: a
        duplicated datagram arrives after we cleared the outstanding id, or a reply
        to the previous Ping lands after we have already sent the next one. NAKing a
        peer for the network's behaviour is worse than staying quiet, and a NAK is
        not free -- our own NAK handler treats one as a reason to re-invite.
    */
    void onPingReply (const ParsedCommand& c) noexcept
    {
        if (pingOutstanding && c.payload && get32 (c.payload) == outstandingPingId)
            pingOutstanding = false;
        // A mismatch or an unsolicited reply is simply not evidence; ignore it.
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
            /* A jump of more than one means the numbers in between never arrived
             * (§5.6: "the Sequence Number can be used to detect duplicate, unordered,
             * and missing UMP Data Commands"). Remember the first of them. One gap is
             * tracked at a time, the oldest, because asking for that one asks for
             * everything after it too -- a Retransmit Request means "send all
             * previously sent UMP Data Commands starting from Sequence Number". */
            if (ahead > 1 && ! gapPending)
            {
                gapPending         = true;
                gapSeq             = std::uint16_t (lastRx + 1);
                gapSinceMs         = plat.clock->nowMs();
                retransmitRequests = 0;
            }

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

    // Has this Sequence Number already been processed? Anything older than the
    // window counts as seen -- we can no longer tell, and re-requesting it would be
    // worse than assuming.
    bool hasSeen (std::uint16_t seq) const noexcept
    {
        if (! haveRx)
            return false;
        const std::uint16_t behind = std::uint16_t (lastRx - seq);
        if (behind >= 0x8000u)
            return false;                       // ahead of us; not yet seen
        if (behind >= kRxWindowBits)
            return true;                        // fell out of the window
        return (rxWindow & (std::uint64_t (1) << behind)) != 0;
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

    /*  Serve a peer's Retransmit Request from the history (§7.2.3).

        The spec contradicts itself about what to do when only SOME of the requested
        range survives. §7.2.3: "All UMP Data Commands that are available in the
        retransmit buffer following the missing packets should still be
        retransmitted." §7.2.4: "The Device shall not retransmit other available UMP
        Data Commands." Those cannot both be followed.

        We follow §7.2.3, the more specific of the two and the only one that helps:
        sending what survived costs one datagram the requester's dedup window will
        sort out, while withholding it loses data that was right there. PROTOCOL.md
        §5.4 records the conflict so the choice is visible rather than accidental.
    */
    void onRetransmitRequest (const ParsedCommand& c) noexcept
    {
        const std::uint16_t wanted = c.cmdSpecific;

        if (historyUsed == 0)
        {
            // Nothing held at all. txSeq is the next number we will use, so it is
            // the honest answer to "what is the earliest you could give me".
            sendRetransmitError (RetransmitError::notInTransmitBuffer, txSeq);
            return;
        }

        /* Search by stored order, not by comparing numbers: the history is in send
         * order, so a range spanning the 0xFFFF -> 0x0000 wrap needs no special case.
         * §7.2.3 warns about exactly this -- "the UMP Data Commands to be
         * retransmitted could start with a high number close to the maximum, and then
         * wrap around to 0x0000. The Sender should order the Retransmit buffer
         * considering this wrap-around." Insertion order already does. */
        std::uint8_t start = historyUsed;
        for (std::uint8_t i = 0; i < historyUsed; ++i)
            if (history[i].seq == wanted)
            {
                start = i;
                break;
            }

        if (start < historyUsed)
        {
            retransmitFrom (start);
            return;
        }

        // Not held. Table 31's Sequence Number field is "the first UMP Data Command
        // that could be retransmitted", so name the oldest we still have.
        sendRetransmitError (RetransmitError::notInTransmitBuffer, history[0].seq);

        /* Then §7.2.3's "still retransmit what follows". Only meaningful if the
         * request is for something OLDER than our oldest, in which case the whole
         * buffer follows the gap. A request for a number we have not sent yet gets
         * the error alone. Wrap-safe 16-bit comparison. */
        const std::uint16_t oldestAhead = std::uint16_t (history[0].seq - wanted);
        if (oldestAhead != 0 && oldestAhead < 0x8000u)
            retransmitFrom (0);
    }

    // Send history[start..] as UMP Data, splitting across datagrams at the 1400-byte
    // limit. "The sending entity should always send all requested UMP Data Commands
    // without any gaps" (§7.2.3), so this never skips one to make things fit.
    void retransmitFrom (std::uint8_t start) noexcept
    {
        std::uint8_t buf[kMaxDatagram];
        Writer w (buf, sizeof buf);
        if (! w.writeSignature())
            return;
        int packed = 0;

        for (std::uint8_t i = start; i < historyUsed; ++i)
        {
            const std::size_t sz = kHeaderBytes + std::size_t (history[i].wordCount) * 4;
            if (packed > 0 && w.size() + sz > kMaxDatagram)
            {
                if (w.ok())
                    plat.socket->send (peer, buf, w.size());
                w = Writer (buf, sizeof buf);
                if (! w.writeSignature())
                    return;
                packed = 0;
            }
            if (! writeUmpData (w, history[i].seq, history[i].words, history[i].wordCount))
                break;
            ++packed;
        }

        if (packed > 0 && w.ok())
            plat.socket->send (peer, buf, w.size());
    }

    void sendRetransmitError (RetransmitError reason, std::uint16_t firstAvailable) noexcept
    {
        sendOne ([&] (Writer& w) { return writeRetransmitError (w, reason, firstAvailable); });
    }

    /*  The peer cannot give us what we asked for (§7.2.4). "the requested UMP data is
        probably lost and cannot be retrieved" -- so stop asking and tell the
        application, which is the only layer that knows whether a missing note matters
        enough to warrant an all-notes-off. */
    void onRetransmitError() noexcept
    {
        abandonGap();
    }

    void abandonGap() noexcept
    {
        if (! gapPending)
            return;
        gapPending = false;
        if (listener)
            listener->onUmpLost (gapSeq);
    }

    /*  Ask for the gap, once the short settling delay has passed (§7.2.3).

        The delay is not politeness: "That will help recovering from out of order
        packets and it prevents sending Retransmit Requests too often." Most gaps on a
        LAN are reordering, and most of the rest are filled by the FEC repeat in the
        next datagram before this timer ever fires -- so the common case costs nothing
        on the wire. */
    void requestRetransmitIfDue (std::uint32_t now) noexcept
    {
        if (! gapPending)
            return;

        if (hasSeen (gapSeq))          // arrived on its own, or via FEC
        {
            gapPending = false;
            return;
        }

        if (! peerDoesRetransmit)      // it NAKed us; §7.2.3 says stop asking
        {
            abandonGap();
            return;
        }

        if (retransmitRequests >= timing.retransmitMaxRequests)
        {
            abandonGap();
            return;
        }

        const std::uint32_t due = timing.retransmitDelayMs << retransmitRequests;
        if (now - gapSinceMs < due)
            return;

        gapSinceMs = now;
        ++retransmitRequests;
        sendOne ([&] (Writer& w) { return writeRetransmitRequest (w, gapSeq, 0); });
    }

    void onNak (const ParsedCommand& c) noexcept
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
        /* A NAK echoes the header of the command it is complaining about (§6.15), so
         * read it before reacting. A NAK of our Retransmit Request means only that
         * the peer does not implement Retransmit -- §7.2.3: "it shall reply to the
         * Retransmit Request Command with a NAK Command with reason 0x01... The
         * remote Device should not send Retransmit Request Commands after that."
         *
         * Without this check the generic handler below would tear down a perfectly
         * healthy session and re-invite, because we asked a question the peer does
         * not answer. Stop asking instead. */
        if (c.payload != nullptr && c.payloadWords >= 1)
        {
            const Command offending = Command (std::uint8_t (get32 (c.payload) >> 24));
            if (offending == Command::retransmitRequest)
            {
                peerDoesRetransmit = false;
                abandonGap();
                return;
            }
        }

        if (role == Role::client && st == State::established)
        {
            setState (State::idle);
            connect (peer);
        }
    }

    /*  Write the retained commands into `w`, oldest first.

        §7.2.2 "FEC Packet Order" makes the order normative, not stylistic:
        "Previous UMP payloads shall be prepended in the order in which they were
        sent. This is to allow the receiving Device to read each UMP Data Command in
        order in which it is received and just skip over the UMP Data Commands it has
        already processed." Emit them newest-first and a conforming receiver walking
        forward sees sequence numbers going backwards.

        `reserveBytes` is what the new command still needs. If everything will not fit
        inside one datagram, the OLDEST repeats are dropped: a command already
        repeated twice has had its chances, while the most recent one has had fewest,
        and the new command is never the thing sacrificed.
    */
    void prependFecRepeats (Writer& w, std::size_t reserveBytes) noexcept
    {
        if (historyUsed == 0 || fecRepeats == 0)
            return;

        // Only the most recent fecRepeats entries are FEC material; the rest of the
        // history exists to answer Retransmit Requests, not to pad every datagram.
        const std::uint8_t oldest = (historyUsed > fecRepeats)
                                        ? std::uint8_t (historyUsed - fecRepeats)
                                        : std::uint8_t (0);
        std::uint8_t first = historyUsed;
        std::size_t  used  = 0;
        for (std::uint8_t i = historyUsed; i-- > oldest; )
        {
            const std::size_t sz = kHeaderBytes + std::size_t (history[i].wordCount) * 4;
            if (w.size() + used + sz + reserveBytes > kMaxDatagram)
                break;
            used  += sz;
            first  = i;
        }

        for (std::uint8_t i = first; i < historyUsed; ++i)
            writeUmpData (w, history[i].seq, history[i].words, history[i].wordCount);
    }

    /*  Keep this command for repeating in later datagrams. A FIFO: oldest drops out.

        Zero-length commands are NOT retained. They carry no data to recover, and
        §7.2.1 already has them repeating on their own schedule -- retaining them
        would push the real commands out of the history to protect packets whose
        entire content is "nothing to say".
    */
    void retainForFec (std::uint16_t seq, const std::uint32_t* words, std::uint8_t count) noexcept
    {
        if (historyCapacity == 0 || count == 0)
            return;

        if (historyUsed == historyCapacity)
        {
            for (std::uint8_t i = 1; i < historyUsed; ++i)
                history[i - 1] = history[i];
            --historyUsed;
        }

        SentUmpSlot& slot = history[historyUsed++];
        slot.seq       = seq;
        slot.wordCount = count;
        for (std::uint8_t i = 0; i < count; ++i)
            slot.words[i] = words[i];
    }

    /*  §7.2.1: "If a Sender has a period where there is no UMP data to send, the
        Sender shall send a Zero Length UMP Data Command to inform the Receiver that
        the Sender currently has no further UMP data." The first one is due within
        300ms of the last non-zero-length command, then "with increasingly longer
        interval times", and the Sender "should expand the interval between each
        further Zero Length UMP Data Command and eventually stop".

        Why it matters to the other end: without it, a sender that simply has nothing
        to play is indistinguishable from one that has died or lost its route. The
        Receiver is left waiting for its own idle timeout to decide. A zero-length
        command says "still here, nothing to say" in 12 bytes, and because it carries
        a Sequence Number like any other UMP Data (§7.2.1) it also keeps the
        receiver's gap detection honest across a quiet patch.

        Only armed once we have actually sent UMP data: the spec measures the first
        declaration from "the most recent UMP Data Command which had a non-zero
        length", so an endpoint that has never sent any has nothing to be idle from.
        A receive-only peer therefore stays silent rather than chattering.
    */
    void declareIdleIfDue (std::uint32_t now) noexcept
    {
        if (! haveSentUmp || idleSent >= timing.idleDeclareCount)
            return;

        // 250, 500, 1000, 2000, 4000... measured from the last thing we sent.
        const std::uint32_t due = timing.idleDeclareMs << idleSent;
        if (now - lastUmpTxMs < due)
            return;

        std::uint8_t buf[kMaxDatagram];
        Writer w (buf, sizeof buf);
        if (! w.writeSignature())
            return;

        /* §7.2.2: on entering an idle period a FEC sender "should send multiple UDP
         * packets with the last UMP Data Commands prior to the idle period with Zero
         * Length UMP Data Command(s)... up to the number of FEC data repeats the
         * Sender is currently using". So the first few declarations carry the
         * repeats -- giving the last real commands extra chances precisely when no
         * new traffic will -- and the later, sparser ones go out bare. */
        if (idleSent < fecRepeats)
            prependFecRepeats (w, kHeaderBytes);

        if (! (writeUmpData (w, txSeq, nullptr, 0) && w.ok()))
            return;

        ++txSeq;
        ++idleSent;
        lastUmpTxMs = now;
        plat.socket->send (peer, buf, w.size());
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
        outstandingPingId = id;
        pingOutstanding   = true;
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
    SentUmpSlot*      history = nullptr;      // caller-owned; null = FEC sending off
    std::uint8_t      historyCapacity = 0;    // retransmit depth (§7.2.3)
    std::uint8_t      historyUsed = 0;         // how many slots currently hold data
    std::uint8_t      fecRepeats = 2;          // how many to prepend (§7.2.2)

    bool              gapPending = false;      // a Sequence Number we are missing
    std::uint16_t     gapSeq = 0;
    std::uint32_t     gapSinceMs = 0;
    std::uint8_t      retransmitRequests = 0;  // how many times we have asked
    bool              peerDoesRetransmit = true;  // until it NAKs us (§7.2.3)

    std::uint32_t     lastUmpTxMs = 0;         // last UMP Data we sent (§7.2.1)
    bool              haveSentUmp = false;     // ...of non-zero length, ever
    std::uint8_t      idleSent = 0;            // zero-length declarations this idle run
    std::uint32_t     outstandingPingId = 0;   // the Ping we are waiting on (§6.14)
    bool              pingOutstanding = false;

    std::uint32_t     inviteStartMs = 0;                  // when this invitation began
    std::uint32_t     byeStartMs = 0, lastByeMs = 0;      // Pending Bye timers
    ByeReason         byeReason = ByeReason::undefined;   // repeated verbatim
};

} // namespace netmidi2
