# Upgrading libnetmidi2 — what changed since `141a075`

Written for the consumers that reference this library **by local path** rather than
by tag: **M2 SoundGen App** and **midi2-router-service**. A `git pull` in your
`00-MyLibraries/libnetmidi2` checkout plus a rebuild is the whole integration, but
unlike the last one this pull **will not compile straight away** — that is deliberate,
and this document is the list.

Baseline is `141a075`, the commit the previous migration note told you to pull.
25 commits, 313 tests across nine suites, CI green on Ubuntu and macOS,
plus a first bench against macOS Tahoe and a Zephyr Teensy.

---

## 1. Things that will fail to compile

Each of these is a rename or removal chosen *so that* it fails loudly. Silent changes
of meaning were the alternative, and they are worse.

### `ByeReason::userRejected` is **gone**

It was `0x40`, and `0x40` does not mean that. §6.16 Table 27 says `0x40` is
**"Invitation Failed: too many opened sessions"**; the user declining is `0x42`.
Those say opposite things to a peer — *come back later* versus *you are not welcome*.

| You had | Use |
|---|---|
| `ByeReason::userRejected` (meaning "user declined") | `ByeReason::userDidNotAccept` (`0x42`) |
| `ByeReason::userRejected` (meaning "host is full") | `ByeReason::tooManySessions` (`0x40`) |
| `ByeReason::noPendingInvitation` | `ByeReason::noPendingSession` |
| `ByeReason::rejectedNoPrior` | `ByeReason::authRejectedNoPrior` |

The enum is now the complete Table 27, so `userTerminated`, `powerDown`,
`tooManyMissingPackets`, `protocolError`, `usernameNotFound` also exist.

### `State` gained three values

```cpp
enum class State { idle, inviting, authenticating, established, resetting, closing, closed };
```

`authenticating` and `resetting` are the spec's *Authentication Required* and
*Pending Session Reset*; `closing` is *Pending Bye*. **An exhaustive `switch` over
`State` now needs three more arms**, and if you map states to strings, three more
cases — a `default` that falls through to "closed" will lie to you.

### `FecSlot` / `setFecSlots` renamed

```cpp
netmidi2::SentUmpSlot history[2];
session.setSentUmpHistory (history, 2);   // was setFecSlots
session.setFecRepeats (2);                // new: how many to prepend per datagram
```

One history now serves two features, so "FEC slot" was misleading. The array length
is the **Retransmit** depth; `setFecRepeats()` is how many of those get prepended to
each datagram for **FEC**.

### `IDiscovery` changed shape

If you had started an implementation, it will not compile — it now lives in
`Discovery.h`, reports `lost` as well as `found`, and carries the `ProductInstanceId`.
See §3 below. (As far as I know nothing implements it yet.)

---

## 2. Behaviour that changed without a compile error

**These are the dangerous ones.** Read them even if the build is green.

### `close()` is no longer synchronous

