# CLAUDE.md — libnetmidi2 project orientation

Read this before editing. It's the working context for this repository. The public
face of the project is `README.md`; the wire contract is `PROTOCOL.md`; this file is
the "how to work on it" brief.

## What this is

`libnetmidi2` is a small, portable **C++17 implementation of Network MIDI 2.0 (UDP)**
— the MIDI Association's UMP‑over‑UDP transport, specification **M2‑124‑UM v1.0**. It
lets two devices exchange **MIDI 2.0 Universal MIDI Packets over Ethernet/Wi‑Fi** at
full 32‑bit resolution with per‑note expression.

It exists because most platforms ship **no** Network MIDI 2.0. Apple's CoreMIDI, for
example, only has legacy RTP‑MIDI (MIDI 1.0, 7‑bit). This library is the real thing,
small enough to run on a desktop app *and* a microcontroller.

## The ecosystem (who consumes this)

This library is transport/protocol only. It's consumed by:

- **M2 SoundGen Host** — a macOS/JUCE MIDI 2.0 plugin host, a **separate repo**
  (default sibling: `../06-M2_SoundGen_App`). It implements the `Platform.h`
  interfaces with JUCE (`DatagramSocket`, `Time`) inside its `NetworkUmpFrontDoor`,
  and is the reference consumer. It references this repo via CMake `LIBNETMIDI2_DIR`
  (defaults to a sibling of the host repo).
