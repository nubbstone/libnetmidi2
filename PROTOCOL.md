# Network MIDI 2.0 (UDP) — wire contract

This is the **shared wire contract** implemented by both ends of the link:

- the **M2 SoundGen Host** (macOS, JUCE) — see `libnetmidi2` in this folder, and
- the **Teensy / Zephyr** sister project (bare-metal Ethernet).

It is a faithful profile of the MIDI Association specification
**M2-124-UM "Network MIDI 2.0 (UDP)" v1.0 (2024-11-20)**. Section numbers below
(§x.y) refer to that document. Where this profile deviates or defers, it says so
explicitly. If this doc and the MA spec ever disagree, **the MA spec wins** — file
a fix here.

The goal: identical framing/parsing on both ends so interop is guaranteed. All
multi-byte fields are **big-endian (network byte order), unsigned** (§5.3).

---

## 1. Transport & framing

- **UDP, peer-to-peer.** Host ↔ Client (roles in §4). No broadcast/multicast for data.
- **Max UDP payload 1400 bytes** (§5.1.1) — never fragment. Implementations must
  keep each datagram ≤ 1400 bytes.
- Each UDP datagram = a **4-byte Signature** followed by **one or more Command Packets**.

### 1.1 Signature (§5.2)

Every UDP datagram starts with the 32-bit word:

```
0x4D 0x49 0x44 0x49        // "MIDI" in ASCII  == 0x4D494449
```

A receiver **must** verify this first word; if it fails, ignore the whole datagram.

### 1.2 Command Packet header (§5.4)

Every command starts with a 32-bit header, immediately followed by its payload:

```
 byte 0          byte 1               byte 2      byte 3
+---------------+--------------------+-----------------------+
| Command Code  | Payload Length     | Command Specific Data |
| (1 byte)      | (1 byte, in words) | (2 bytes / 16-bit)    |
+---------------+--------------------+-----------------------+
| Command Payload:  <Payload Length> x 32-bit words ...      |
+-----------------------------------------------------------+
```

- **Command Code** (1 byte) — which command (table below).
- **Payload Length** (1 byte) — length of the payload **in 32-bit words** (0 = header only).
- **Command Specific Data** (2 bytes) — per-command; when unused, set to 0. Some
  commands split it into **Data 1** (high byte) and **Data 2** (low byte).
- **Command Payload** — `Payload Length × 4` bytes. Always a multiple of 4. Extra
  trailing words within the declared length must be ignored (forward-compat, §5.3).

Multiple command packets may be packed into one datagram after the single signature.

---

## 2. Command codes (§5.5, Table 8)

| Code   | Name                                | Cmd-Specific Data      | Payload                          | Dir  | Phase |
|--------|-------------------------------------|------------------------|----------------------------------|------|-------|
| `0xFF` | **UMP Data**                        | Sequence Number (16b)  | UMP words                        | both | 1     |
| `0x01` | **Invitation**                      | D1=name len, D2=caps   | Endpoint Name + Product Id       | C→H  | 1     |
| `0x02` | Invitation with Authentication      | —                      | sha256 digest                    | C→H  | 3     |
| `0x03` | Invitation with User Authentication | —                      | sha256 digest + username         | C→H  | 3     |
| `0x10` | **Invitation Reply: Accepted**      | —                      | Endpoint Name + Product Id       | H→C  | 1     |
| `0x11` | Invitation Reply: Pending           | —                      | Endpoint Name + Product Id       | H→C  | 2     |
| `0x12` | Invitation Reply: Auth Required     | —                      | Crypto-Nonce                     | H→C  | 3     |
| `0x13` | Invitation Reply: User Auth Req'd   | —                      | Crypto-Nonce                     | H→C  | 3     |
| `0x20` | **Ping**                            | 0 (reserved)           | Ping Id (32-bit)                 | both | 1     |
| `0x21` | **Ping Reply**                      | 0 (reserved)           | Ping Id (32-bit, echoed)         | both | 1     |
| `0x80` | Retransmit Request                  | requested Seq Number   | none                             | both | 2     |
| `0x81` | Retransmit Error                    | requested Seq Number   | none                             | both | 2     |
| `0x82` | Session Reset                       | 0                      | none                             | both | 2     |
| `0x83` | Session Reset Reply                 | 0                      | none                             | both | 2     |
| `0x8F` | **NAK**                             | D1=NAK reason, D2=code | echoed command header (+ opt)    | both | 1     |
| `0xF0` | **Bye**                             | D1=Bye reason          | optional echoed command          | both | 1     |
| `0xF1` | **Bye Reply**                       | 0                      | none                             | both | 1     |