It now enters `closing` (the spec's Pending Bye) and repeats the Bye until the peer
acknowledges or `byeTimeoutMs` expires. **Code that treats "called `close()`" as
"session is closed" is now wrong** — wait for `State::closed`.

Why: previously one Bye went out and the state jumped to closed, so a single lost
datagram left the peer holding a session you had already dropped, waiting out its own
idle timeout. §6.16 asks for the retransmission.

### A client no longer invites forever

An unanswered Invitation now expires into Bye `0x04` and Pending Bye, per §6.2. If
anything in your UI assumed "inviting" persists until cancelled, it will now end on
its own after `inviteTimeoutMs` (default 10s).

### A Ping is answered in every state, from anyone

§6.1 Table 9 requires it. Previously a Host busy with one Client looked dead to every
other box on the LAN. This does **not** refresh the liveness timer — answering is not
accepting — but you will see replies going to endpoints you have no session with.

### UMP Data you send may now be accompanied by repeats

Only if you opt in via `setSentUmpHistory()`. Off by default.

### Duplicate detection got stricter, and it matters

The receive window is now 64 entries, not 1. **Against a peer that sends FEC, the old
code delivered every MIDI message up to three times** — every note-on fired three
times. It had not bitten because the Teensy does not send FEC; that was luck, not
compatibility. If you ever saw inexplicable duplicate notes, this was why.

This is now measured rather than argued: macOS Tahoe's CoreMIDI *does* send FEC, and in
a bench run it delivered the same two UMP messages across **seven** UMP Data commands.
The window collapsed them back to two. On the old code that session would have fired
every note three or four times.

### A Host asking for time no longer gets a protocol error

A Host may answer an Invitation with `0x11` Invitation Reply: Pending — "I need a
moment", typically because it is asking a user. We used to reply NAK `0x01`, *command
not supported*. **macOS Tahoe sends `0x11` on every single connection**, so this was
happening on every handshake against it; Tahoe ignored the NAK and carried on, which is
the only reason it worked.

The visible change is timing: the handshake can now legitimately take as long as a
person takes to answer a permission dialog. Once a Host says "pending" we stop repeating
the Invitation and restart the clock against the new `Timing::invitePendingTimeoutMs`
(60s) instead of `inviteTimeoutMs` (10s). **If your UI has its own connect timeout,
check it is not shorter than that**, or you will abandon handshakes the Host is still
working on. Listen for `onInvitationPending()` to show the right thing.

### A NAK no longer always tears the session down

`onNak` already ignored a NAK of a Retransmit Request. It now also absorbs a NAK of a
**zero-length UMP Data** — our §7.2.1 idle declaration — and simply stops sending them
to that peer.

Without this, one non-conformant peer produced a livelock: NAK → tear down → re-invite →
establish → idle timer fires → another zero-length → another NAK. Four full handshakes
in three seconds with no MIDI moving. Found against a Teensy running Zephyr's
`netmidi2.c`. We are right and it is wrong (§7.1 lists a zero-length payload as legal,
§7.2.1 says a Sender *shall* send one, and Tahoe both sends and accepts them) — but a
thrashing session helps nobody.

---

## 3. New capability you have to opt into

Everything here is inert until you supply an adapter. Nothing breaks if you skip it.

### Several Clients on one Host port — `HostPort.h`

§3.2: a Host serves *all* its Clients from one UDP port, distinguishing them by source
address. A bare `Session` holds one peer, so a Host built from a single Session answers
the first Client and **silently ignores every other one**.

This matters the moment discovery works: a Host advertises **one** port, so every
Client that finds you aims at it.

```cpp
Session  a (plat, Role::host, &la, "My Host", "MYHOST-1");
Session  b (plat, Role::host, &lb, "My Host", "MYHOST-1");   // same identity
Session* slots[] = { &a, &b };
HostPort port (plat, slots, 2);
port.listen();
for (;;) port.tick();          // NOT a.tick() / b.tick()
```

Exactly one thing may drain a shared socket. Under `HostPort` you call `port.tick()`
and never `session.tick()`.

### mDNS discovery — `Discovery.h`

Implement `IDiscovery` with `NSNetService` or JUCE's `NetworkServiceDiscovery`,
publishing `_midi2._udp` with **TTL ≤ 60s** (§4.6). Then:

```cpp
port.advertise ("NUBBSOFT1-SoundGen", boundPort, "M2 SoundGen Host", "NUBBSOFT-HOST-1");
```

The library validates the identity and refuses to publish an illegal one. Two traps
worth knowing: the field limits are in **bytes, not characters** (a 34-glyph UTF-8
name is 102 bytes and is rejected), and the **PTR instance name is not the display
name** — the TXT `UMPEndpointName` is. Showing the wrong one in a device picker is the
easy mistake and nothing on the wire objects.

### Authentication — `Auth.h`

Implement `ICrypto` (`sha256` + `randomBytes`) with CommonCrypto, then:

```cpp
host.requireAuthentication ("5483");     // or requireUserAuthentication (&store)
client.setSharedSecret ("5483");
```

**`randomBytes` returning `false` must mean no challenge is issued, never a guessable
one.** The spec's own example secret is `5483` and it calls the shared secret
"typically a 4- or 6-digit number", so the digest is only ever as strong as a PIN — the
nonce is what carries the security, and §6.7's doubling failure delay is the defence.

### Knowing the Host is asking a user — `onInvitationPending()`

```cpp
void onInvitationPending() override   // optional, default does nothing
{
    showStatus ("Waiting for the other device to accept...");
}
```

Purely informational, and worth wiring up: this is the difference between a UI that
says "waiting" and one that looks hung. `writeInvitationPending()` is also now in
`Protocol.h` if you implement the Host half (deciding to stall is your UI's call, not
the transport's).

### Retransmit and Session Reset

Retransmit needs no setup beyond `setSentUmpHistory()`. Session Reset is
`session.resetSession()`; it enters `resetting` and completes on the peer's reply.
Both add optional listener callbacks — `onUmpLost()` and `onSessionReset()` — which
are worth handling: §7.2.4 and §6.11 both suggest an **all-notes-off**, because the
data that went missing may have contained the Note Off.

---

## 4. What is not proven

Stated plainly, because the test count invites more confidence than is warranted.

- **It has now run against real hardware, once, on one network.** On 2026‑09‑19 it was
  benched against **macOS Tahoe's own CoreMIDI Network MIDI 2.0** (the first independent
  conformant implementation it has ever met) and against the Zephyr Teensy: full
  handshake, Ping both ways, a UMP Stream Endpoint Discovery round trip, FEC
  deduplicated, clean Bye. That bench found two defects in one afternoon that the
  nine-suite, 313-check run could not, which is the honest measure of what local tests
  are worth here. Reproduce it with `tools/nm2_bench.cpp`.
  What that does **not** cover: sustained traffic, real musical load, packet loss, Wi-Fi,
  authentication against a non-Apple peer, or more than one concurrent client.
- **The `0x12`/`0x13` authentication framing is inferred, not transcribed.** Those
  tables contradict themselves; the layout is derived from their own stated `pl`
  ranges and argued in `PROTOCOL.md` §3.7. The digests are solid — both match the
  spec's published vectors byte for byte — but the envelope around them is reasoning.
  If a real peer disagrees about those two commands, start there.
- **`0x11` Invitation Reply: Pending — Client side is now implemented; the Host side is
  not.** We honour a Host that asks for time. We never *send* one, because deciding to
  stall while a user is asked is your UI's decision, not the transport's.

Eleven bugs were fixed along the way, several of which no test could previously see:
a remotely triggerable **stack buffer overflow** (a peer claiming 255 UMP words wrote
764 bytes past a 64-word buffer), the FEC triple-delivery above, two handshake paths
that could hang forever, and a Ping Reply that was being sent to an empty endpoint.
`PROTOCOL.md` records five errors found in the specification itself.
