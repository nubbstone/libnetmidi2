# libnetmidi2

A small, portable, dependency‑free **C++17 implementation of Network MIDI 2.0
(UDP)** — the MIDI Association's UMP‑over‑UDP transport, spec **M2‑124‑UM v1.0**.

![CI](https://github.com/nubbstone/libnetmidi2/actions/workflows/ci.yml/badge.svg)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

Send and receive **MIDI 2.0 Universal MIDI Packets over Ethernet/Wi‑Fi** with full
32‑bit resolution and per‑note expression — on desktop, and on a microcontroller.

## Why

Network MIDI 2.0 is only just arriving in operating systems. **macOS 26 (Tahoe) is
the first mainstream OS to ship it** — before that, Apple offered only legacy
**RTP‑MIDI (MIDI 1.0)**, which collapses everything to 7 bits. Most other platforms,
and every bare‑metal target, still have nothing.

So this library exists to put the real transport wherever you need it:

- **Platforms that have no implementation** — Windows, Linux, and embedded targets.
- **Microcontrollers** — the core has no heap, no STL and no OS headers, so the same
  code that runs in your desktop app runs on a Teensy or under Zephyr.
- **macOS before 26**, where CoreMIDI's network transport is still MIDI 1.0 only.
- **Control over the transport** — you own the socket, the timing and the buffers,
  which matters when you are building a router or a synth rather than an app that
  happens to speak MIDI.

Where an OS *does* ship Network MIDI 2.0, this library talks to it. That is the
point of being spec‑faithful rather than merely self‑consistent.

## Interop

Verified on the bench against independent implementations, not just against itself:

| Peer | Result |
|---|---|
| **macOS 26 Tahoe — CoreMIDI Network MIDI 2.0** | Full handshake, Ping both ways, UMP Stream round trip, its FEC correctly deduplicated, clean Bye |
| **Teensy 4.1 / Zephyr** (titou's `netmidi2.c`) | Full handshake, Ping, UMP Stream round trip, clean Bye |

This matters more than the test count. The nine suites run our code against our own
code, so a symmetric misreading of the spec passes all of them — and two did, until
a real peer disagreed. [`tools/nm2_bench.cpp`](tools/nm2_bench.cpp) is the harness
that found them, and how to repeat the exercise.

Scope of that testing, stated plainly: one afternoon, one LAN. It does not cover
sustained musical load, packet loss, Wi‑Fi, or authentication against a non‑Apple
peer.

## Tests: what each one is worth

313 checks across nine suites, all passing. The count on its own is close to
meaningless, so here is what each suite actually establishes — and, more usefully,
what it structurally cannot.

| Suite | Checks | What it proves | What it cannot |
|---|---:|---|---|
| `conformance_vectors` | 25 | Byte‑exact framing against the spec's own Appendix A.1 figures, in both directions. The only suite that can catch a wire‑format error. | Anything time‑dependent — it never opens a session |
| `protocol_guards` | 43 | The reject paths: `Writer` overflow, the §7.1 64‑word cap, bad signature, truncated commands | That well‑formed input is handled *correctly* |
| `session_loopback` | 90 | The whole lifecycle over real UDP: handshake, dedup across the 64‑entry window, idle declarations, §6.6's three arms, timeouts, graceful close | Any symmetric misreading of the spec — both ends are our code |
| `host_multiclient` | 23 | §3.2 routing: several Clients on one port, no crosstalk, Bye `0x40` when full, slot reuse | Scale past a handful, or real network conditions |
| `discovery` | 39 | The §4 contract: TXT limits in **bytes**, the TTL ceiling, and the §4.4 rule that the advertised name is the invited name | That any real responder behaves — the mDNS adapter here is a fake |
| `fec_sending` | 20 | §7.2.2 ordering (oldest first, new command last), the 1400‑byte cap, the idle interaction | That a real peer's FEC resembles ours |
| `retransmit` | 25 | §7.2.3–7.2.4 serve, ask, and give up; and that a NAK of our request does not tear the session down | Recovery under genuine packet loss |
| `auth` | 25 | Both published digest vectors byte‑for‑byte, challenge/response both ways, wrong secret and unknown user rejected | The §6.7 timing defence, and the `0x12`/`0x13` framing, which is inferred rather than transcribed |
| `session_reset` | 23 | §6.11–6.12 in both directions, and — the point — that traffic still **flows** afterwards | Reset under load |

The row worth staring at is `session_loopback`, and the column is the last one. `session_loopback` is
the largest suite and the weakest evidence: it runs this library against itself, so any
misunderstanding of the spec that is applied consistently to both the sending and the
receiving side passes every check. Two real defects lived there until other people's
implementations disagreed with us — a NAK livelock (found by the Teensy) and an
unimplemented `0x11` (found by Tahoe, which sends one on every connection). Neither was
a coding mistake; both were us being wrong in a mirror, consistently, on both sides.

That is why the interop table above is the more meaningful of the two, and why
`conformance_vectors` matters out of proportion to its 25 checks: the spec's published
byte grids are the only fixed point in the repo that we did not write.

## Features

- **Faithful to M2‑124‑UM** — 4‑byte `MIDI` signature, 32‑bit command header,
  Invitation/Reply, Ping/Bye, NAK, UMP Data with sequence numbers, and the §7.2
  integrity machinery. Byte‑for‑byte conformance vectors from the spec's Appendix A.1.
- **Both roles** — act as the UDP *Host* (listen/accept) or *Client* (discover/invite).
- **Many clients, one port** — `HostPort` serves N sessions from a single UDP port,
  as §3.2 requires of a Host.
- **Robustness** — 64‑entry anti‑replay window, FEC send/receive (§7.2.2),
  Retransmit (§7.2.3–7.2.4), Session Reset (§6.11–6.12), idle declarations (§7.2.1).
- **Authentication** — shared‑secret and user/password (§6.7–6.10), with SHA‑256 and
  entropy injected through `ICrypto` so you can use CommonCrypto, mbedTLS, or a
  hardware engine. No crypto is implemented here.
- **Freestanding‑friendly** — no exceptions, no RTTI, no heap, no STL containers,
  no OS headers in the core. CI enforces it.
- **Injected I/O** — the protocol core is OS‑agnostic; you supply the socket, clock,
  and optionally mDNS and crypto.
- **Header‑only core** — drop the include dir into your build; no library to link.
- **Tested** — nine suites, 313 checks, plus the real‑peer bench above. What each
  suite does and does not prove is [tabulated](#tests-what-each-one-is-worth).

## Design: portable core + injected I/O

```
             ┌─────────────────────────────────────────────┐
             │  libnetmidi2 core  (portable, freestanding)  │
             │  Protocol.h  — wire format (build/parse)      │
             │  Session.h   — state machine, seq, ping, bye  │
             │  HostPort.h  — one port, many sessions (§3.2) │
             └───────────────┬──────────────────────────────┘
                             │ IUdpSocket · IClock · IDiscovery · ICrypto
              ┌──────────────┴───────────────┐
      Desktop (e.g. JUCE adapters)     Embedded (e.g. Zephyr adapters)
      DatagramSocket / Time /          BSD sockets / k_uptime /
      NetworkServiceDiscovery          mDNS responder
```

Because every platform compiles the **same** `Protocol.h` and `Session.h`, framing is
identical by construction — two peers built on this library cannot drift apart.

## Layout

```
include/netmidi2/
  Protocol.h   wire format: signature, command codes, big-endian build/parse
  Platform.h   injected I/O interfaces (IUdpSocket / IClock / IDiscovery / ICrypto)
  Session.h    session state machine, sequence numbers, ping/bye, FEC, retransmit
  HostPort.h   one UDP port serving many Clients (§3.2)
  Discovery.h  the mDNS/DNS-SD contract for _midi2._udp (§4) — no responder here
  Auth.h       the §6.7–6.10 digest construction — no crypto here
PROTOCOL.md    the wire contract — a readable profile of M2-124-UM
UPGRADING.md   what consumers must change, and what is not yet proven
tests/         nine suites: conformance vectors, protocol guards, loopback,
               multi-client, discovery, FEC, retransmit, auth, session reset
tools/         nm2_bench.cpp — interop harness for driving a real peer
               nm2_cli.cpp   — a complete endpoint: mDNS, multi-client host,
                               client, and an arpeggiator. Includes a working
                               Bonjour IDiscovery adapter.
```

## Integrate

It's a header‑only interface target. With CMake:

```cmake
add_subdirectory(libnetmidi2)            # or FetchContent / a submodule
target_link_libraries(your_app PRIVATE netmidi2)
```

Or add `libnetmidi2/include` to your include path and `#include <netmidi2/Session.h>`.

**Already using it?** Read [`UPGRADING.md`](UPGRADING.md) before you pull — it lists
the API breaks and, more importantly, the behaviour that changed without one.

## Usage

Implement the platform interfaces for your OS (`IUdpSocket` and `IClock` are required;
`IDiscovery` and `ICrypto` are optional), then drive a `Session`:

```cpp
#include <netmidi2/Session.h>
using namespace netmidi2;

struct MyListener : ISessionListener {
    void onUmpReceived (const uint32_t* words, uint8_t count) override {
        // hand `words` (host-order UMP) to your synth / router
    }
    void onStateChanged (State s) override { /* update your UI */ }

    // The host is asking a user whether to admit us (§6.6). This can take as long
    // as a person takes to answer a dialog -- don't show a short spinner.
    void onInvitationPending() override { /* "waiting for the other device..." */ }
};

MyUdpSocket socket;      // your IUdpSocket
MyClock     clock;       // your IClock
MyListener  listener;

Platform platform { &socket, &clock, nullptr };  // nullptr = no mDNS (explicit host:port)
Session   session (platform, Role::client, &listener, "My Endpoint", "MY-PRODUCT-1");

Endpoint peer {};                        // the host to invite
std::strcpy (peer.address, "203.0.113.50");
peer.port = 5004;
session.connect (peer);                  // Client: send an Invitation

for (;;) {                               // call frequently from your run loop
    session.tick();                      // drains the socket, runs ping/timeout

    if (session.state() == State::established) {
        uint32_t noteOn[2] = { 0x40903C00u, 0xFFFF0000u };  // MIDI 2.0 note-on C4
        session.sendUmp (noteOn, 2);
    }
    // sleep ~1 ms
}
```

A UDP *Host* is the same, but calls `session.listen()` instead of `connect()` and
learns its peer from the incoming Invitation. A Host that may face **more than one**
Client needs `HostPort` rather than a second `Session` on a second port: §3.2 requires
all Clients to be served from the one advertised port.

Two things worth knowing before you wire it in:

- **`close()` is not synchronous.** It enters the spec's Pending Bye state and repeats
  the Bye until the peer answers. Wait for `State::closed`.
- **`State` has seven values**, including `authenticating`, `resetting` and `closing`.
  A `switch` over it needs arms for those.

See `tests/session_loopback.cpp` for a complete, runnable example with POSIX adapters.

## Build & test

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure     # all nine suites
```

To build the interop harness and point it at a real device:

```bash
cmake -S . -B build -DNETMIDI2_BUILD_TOOLS=ON && cmake --build build
./build/nm2_bench browse                       # list _midi2._udp peers on the LAN
./build/nm2_bench client <host> <port> --probe # full session, prints every datagram
```

`--probe` sends a UMP Stream Endpoint Discovery: a legal UMP that a conformant
endpoint answers and that **cannot make a sound**, so it is safe to point at a live
rig. `--note` is the audible opt‑in.

The same option builds **`nm2_cli`**, a complete endpoint rather than a diagnostic —
it advertises over mDNS, accepts several clients on one port, dials out to another
device by name or address, and plays an arpeggiated chord down the session:

```bash
./build/nm2_cli --listen 5004 --name "My Synth" --pid "SYNTH-0001"
./build/nm2_cli --connect "My Synth" --chord C4:maj7 --bpm 96
./build/nm2_cli --listen --connect 203.0.113.50:5004      # both roles at once (§8)
```

It carries a working **Bonjour `IDiscovery` adapter** (~150 lines), which is the one
adapter this library deliberately doesn't ship. Worth reading if you're writing your
own for Avahi or Zephyr.

## Documentation

- **[`docs/libnetmidi2 tutorial.md`](docs/libnetmidi2%20tutorial.md)** — a step-by-step guide: why the problem
  exists, how to write the four adapters, how to deploy, and the half-dozen things
  that reliably catch people out. **Start here if you are integrating the library.**

## Protocol reference

- **[`PROTOCOL.md`](PROTOCOL.md)** — the wire contract this library implements: the
  signature, command header, full command‑code table, UMP Data framing, session
  lifecycle, discovery, and the §7.2 integrity mechanisms. Start here if you're
  implementing the other end.
- MIDI Association **M2‑124‑UM** "Network MIDI 2.0 (UDP)" v1.0 — the normative spec.

## Status

The transport is feature‑complete against M2‑124‑UM, and interop‑tested against
macOS Tahoe and a Zephyr peer.

| Area | Status |
|---|---|
| Wire format (`Protocol.h`), Appendix A.1 conformance vectors | ✅ |
| Session lifecycle, sequence dedup, Ping/Bye/NAK | ✅ |
| Both roles, bidirectional UMP | ✅ |
| Several Clients on one Host port (`HostPort`, §3.2) | ✅ |
| mDNS discovery contract (`_midi2._udp`, §4) | ✅ contract — the responder is an adapter |
| FEC send + receive, Retransmit, idle declarations (§7.2) | ✅ |
| Authentication, shared‑secret and user (§6.7–6.10) | ✅ — SHA‑256 injected via `ICrypto` |
| Session Reset (§6.11–6.12) | ✅ |
| Invitation Reply: Pending (`0x11`, §6.6) | ✅ Client side — we honour a Host that asks for time. Sending one is the app's call, so the Host side is left to you. |

`IDiscovery` and `ICrypto` have no implementations in this repo, by design: an mDNS
responder and a SHA‑256 are platform territory, and shipping one of each here would
make hardware accelerators unreachable. See `Discovery.h` and `Auth.h` for the
contracts, and `tests/` for working fakes.

## Contributing

Issues and pull requests welcome. Please keep the **core** (`Protocol.h`, `Session.h`)
freestanding — no exceptions/RTTI/heap/STL containers/OS headers — so it keeps
compiling on embedded targets. Platform‑specific code belongs in adapters, behind the
`Platform.h` interfaces. New wire behaviour should cite the M2‑124‑UM section it
implements and, where practical, add a test. If you change public API, update
`UPGRADING.md`.

## License

MIT — see [LICENSE](LICENSE).
