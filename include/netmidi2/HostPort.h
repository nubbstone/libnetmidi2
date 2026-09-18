/*
    libnetmidi2 — one UDP port, many Clients.

    §3.2 is explicit about how a Host serves more than one Client: "A Host shares its
    UDP port with all Clients... When serving multiple Clients, the Host shall
    uniquely identify the connection for each Client via the Client's source IP
    address and UDP port number of the incoming UDP packets."

    A `Session` holds exactly one peer, which is the right shape for one conversation
    but not for a Host. Binding a second port per extra Client is not an alternative:
    a Host advertises ONE port in its mDNS SRV record (§4.3), so every Client that
    discovers it invites that port, and whichever Session owns the socket answers
    first while the rest are ignored. That is how a second box ends up permanently
    stuck at `inviting` against a host that looks perfectly healthy.

    So this owns the socket and the routing, and the Sessions own the conversations:

        Session  a (plat, Role::host, &la, "My Host", "MYHOST-1");
        Session  b (plat, Role::host, &lb, "My Host", "MYHOST-1");
        Session* slots[] = { &a, &b };
        HostPort port (plat, slots, 2);
        port.listen();
        for (;;) port.tick();        // NOT a.tick() / b.tick()

    The slots are N connections to ONE Host identity, not N Hosts -- they share the
    UMP Endpoint Name and Product Instance Id. (A device exposing several distinct
    Hosts allocates a port per Host, §3.2, which means one HostPort each.)

    Freestanding-friendly, like the rest of the core: the slot array is caller-owned
    pointers + a count, so there is no allocation and no fixed upper bound baked in.
*/

#pragma once

#include "Session.h"

namespace netmidi2
{

class HostPort
{
public:
    HostPort (const Platform& platform, Session* const* sessionSlots, std::size_t slotCount) noexcept
        : plat (platform), slots (sessionSlots), count (slotCount) {}

    std::size_t capacity() const noexcept { return count; }

    // Make every free slot ready to accept an Invitation.
    void listen() noexcept
    {
        for (std::size_t i = 0; i < count; ++i)
            slots[i]->listen();
    }

    std::size_t activeCount() const noexcept
    {
        std::size_t n = 0;
        for (std::size_t i = 0; i < count; ++i)
            if (! isFree (slots[i]->state()))
                ++n;
        return n;
    }

    // The Session currently serving `who`, or nullptr. Identity is source address
    // plus source port, per §3.2 -- two Clients behind one NAT differ by port.
    Session* sessionFor (const Endpoint& who) const noexcept
    {
        for (std::size_t i = 0; i < count; ++i)
            if (! isFree (slots[i]->state()) && slots[i]->remote() == who)
                return slots[i];
        return nullptr;
    }

    // Drain the shared socket, route each datagram to its Session, then run every
    // slot's clocks. Replaces tick() on the individual Sessions.
    void tick() noexcept
    {
        std::uint8_t buf[kMaxDatagram];
        Endpoint from;
        for (int guard = 0; guard < 64; ++guard)     // bound work per tick
        {
            const int n = plat.socket->receive (buf, sizeof buf, from);
            if (n <= 0)
                break;
            route (buf, std::size_t (n), from);
        }

        for (std::size_t i = 0; i < count; ++i)
            slots[i]->tickTimers();
    }

private:
    static bool isFree (State s) noexcept
    {
        return s == State::idle || s == State::closed;
    }

    void route (const std::uint8_t* data, std::size_t len, const Endpoint& from) noexcept
    {
        // 1. An endpoint we are already talking to. This is the whole point: the
        //    sender decides which conversation a datagram belongs to.
        if (Session* s = sessionFor (from))
        {
            s->deliver (data, len, from);
            return;
        }

        /* 2. Nobody owns this sender, so give it a free slot. An Invitation
         *    establishes there; anything else finds a sessionless Session and gets
         *    the replies §5.5/§6.16/§7.1 require -- NAK for a command we do not
         *    support, Bye 0x05 for UMP Data with no session, a Bye Reply for a Bye.
         *    Routing strays to a free slot rather than dropping them is what keeps
         *    those answers working once a port is shared.
         *
         *    listen() first, so a slot left `closed` by a previous Client is reusable
         *    rather than dead weight. */
        for (std::size_t i = 0; i < count; ++i)
        {
            if (isFree (slots[i]->state()))
            {
                slots[i]->listen();
                slots[i]->deliver (data, len, from);
                return;
            }
        }

        // 3. Every slot is busy with somebody else.
        refuseWhenFull (data, len, from);
    }

    /*  A full Host owes an Invitation an answer, and the spec has a code for exactly
        this: Bye 0x40, "Invitation Failed: too many opened sessions" (§6.16 Table
        27). Silence here is precisely the failure that reads as "the host is broken"
        -- the Client cannot tell a full host from an absent one, and retries until
        its own invite timeout.

        Only an Invitation is answered. The other replies are all statements about a
        session, and with no free slot there is no session state to make them from;
        a stray from an unknown sender is dropped instead.

        (§6.6 Invitation Reply: Pending is the nicer answer here -- "wait, a slot may
        free up" -- but it is Phase 2.)
    */
    void refuseWhenFull (const std::uint8_t* data, std::size_t len, const Endpoint& from) noexcept
    {
        bool sawInvitation = false;
        parseDatagram (data, len, [&] (const ParsedCommand& c) {
            if (c.code == Command::invitation)
                sawInvitation = true;
        });

        if (! sawInvitation)
            return;

        std::uint8_t out[kMaxDatagram];
        Writer w (out, sizeof out);
        if (w.writeSignature() && writeBye (w, ByeReason::tooManySessions) && w.ok())
            plat.socket->send (from, out, w.size());
    }

    Platform             plat;
    Session* const*      slots;
    std::size_t          count;
};

} // namespace netmidi2