An unknown Command Code → reply **NAK** "command not supported" (§5.5).

**Phase 1** (what both ends implement first): UMP Data, Invitation / Accepted,
Ping / Ping Reply, Bye / Bye Reply, NAK. No authentication, no retransmit
(dedup + FEC only). That's a complete, usable standard session.

---

## 3. The commands we implement in Phase 1

### 3.1 UMP Data — `0xFF` (§7.1)

```
header:  code=0xFF | payloadLen=pl (0..64) | cmdSpecific = Sequence Number (16-bit)
payload: pl x 32-bit words = zero or more WHOLE UMP messages (never split a UMP)
```

- Only valid in the **Established** state; otherwise reply Bye reason `0x05`.
- Sequence Number: 16-bit, per-sender per-session, starts `0x0000`, +1 per UMP Data
  command, wraps after `0xFFFF` (§5.6, §7.1). One seq number covers all UMPs in the
  command.
- A datagram may carry several UMP Data commands (used for FEC redundancy, §7.2.2).

### 3.2 Invitation — `0x01` (§6.4)  *(Client → Host)*

```
header:  code=0x01 | payloadLen=pl (2..36) | D1 = name length in words (csd1)
                                             D2 = Capabilities bitmap (csd2)
payload: [csd1 words]         UMP Endpoint Name  (UTF-8, ≤ 98 bytes, null-padded)
         [(pl-csd1) words]    Product Instance Id (ASCII, ≤ 42 bytes, null-padded)
```

- **Capabilities** bitmap (§6.4, Table 11): D0 = supports Invitation w/ Auth,
  D1 = supports Invitation w/ User Auth, D2..D7 reserved. **Phase 1 sends `0x00`.**
- Strings are UTF-8 (name) / ASCII (product id), no BOM, null-padded to the word
  boundary (§5.3).

### 3.3 Invitation Reply: Accepted — `0x10` (§6.5)  *(Host → Client)*

```
header:  code=0x10 | payloadLen=pl | D1 = name length in words | D2 = 0
payload: Host's UMP Endpoint Name + Product Instance Id   (same encoding as 3.2)
```

Establishes the session. (Pending `0x11` / Auth `0x12`,`0x13` are Phase 2/3.)

### 3.4 Ping / Ping Reply — `0x20` / `0x21` (§6.13–6.14)

```
Ping:       code=0x20 | payloadLen=1 | cmdSpecific=0 | payload: Ping Id (32-bit)
Ping Reply: code=0x21 | payloadLen=1 | cmdSpecific=0 | payload: Ping Id (echoed)
```

Either side may Ping at any time/state. The receiver echoes the Ping Id in a Reply.
Used for keepalive + stale detection: if pings go unanswered for too long, send
**Bye reason `0x04` (Timeout)**.

### 3.5 Bye / Bye Reply — `0xF0` / `0xF1` (§6.16–6.17)

```
Bye:       code=0xF0 | payloadLen=n | D1 = Bye reason | payload: optional echoed cmd
Bye Reply: code=0xF1 | payloadLen=0 | cmdSpecific=0
```

