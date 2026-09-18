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
             State (idle→inviting→established→closing→closed), connect/listen/close/
             sendUmp/tick, ping keepalive, timeout, Bye, sequence-number dedup.
             Delivers received UMP + state changes via ISessionListener.
Platform.h   injected I/O: IUdpSocket (non-blocking send/recv), IClock (millis),
             IDiscovery (mDNS, optional). Bundled in a Platform struct.
HostPort.h   one UDP port, many Clients (§3.2): owns the shared socket and routes
             each datagram to a Session by source endpoint. Host-side only.
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
include/netmidi2/  Protocol.h · Platform.h · Session.h · HostPort.h  (the library)
PROTOCOL.md        the wire contract — a readable profile of M2-124-UM
tests/             conformance_vectors.cpp — byte-exact vs spec Appendix A.1 (unit)
                   protocol_guards.cpp     — malformed/oversized input rejection (unit)
                   session_loopback.cpp    — Host+Client over real localhost UDP
                   host_multiclient.cpp    — one Host port, several Clients (§3.2)
CMakeLists.txt     INTERFACE target `netmidi2` + all four tests (add_test)
README.md          public front page
.github/workflows/ci.yml   build+ctest (ubuntu/macos) + freestanding compile check
LICENSE            MIT (copyright holder is a <COPYRIGHT HOLDER> placeholder — unset)
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
ctest --test-dir build --output-on-failure     # conformance vectors + loopback
```

Three suites:

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
- **`nm2_session_loopback`** (integration, POSIX). Stands up a Host and a Client
  `Session` over real localhost UDP: full handshake → both Established, bidirectional
  UMP delivery, duplicate‑sequence ignored, recovery from a lost InvitationAccepted,
  NAK re‑invite, stranger traffic rejected, oversized UMP Data rejected, the §7.1 /
  §5.5 replies owed to a sender we have no session with, FEC repeats deduplicated
  across a 64‑entry window (including the `0xFFFF` wrap), an unanswered Invitation
  expiring into Bye `0x04`, a Bye retransmitted until acknowledged, liveness timeout,
  graceful Bye → Closed.

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

**Phase 1 is done and interop‑tested** (against a real Teensy peer via the host):
Invitation/Reply, Ping/Bye, NAK, bidirectional UMP Data, both roles, over an
explicit `host:port`.

| Next | |
|---|---|
| mDNS discovery (`_midi2._udp` PTR/SRV/TXT) so peers find each other | planned |
| FEC (redundant UMP Data in a datagram) + Retransmit (`0x80`/`0x81`) | planned |
| Authentication (Invitation with Auth `0x02`/`0x03`, nonce/sha256) | planned |
| ~~Spec Appendix A.1 conformance vectors as a unit test~~ | **done** (`tests/conformance_vectors.cpp`) |

## Reference material

- MIDI Association **M2‑124‑UM "Network MIDI 2.0 (UDP)" v1.0 (2024‑11‑20)** — the
  normative spec. 
- Public overview: https://midi.org/network-midi-2-0-udp-overview

## Housekeeping / gotchas

- **Placeholders:** `LICENSE` copyright holder and the README CI badge URL are set
  (`nubbstone`). Remote: `github.com/nubbstone/libnetmidi2`.
- **Comment hazard:** never write a `*/` inside a block comment (e.g. a path like
  `zsock_*/…`) — it closes the comment early. (Bitten once.)
- The library must stay **buildable on its own** (this repo's CI proves it). If a
  change only makes sense alongside the host, it probably belongs in the host, not here.
