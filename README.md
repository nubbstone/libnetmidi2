# libnetmidi2

A standalone **Network MIDI 2.0 (UDP)** library — the MIDI Association's
UMP-over-UDP transport (spec **M2-124-UM**). macOS has no such support (only legacy
RTP-MIDI 1.0), so this implements it.

It's deliberately a **library on its own**, not baked into any app. The M2 SoundGen
Host consumes it; the Teensy/Zephyr sister project can consume the *same* core.

## Design: portable core + injected I/O

The protocol logic knows nothing about the OS. All I/O is injected, so one codebase
runs everywhere:

```
             ┌─────────────────────────────────────────────┐
             │  libnetmidi2 core  (portable, freestanding)  │
             │  Protocol.h  — wire format (build/parse)      │
             │  Session     — state machine, seq, ping, bye  │
             └───────────────┬──────────────────────────────┘
                             │ IUdpSocket · IClock · IDiscovery
              ┌──────────────┴───────────────┐
      macOS host (JUCE adapters)      Teensy (Zephyr adapters)
      DatagramSocket / Time /         zsock_* / k_uptime /
      NetworkServiceDiscovery         Zephyr mDNS
```

Because both ends compile the **same** `Protocol.h` (and, ideally, the same
`Session`), framing is identical by construction — interop drift ≈ 0. The core is
written freestanding-friendly: **no exceptions, no RTTI, no heap, no STL containers,
no platform headers** — safe for Zephyr.

## Layout

```
include/netmidi2/
  Protocol.h   ✅ wire format: signature, command codes, big-endian build/parse
  Platform.h   ✅ injected I/O interfaces (IUdpSocket / IClock / IDiscovery)
  Session.h    ✅ session state machine (Idle→Inviting→Established), seq/ping/bye, both roles
PROTOCOL.md    ✅ the shared wire contract (faithful profile of M2-124-UM)
CMakeLists.txt ✅ target `netmidi2` + loopback test
tests/         ✅ session_loopback.cpp (Host+Client over real localhost UDP); ⏳ spec A.1 vectors
adapters/juce/ ⏳ JUCE implementations of the Platform interfaces (host side)
```

## How the host uses it

The host adds a thin `NetworkUmpFrontDoor` that parallels its existing CoreMIDI
front door:

- **receive:** `Session` decodes UMP Data → hands UMP words to the same
  `SourceTap → UmpToClapTranslator` path the USB/Gaia input uses → Surge. Identical
  downstream, so mapping / learn / everything works unchanged.
- **send:** local UMP (from the Gaia, apps, or the host's virtual endpoint) →
  `Session` frames it as UMP Data → out to the peer.

Nothing else in the host changes — network is just another UMP source/sink.

## Roadmap

| Phase | Deliverable | Status |
|---|---|---|
| N0 | Wire format (`Protocol.h`) + platform interfaces + contract doc | ✅ done |
| N1 | `Session` state machine: Invitation/Accepted, Ping/Bye, UMP Data (explicit host:port) | ✅ done + loopback test |
| N1 | JUCE platform adapters + host `NetworkUmpFrontDoor` + connect UI, **both directions** (recv→plugin, send local UMP→peer) | ✅ done (`--nettest`) |
| —  | Cross-device test against the real Teensy (titou lib) over the LAN | ⏳ next (needs Teensy IP:port + role) |
| N2 | mDNS discovery (`_midi2._udp`), FEC + retransmit robustness | — |
| N3 | Authentication (Invitation w/ Auth), multi-peer | — |

## References

- `PROTOCOL.md` — the wire contract both ends build to (start here).
- MIDI Association **M2-124-UM** Network MIDI 2.0 (UDP) v1.0, 2024-11-20.