Graceful close from either side; the peer answers Bye Reply, then both go Idle.
**Bye reason codes seen in the spec** (partial): `0x00` undefined, `0x04` Timeout,
`0x05` Session not Established, `0x06` No Pending Invitation, `0x40` reject (user),
`0x41` Rejected (no prior session), `0x43` Authentication failed, `0x45` No matching
Auth method, `0x80` Invitation Canceled.

### 3.6 NAK — `0x8F` (§6.15)

```
NAK: code=0x8F | payloadLen=n | D1 = NAK reason | D2 = offending Command Code
     payload: the echoed header of the offending command (+ optional text)
```

NAK reasons include `0x02` "Command Not Expected" and "command not supported".
NAK has no reply; send once.

---

## 4. Sessions & discovery

### 4.1 Session state machine (§6.1)

```
Idle ──(send/recv Invitation)──▶ Pending ──(Invitation Reply: Accepted)──▶ Established
  ▲                                                                            │
  └──────────────────── Bye / Bye Reply / Timeout ────────────────────────────┘
```

- Only in **Established** may either side send **UMP Data**.
- Most session commands are **repeated until acknowledged** (§7.2 General
  Considerations); NAK is sent once.
- Ping keepalive runs throughout Established; repeated unanswered pings → Bye `0x04`.

### 4.2 Roles

The spec's **UDP Host** listens/advertises and accepts Invitations; the **UDP Client**
discovers and sends the Invitation (§2.1–2.2, §4). Either physical device can play
either role.

**This project's convention (Phase 1):** the **M2 SoundGen Host is the UDP Host**
(advertises via mDNS, binds a UDP port, accepts Invitations); the **Teensy is the
UDP Client** (discovers the host, sends the Invitation). Both then send UMP Data
bidirectionally once Established. *(Open to revisiting — see §6.)*

### 4.3 Ports (§3)

There is **no fixed UDP port.** The Host binds a port of its choosing and advertises
it in the mDNS SRV record; the Client learns it via discovery. For bring-up before
mDNS exists, allow an **explicit `host:port`** override on both ends.

### 4.4 Discovery — mDNS / DNS-SD (§4)

- Service type **`_midi2._udp`**.
- **PTR** record → service instance name.
- **SRV** record → hostname + UDP port.
- **TXT** record keys: `UMPEndpointName`, `ProductInstanceId`.
- **A / AAAA** → IP address. Respect TTLs (§4.6).

---

## 5. Data-integrity (§7.2) — phased

- **Dedup:** ignore a UMP Data command whose Sequence Number was already processed.
- **Order:** track last seq; reorder/accept per §7.2.
- **FEC (Phase 2):** repeat recent UMP Data commands within later datagrams for
  redundancy (§7.2.2).
- **Retransmit (Phase 2):** `0x80` Retransmit Request / `0x81` Retransmit Error
  (§7.2.3–7.2.4).

Phase 1: dedup + in-order acceptance; drop-and-continue on gaps (fine on a quiet LAN).

---

## 6. Open decisions to confirm with the sister project

1. **Role assignment** — Mac = Host, Teensy = Client (proposed in §4.2). OK?
2. **Shared code vs two implementations** — can the Teensy/Zephyr build consume the
   portable C++ core (`libnetmidi2`), or will it implement this contract in Zephyr-C?
   Shared code = one source of truth = near-zero interop drift.
3. **Endpoint identity** — the `UMPEndpointName` / `ProductInstanceId` strings each
   end advertises.
4. **Bring-up transport** — explicit `host:port` first (skip mDNS), add discovery after
   the UMP round-trip works?

---

## 7. Conformance vectors

The MA spec's **Appendix A.1** gives example byte-for-byte UDP packets. Both ends
should include a unit test that builds each Phase-1 command and asserts the exact
bytes against those vectors — that's the cheapest guarantee the two implementations
agree. (To be transcribed here from the spec PDF.)