- **A Teensy 4.1 / Zephyr MIDI 2.0 synth** — a bare‑metal peer. It currently runs a
  *different* spec‑compliant Network MIDI 2.0 lib (titou's), and interoperates with
  us **by both following M2‑124‑UM** — which is the whole point of being spec‑faithful.
  Longer term it could compile this same core (that's why the core is embeddable).

Nothing here should depend on those projects. This is a standalone library.

## Prime directives (do not violate)

1. **The core is freestanding‑friendly.** `Protocol.h`, `Platform.h`, and `Session.h`
   must compile with **no exceptions, no RTTI, no heap, no STL containers, no OS
   headers** — so the same code builds on Zephyr / bare‑metal. Callbacks go through
   `ISessionListener` (a virtual interface), **not `std::function`**. Buffers are
   caller‑owned pointers + lengths. CI enforces this with a
   `-fno-exceptions -fno-rtti` compile of `Session.h`. If you reach for `std::vector`,
   `std::function`, exceptions, or `new` in the core, stop.
2. **Spec‑faithful to M2‑124‑UM.** Cite the spec section (§x.y) for any wire
   behaviour. Precedence when they disagree: **MA spec > `PROTOCOL.md` > code**. Fix
   upward, don't paper over.
3. **Injected I/O.** The protocol core is OS‑agnostic. All sockets/clock/mDNS come in
   through the `Platform.h` interfaces (`IUdpSocket` / `IClock` / `IDiscovery`).
   Platform‑specific code lives in *adapters* (in the consumer), never in the core.
4. **Big‑endian on the wire** (network byte order), unsigned, per §5.3.

## Architecture

```
Protocol.h   pure wire format: 4-byte "MIDI" signature, 32-bit command header,
             command-code enum, big-endian build/parse (Writer + parseDatagram).
Session.h    the state machine on top: Role (host/client),
             State (idle→inviting→authenticating→established→resetting→
             closing→closed), connect/listen/close/
             sendUmp/tick, ping keepalive, timeout, Bye, sequence-number dedup.
             Delivers received UMP + state changes via ISessionListener.
Platform.h   injected I/O: IUdpSocket (non-blocking send/recv), IClock (millis),
             IDiscovery (mDNS, optional). Bundled in a Platform struct.
HostPort.h   one UDP port, many Clients (§3.2): owns the shared socket and routes
             each datagram to a Session by source endpoint. Host-side only.
Discovery.h  the mDNS/DNS-SD contract (§4): IDiscovery, DiscoveredHost, the TXT
             field limits and TTL ceiling. No responder here — that's an adapter.
Auth.h       the §6.7–6.10 contract: ICrypto (sha256 + randomBytes), the digest
             construction, constant-time compare. No crypto implemented here.
```

**One Session is one conversation, not one Host.** §3.2 requires a Host to serve all
its Clients from a single port, identifying each by source address+port — and a Host
advertises only one port over mDNS, so binding an extra port per Client is not a
workaround, it just means the second Client to discover you is ignored. Use
`HostPort` with N Session slots for any Host that might face more than one peer.
Exactly one thing may drain a shared socket, so with `HostPort` you call
`port.tick()` and never `session.tick()` — the Session halves are `deliver()` and
`tickTimers()`, which is what HostPort drives.

Consumer flow: implement the `Platform` interfaces → construct a `Session` → call
`connect()` (client) or `listen()` (host) → drive `tick()` frequently → send with
`sendUmp()`, receive via the listener. `tests/session_loopback.cpp` is a complete,
runnable example with POSIX adapters.

## Repository layout

```
include/netmidi2/  Protocol.h · Platform.h · Session.h · HostPort.h
                   Discovery.h · Auth.h
PROTOCOL.md        the wire contract — a readable profile of M2-124-UM
UPGRADING.md       what consumers must change, and what is not yet proven
tests/             conformance_vectors.cpp — byte-exact vs spec Appendix A.1 (unit)
                   protocol_guards.cpp     — malformed/oversized input rejection (unit)
                   session_loopback.cpp    — Host+Client over real localhost UDP
                   host_multiclient.cpp    — one Host port, several Clients (§3.2)
                   discovery.cpp           — mDNS contract + limits, fake adapter (§4)
                   fec_sending.cpp         — FEC repeat order, size cap, idle (§7.2.2)
                   retransmit.cpp          — request/serve/NAK handling (§7.2.3–7.2.4)
                   auth.cpp                — digests vs the spec's examples (§6.7–6.10)
                   session_reset.cpp       — resync both counters (§6.11–6.12)
tools/             nm2_bench.cpp — interop harness: drives a real Session against a
                   real peer and prints every datagram. The only thing that can catch
                   what our own code and our own tests already agree about.
                   nm2_cli.cpp   — a complete endpoint (mDNS advertise + browse,
                   HostPort, client, arpeggiator). Contains the only real IDiscovery
                   adapter in the repo, over Bonjour — keep it in tools/, not the
                   core, and note it cannot set the §4.6 TTL through
                   DNSServiceRegister.
CMakeLists.txt     INTERFACE target `netmidi2` + all nine tests (add_test)
README.md          public front page
.github/workflows/ci.yml   build+ctest (ubuntu/macos) + freestanding compile check
LICENSE            MIT, © Nubbstone
```

## Read these first (in order)

1. `PROTOCOL.md` — the command table, packet header, UMP Data framing, session
   lifecycle, discovery. Everything else implements this.
2. `include/netmidi2/Protocol.h` — how commands are built/parsed.
3. `include/netmidi2/Session.h` — the state machine and timing.

## The wire protocol at a glance

- Every UDP datagram starts with the 32‑bit signature `0x4D494449` ("MIDI"). Max
  datagram 1400 bytes (avoid IP fragmentation).
- Command packet header (32‑bit): `command code | payload length in 32‑bit words |
  16‑bit command‑specific data`, then the payload.
- Phase‑1 command set: **UMP Data `0xFF`** (16‑bit sequence number + UMP words),
  **Invitation `0x01`** / **Reply‑Accepted `0x10`**, **Ping `0x20`** / **Reply
  `0x21`**, **Bye `0xF0`** / **Reply `0xF1`**, **NAK `0x8F`**.
- Sequence numbers are 16‑bit, per‑sender per‑session, wrap at `0xFFFF`; used for
  dedup / ordering / loss detection.
- Discovery (planned) is mDNS/DNS‑SD, service type `_midi2._udp`.

## Build & test

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure     # all nine suites
```

Nine suites:

- **`nm2_conformance_vectors`** (unit, no sockets — builds anywhere). Byte‑for‑byte
  against M2‑124‑UM Appendix A.1 Figures 12–15, in both directions. This is the only
  test that can catch a wire‑format error: the loopback runs our code against our own
  code, so a symmetric mistake cancels out and passes. Keep it that way — if a vector
  fails, **the library is wrong, not the vector** (spec > PROTOCOL.md > code).
- **`nm2_protocol_guards`** (unit, no sockets). The *reject* paths: `Writer` overflow,
  the §7.1 64‑word limit, bad signature, truncated command. Everything here arrives
  from the network, so none of it may be assumed well‑formed.
- **`nm2_host_multiclient`** (integration, POSIX). Two Clients on one Host port:
  both establish, traffic routes by sender with no crosstalk, a third is refused with
  Bye `0x40` rather than silence, and a vacated slot is reused.
- **`nm2_discovery`** (integration, POSIX + a fake mDNS adapter). The §4 contract:
  service type and field limits, identity validation (including the byte‑vs‑glyph
  trap), a browse resolving into a real session, `lost` events, and the §4.4 join —
  that the name a Host *advertises* is the name it *invites with*.
- **`nm2_fec_sending`** (integration, POSIX). §7.2.2 on the sending side: repeats
  prepended oldest‑first with the new command last, the oldest dropped rather than
  bursting 1400 bytes, a round trip proving our own receiver deduplicates what our
  sender emits, and the idle‑period interaction with §7.2.1.
- **`nm2_retransmit`** (integration, POSIX). §7.2.3–7.2.4: serving a request from the
  history, Retransmit Error when it has aged out, Bye `0x05` with no session, gap
  detection and bounded re‑asking, and that a NAK of our request stops the asking
  without tearing the session down.
- **`nm2_auth`** (integration, POSIX + a reference SHA‑256). Both published digest
  vectors byte‑for‑byte, nonce generation, the full challenge/response both ways,
  wrong secret and unknown user rejected, Bye `0x45` for a client that cannot
  authenticate, and a host with no `ICrypto` declining to pretend.
- **`nm2_session_reset`** (integration, POSIX). §6.11–6.12: the exchange both ways,
  UMP refused while pending, repeat‑then‑Bye‑`0x04` when nobody answers, an
  unsolicited Reply provoking one of our own, and — the point — that traffic still
  **flows** afterwards.
- **`nm2_session_loopback`** (integration, POSIX). Stands up a Host and a Client
  `Session` over real localhost UDP: full handshake → both Established, bidirectional
  UMP delivery, duplicate‑sequence ignored, recovery from a lost InvitationAccepted,
  NAK re‑invite, stranger traffic rejected, oversized UMP Data rejected, the §7.1 /
  §5.5 replies owed to a sender we have no session with, the three arms of §6.6
  Invitation Reply: Pending (wait / Bye `0x06` / NAK `0x02`), FEC repeats deduplicated
  across a 64‑entry window (including the `0xFFFF` wrap), an unanswered Invitation
  expiring into Bye `0x04`, a Bye retransmitted until acknowledged, zero‑length idle
  declarations with their backoff (§7.2.1), liveness timeout, graceful Bye → Closed.

**Replying vs accepting.** Several things are answered regardless of who sent them —
an Invitation, a sessionless Ping, a Bye (always acknowledged, §6.16), an Invitation
Reply: Accepted (§6.5), and the error replies (Bye `0x05` for UMP Data with no
session, Bye `0x06` for an Accepted we never invited, NAK `0x01` for a command we
don't support). Answering is *not* accepting: none of them may call `touch()` or
change state, only our own peer's Bye closes our session, and only an endpoint we
actually invited can establish one. Replies go to the **sender**, never to `peer` —
an idle `Session` has no `peer`, and replying there sent Ping Replies to an empty
endpoint for as long as that code existed. That distinction is load‑bearing; see the
`stranger:`, `liveness:`, `spec-reply:` and `dup-accept:` checks, each of which
exists because some part of it was once got wrong.

**Answering is separate from liveness.** A Ping is answered in *any* state and from
*anyone* (§6.1 Table 9, §6.13) — a Host busy with one Client that stayed silent looks
dead to every other box on the LAN. That is only safe because `touch()` is gated on
`fromPeer || sessionless`, so a stranger's Ping gets a reply without refreshing the
idle timer. The two were once welded together, and refusing the Ping *was* how the
timer was protected. Keep them apart; the `liveness:` checks are what prove it.

**Careful with the "tell them" replies.** Each was added to stop a peer transmitting
into silence — but a reply sent in the wrong state is worse than no reply. The
sharpest case: §6.5 says to Bye `0x06` an Accepted with no pending invitation, *and*
to ignore one that arrives when already Established. Since hosts retransmit the
Accepted until they see traffic, checking those in the wrong order makes a client
destroy its own session with the host's own recovery packet. When adding another of
these, ask what happens when it fires against a legitimate retransmission.

**Closing takes time now.** `close()` enters `closing` (the spec's Pending Bye, §6.1)
and repeats the Bye until the peer replies or `byeTimeoutMs` expires — it does NOT
return with the session already closed. Anything waiting for a teardown must watch
for `State::closed`, not assume `close()` finished the job. Same for an unanswered
Invitation: it expires into a Bye and Pending Bye rather than retrying forever
(§6.2). `State` gained `closing` between `established` and `closed`, so a consumer
switching exhaustively over it will need a new arm.

**One history, two features.** `setSentUmpHistory()` lends the Session caller‑owned
storage for recently sent commands; `setFecRepeats()` says how many of those get
prepended to each datagram. FEC (§7.2.2) wants two; Retransmit (§7.2.3) answers
requests from the whole array, so a deeper history serves older requests. A slot is
sized for the largest legal command, so a Session that opts out pays nothing.
Receiving FEC repeats has no switch and never did — §7.2.2 makes coping with them a
receiver `shall`, since the peer may send them whatever we do. When touching FEC, the
rule that looks like style and is not: repeats go **oldest‑first, new command last**.

**Many Clients share one port; many Endpoints do not.** §3.2 has a Host serve all its
Clients from a single UDP port (that is what `HostPort` is for), while §9 requires each
UMP Endpoint a device exposes to have **its own** Host instance, port and Endpoint
Name — so one `HostPort` per Endpoint, N Session slots inside each. A device acting as
both Host and Client for the same Endpoint (§8) uses the same Endpoint Name and
Product Instance Id in both roles, on separate ports, or peers cannot tell the two
halves are one device. PROTOCOL.md §4.5.

**Resetting means BOTH counters.** §6.11 resets sequence numbers to zero on each
end. Clearing only `txSeq` fails silently: the peer restarts at `0`, our replay
window still holds the old numbers, and everything after the reset is dropped as a
duplicate while the session looks perfectly healthy. `applyReset()` clears the send
counter, the receive window, the resend history and the gap state together — keep it
that way, and note the test asserts traffic *flows* after a reset rather than that
one occurred.

**`close()` owes a Bye from every live state.** It is written as "anything but idle
or closed" rather than a list, because it once listed only `established` and
`inviting` — so closing from `authenticating` or `resetting` skipped the Bye entirely
and left the peer to time out on its own. A new state inherits the right behaviour
automatically now; do not turn it back into a list.

**No crypto in this library.** SHA‑256 and entropy come through `ICrypto`, so a
platform uses CommonCrypto, mbedTLS, or a hardware SHA engine. Do not add a built‑in
hash — it would ship one implementation everywhere and make accelerators unreachable.
The security here rests on the **nonce**, not the hash: the spec's own example secret
is `5483`, so the digest is only as strong as a PIN, and `randomBytes` returning
false must mean no challenge is issued rather than a guessable one. §6.7's doubling
failure delay *is* the defence, and digest comparison must stay constant‑time.

**The `0x12`/`0x13` framing is inferred, not transcribed.** Those tables contradict
themselves; PROTOCOL.md §3.7 argues the reading from their own `pl` ranges. If a real
peer disagrees about those two commands, start there — the digests are solid.

**A NAK is not automatically a session problem, and re-inviting can be a loop.**
Before the generic "re-invite" default takes on a new command, ask: *would a fresh
session send this again, unprompted?* If yes, the default is not one extra round-trip,
it is a livelock. Not hypothetical — the Teensy NAKs our zero-length idle declarations
(§7.2.1) as malformed, which produced four full handshakes in three seconds with no UMP
moving between them, until `onNak` learned to recognise them and set
`peerAcceptsIdleDeclarations = false` instead. The peer is wrong there (§7.1 lists a
zero-length payload as legal and §7.2.1 says a Sender *shall* send one; Tahoe both
sends and accepts them) — but being right is not a reason to let a session thrash.

**A NAK is not automatically a session problem.** `onNak` reads the echoed command
header (§6.15) before reacting. A NAK of our Retransmit Request means only that the
peer does not implement Retransmit; the generic "re‑invite" response would tear down
a healthy session for asking a question. Any new command we send needs the same
thought before it inherits the default.

**Discovery is a contract here, not an implementation.** `Discovery.h` has no mDNS
responder and must not grow one — multicast, record encoding and a TTL cache are
adapter territory (prime directives 1 and 3). What *does* belong here is anything the
spec constrains that can be checked without a network: the TXT limits (bytes, not
glyphs), the TTL ceiling, and the §4.4 rule that the advertised `UMPEndpointName` is
the same string the Invitation Reply carries. That last one is a cross‑layer
invariant nothing on the wire enforces, so it is pinned by a test.

**Note on sanitizers:** `-fsanitize=address` is broken on this machine — even a
hello‑world ASan binary hangs with no output. Use `-fstack-protector-all` (it caught
the overflow above cleanly, SIGABRT) rather than assuming your code is at fault.

## Conventions

- C++17. Match the surrounding style (JUCE‑ish spacing, `noexcept` on core methods).
- Header‑only core; keep it that way unless there's a strong reason. A stateful
  `Session` is inline in the header today.
- Every new wire behaviour: cite the M2‑124‑UM section, update `PROTOCOL.md` if the
  contract changes, and add/extend a test where practical.
- Prefer conformance vectors: M2‑124‑UM **Appendix A.1** has example byte‑for‑byte
  packets, transcribed in `tests/conformance_vectors.cpp` — the cheapest interop
  guarantee there is. Extend it whenever the spec publishes a vector for a command
  you touch. Note the A.1 byte grids are **figures/images**: `pdftotext` cannot read
  them, so render the page (`pdftoppm -f 50 -l 51 -r 300 -png`) and read it — and
  read at 300 dpi, because `0x02` and `0x00` are indistinguishable at thumbnail size.

## Status & roadmap

**Bench-tested against macOS Tahoe** (2026‑09‑19, CoreMIDI's own Network MIDI 2.0,
enabled in Audio MIDI Setup → MIDI Network Setup → "Network MIDI 2.0 Session 1").
That is the first run against an independent, conformant implementation, and it found
two defects the whole nine-suite run could not — see `tools/nm2_bench.cpp`, which is
how to do it again. Build with `-DNETMIDI2_BUILD_TOOLS=ON`; `nm2_bench browse` lists
peers, `nm2_bench client <host> <port> --probe` runs a full session using a UMP Stream
Endpoint Discovery as the payload, which is silent and therefore safe against a live
rig (`--note` is the audible opt-in). **Do not treat the other `_midi2._udp` boxes on
that LAN as a reference** — they run older, unvetted builds of this very library, so
agreeing with them proves nothing.

**Phase 1 is done and interop‑tested** (against a real Teensy peer via the host):
Invitation/Reply, Ping/Bye, NAK, bidirectional UMP Data, both roles, over an
explicit `host:port`.

| Next | |
|---|---|
| ~~mDNS discovery contract + orchestration~~ | **done** (`Discovery.h`; the responder itself is adapter work) |
| ~~FEC sending~~ (receiving always worked) | **done** (`Session::setFecSlots`) |
| ~~Retransmit (`0x80`/`0x81`)~~ | **done** (shares the sent‑UMP history with FEC) |
| ~~Authentication (`0x02`/`0x03`, `0x12`/`0x13`)~~ | **done** (`Auth.h`; SHA‑256 + entropy injected via `ICrypto`) |
| ~~Session Reset (`0x82`/`0x83`)~~ | **done** |
| ~~Invitation Reply: Pending (`0x11`, §6.6) — Client side~~ | **done.** Not optional after all: macOS Tahoe sends `0x11` on *every* connection. We wait, with a long budget, and stop repeating the Invitation. |
| Invitation Reply: Pending (`0x11`) — **Host** side | still open, and still the consumer's call: *deciding* to stall while a user is asked is UI, not transport. `writeInvitationPending()` is there when they want it. |
| ~~Spec Appendix A.1 conformance vectors as a unit test~~ | **done** (`tests/conformance_vectors.cpp`) |

## Reference material

- MIDI Association **M2‑124‑UM "Network MIDI 2.0 (UDP)" v1.0 (2024‑11‑20)** — the
  normative spec. 
- Public overview: https://midi.org/network-midi-2-0-udp-overview

## Housekeeping / gotchas

- **Consumers integrate by local path, not by tag.** When you change public API,
  update `UPGRADING.md` — it is what the M2 SoundGen App and midi2-router-service read
  before pulling. Prefer a rename that fails to compile over a silent change of
  meaning, and say so there.
- **Placeholders:** `LICENSE` copyright holder and the README CI badge URL are set
  (`nubbstone`). Remote: `github.com/nubbstone/libnetmidi2`.
- **Comment hazard:** never write a `*/` inside a block comment (e.g. a path like
  `zsock_*/…`) — it closes the comment early. (Bitten once.)
- The library must stay **buildable on its own** (this repo's CI proves it). If a
  change only makes sense alongside the host, it probably belongs in the host, not here.
