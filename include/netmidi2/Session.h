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

    void handleCommand (const ParsedCommand& c, const Endpoint& from) noexcept
    {
        touch();
        switch (c.code)
        {
            case Command::invitation:
                if (role == Role::host && st != State::established)
                {
                    peer = from;
                    sendInvitationAccepted();
                    setState (State::established);
                }
                break;

            case Command::invitationReplyAccepted:
                if (role == Role::client && st == State::inviting)
                    setState (State::established);
                break;

            case Command::ping:
                if (c.payload) sendPingReply (get32 (c.payload));
                break;

            case Command::pingReply:
                break; // liveness already refreshed by touch()

            case Command::umpData:
                if (st == State::established && c.payloadWords > 0 && c.payload)
                {
                    const std::uint16_t seq = c.cmdSpecific;
                    if (! (haveRx && seq == lastRx))        // ignore exact duplicate
                    {
                        haveRx = true; lastRx = seq;
                        deliverUmp (c.payload, c.payloadWords);
                    }
                }
                break;

            case Command::bye:
                sendByeReply();
                setState (State::closed);
                break;

            case Command::byeReply:
                setState (State::closed);
                break;

            default:
                break; // NAK / unhandled — ignore for Phase 1
        }
    }

    void deliverUmp (const std::uint8_t* payload, std::uint8_t words) noexcept
    {
        if (! listener) return;
        std::uint32_t out[64];
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
