# libnetmidi2

A small, portable, dependency‑free **C++17 implementation of Network MIDI 2.0
(UDP)** — the MIDI Association's UMP‑over‑UDP transport, spec **M2‑124‑UM**.

![CI](https://github.com/nubbstone/libnetmidi2/actions/workflows/ci.yml/badge.svg)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

Send and receive **MIDI 2.0 Universal MIDI Packets over Ethernet/Wi‑Fi** with full
32‑bit resolution and per‑note expression — no operating‑system support required.

## Why

Most platforms still ship **no** Network MIDI 2.0 transport. Apple's CoreMIDI, for
example, only offers legacy **RTP‑MIDI (MIDI 1.0)**, which collapses everything to
7‑bit. `libnetmidi2` implements the real thing — the MIDI Association's Network
MIDI 2.0 (UDP) spec — in a form small enough to run on a desktop app *and* on a
microcontroller.

## Features

- **Faithful to M2‑124‑UM** — 4‑byte `MIDI` signature, 32‑bit command header,
  Invitation/Reply, Ping/Bye, NAK, UMP Data with sequence numbers. Interops with
  any other spec‑compliant implementation.
- **Both roles** — act as the UDP *Host* (listen/accept) or *Client* (discover/invite).
- **Bidirectional** — send and receive UMP once a session is Established.
- **Freestanding‑friendly** — no exceptions, no RTTI, no heap, no STL containers,
  no OS headers in the core. Safe for Zephyr / bare‑metal as well as desktop.
- **Injected I/O** — the protocol core is OS‑agnostic; you supply the socket,
  clock, and (optionally) mDNS. One codebase runs everywhere.
- **Header‑only core** — drop the include dir into your build; no library to link.
- **Tested** — a real‑UDP loopback test drives the full handshake + bidirectional
  UMP + graceful close.

## Design: portable core + injected I/O

```
             ┌─────────────────────────────────────────────┐
             │  libnetmidi2 core  (portable, freestanding)  │
             │  Protocol.h  — wire format (build/parse)      │
             │  Session.h   — state machine, seq, ping, bye  │
             └───────────────┬──────────────────────────────┘
                             │ IUdpSocket · IClock · IDiscovery
              ┌──────────────┴───────────────┐
      Desktop (e.g. JUCE adapters)     Embedded (e.g. Zephyr adapters)
      DatagramSocket / Time /          BSD sockets / k_uptime /
      NetworkServiceDiscovery          mDNS responder
```

Because every platform compiles the **same** `Protocol.h` and `Session.h`, framing
is identical by construction — two peers built on this library interoperate with
zero drift.

## Layout

```
include/netmidi2/
  Protocol.h   wire format: signature, command codes, big-endian build/parse
  Platform.h   injected I/O interfaces (IUdpSocket / IClock / IDiscovery)
  Session.h    session state machine (Idle→Inviting→Established), seq/ping/bye
PROTOCOL.md    the wire contract — a readable profile of M2-124-UM
tests/         session_loopback.cpp — Host+Client over real localhost UDP
CMakeLists.txt target `netmidi2` + the loopback test
```

## Integrate

It's a header‑only interface target. With CMake:

```cmake
add_subdirectory(libnetmidi2)            # or FetchContent / a submodule
target_link_libraries(your_app PRIVATE netmidi2)
```

Or just add `libnetmidi2/include` to your include path and `#include <netmidi2/Session.h>`.

## Usage

Implement the three platform interfaces for your OS (`IUdpSocket`, `IClock`, and —
optionally, for mDNS discovery — `IDiscovery`), then drive a `Session`:

```cpp
#include <netmidi2/Session.h>
using namespace netmidi2;

struct MyListener : ISessionListener {
    void onUmpReceived (const uint32_t* words, uint8_t count) override {
        // hand `words` (host-order UMP) to your synth / router
    }
    void onStateChanged (State s) override { /* update your UI */ }
};

MyUdpSocket socket;      // your IUdpSocket
MyClock     clock;       // your IClock
MyListener  listener;

Platform platform { &socket, &clock, nullptr };  // nullptr = no mDNS (explicit host:port)
Session   session (platform, Role::client, &listener, "My Endpoint", "MY-PRODUCT-1");

Endpoint peer {};                        // the host to invite
std::strcpy (peer.address, "192.168.1.50");
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
learns its peer from the incoming Invitation.

See `tests/session_loopback.cpp` for a complete, runnable example (with POSIX socket
and clock adapters) that stands up a Host and a Client and exchanges UMP.

## Build & test

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure     # runs the UDP loopback test
```

## Protocol reference

- **[`PROTOCOL.md`](PROTOCOL.md)** — the wire contract this library implements: the
  signature, command header, full command‑code table, UMP Data framing, session
  lifecycle, and mDNS discovery. Start here if you're implementing the other end.
- MIDI Association **M2‑124‑UM** "Network MIDI 2.0 (UDP)" v1.0 — the normative spec.

## Status

Phase 1 of the spec is implemented and interop‑tested: **Invitation / Reply,
Ping / Bye, NAK, and bidirectional UMP Data** over an explicit `host:port`.

| Area | Status |
|---|---|
| Wire format (`Protocol.h`) | ✅ |
| Session (Invitation → Established, Ping/Bye, sequence dedup) | ✅ |
| Both roles (Host / Client), bidirectional UMP | ✅ + loopback test |
| mDNS discovery (`_midi2._udp`) | planned |
| FEC + retransmit robustness | planned |
| Authentication (Invitation with Auth) | planned |

## Contributing

Issues and pull requests welcome. Please keep the **core** (`Protocol.h`,
`Session.h`) freestanding — no exceptions/RTTI/heap/STL containers/OS headers — so
it keeps compiling on embedded targets. Platform‑specific code belongs in adapters,
behind the `Platform.h` interfaces. New wire behaviour should cite the M2‑124‑UM
section it implements and, where practical, add a test.

## License

MIT — see [LICENSE](LICENSE).
