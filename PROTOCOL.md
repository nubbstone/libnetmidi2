# Network MIDI 2.0 (UDP) — wire contract

This is the **shared wire contract** implemented by both ends of the link.

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

An unknown Command Code → reply **NAK reason `0x01`** "Command Not Supported" (§5.5).
So does a code the spec defines but this implementation does not support — from a
sender's point of view those are the same thing. See §3.6.

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

> **Receivers must range-check `pl`.** Payload Length is a single byte, so a command
> can *claim* up to 255 words, while §7.1 Table 29 caps a UMP Data command at **64**
> ("the length shall not exceed 64 words"). Such a datagram is not malformed framing
> — 8 + 255×4 = 1028 bytes sits well inside the 1400-byte limit, so a length check on
> the datagram will not catch it. Anything over 64 words must be dropped. Enforcing
> this only when *sending* is not enough; in this library it was a remotely
> triggerable stack buffer overflow until `Session` checked it on receipt too
> (`kMaxUmpWordsPerCommand`, regression-tested as "oversized:").

- Only valid in the **Established** state; otherwise reply **Bye reason `0x05`**
  (Session not Established) to the sender — §7.1 makes this a *shall*, and it is the
  only thing that tells a peer still transmitting into a session we have forgotten
  (because we restarted) that it needs to re-invite. We answer any sender, since a
  sender we have no session with is precisely the one that needs telling; answering
  never refreshes our liveness timer or alters our state.
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
header:  code=0x10 | payloadLen=pl (2..36) | D1 = name length in words
                                             D2 = 0 (Reserved)
payload: Host's UMP Endpoint Name + Product Instance Id   (same encoding as 3.2)
```

Establishes the session. (Pending `0x11` / Auth `0x12`,`0x13` are Phase 2/3.)

Structurally identical to the Invitation (§3.2) — same csd1, same two padded strings
— differing only in the code and in csd2 being Reserved rather than Capabilities.
One builder emits both.

**On receipt, §6.5 gives two rules, and the order between them matters:**

1. **Already Established with that host → ignore it.** A host repeats its Accepted
   until it sees traffic (see §3.2 / the lost-acceptance recovery), so a duplicate
   arriving right after the session opened is routine, not an error.
2. **Otherwise, no pending invitation → Bye reason `0x06`** (No Pending Invitation).

Check (1) first. Reversed, a client answers its own host's retransmission with a Bye
and destroys the session it just opened — using the handshake's own recovery
mechanism to do it. That is strictly worse than the silence rule 2 replaces, and is
regression-tested (`dup-accept:`).

An Accepted from an endpoint we never invited **must not establish anything**,
however well-formed — otherwise any box on the LAN could hand us a session unasked.

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
Bye:       code=0xF0 | payloadLen=pl (0..255) | D1 = Bye Reason | D2 = 0 (Reserved)
           payload: optional UTF-8 Text Message (pl*4 bytes, null-padded, no BOM)
Bye Reply: code=0xF1 | payloadLen=0 | cmdSpecific=0
```

The Bye payload is a **Text Message**, not an echoed command header (§6.16 Table 26)
— that is NAK's payload, see §3.6. We send `pl = 0`.

Graceful close from either side; the peer answers Bye Reply, then both go Idle.
**Bye reason codes seen in the spec** (partial): `0x00` undefined, `0x04` Timeout,
`0x05` Session not Established, `0x06` No Pending Invitation, `0x40` reject (user),
`0x41` Rejected (no prior session), `0x43` Authentication failed, `0x45` No matching
Auth method, `0x80` Invitation Canceled.

**Always acknowledge a Bye.** §6.16: "Because the Bye Command might be repeated, the
Bye Reply shall also be sent if there is no Pending or Established Session." Send
exactly one Bye Reply per Bye received, **to the sender**. A sender we have no
session with is precisely the one that would otherwise retransmit until it times
out. Acknowledging is not accepting: only our own peer's Bye closes our session —
letting any sender close it was a real bug (see the `stranger:` tests).

### 3.6 NAK — `0x8F` (§6.15)

```
NAK: code=0x8F | payloadLen=pl (1..255) | D1 = NAK Reason | D2 = 0 (Reserved)
     payload: word 0 = header word of the offending command, copied verbatim
              words 1.. = optional UTF-8 Text Message ((pl-1)*4 bytes)
```

**D2 is Reserved and must be 0** (§6.15 Table 24) — *not* the offending command's
code, as an earlier version of this document claimed. The offending command is
identified by its whole 32-bit header echoed in the payload, so the minimum `pl` is
**1**. We send no Text Message, so we always send `pl = 1` (a 12-byte datagram).

NAK reasons (§6.15 Table 25):

| Value | Reason | Used for |
|---|---|---|
| `0x00` | Other | reason is in the Text Message |
| `0x01` | **Command Not Supported** | a Command Code we don't implement (§5.5) |
| `0x02` | Command Not Expected | supported, but not valid right now (e.g. an unsolicited Ping Reply) |
| `0x03` | Command Malformed | missing payload / unparseable values. *Not* to be sent merely because a payload is longer than expected |
| `0x20` | Bad Ping Reply | Ping Reply carried the wrong Ping Id |

NAK has no reply; send once. On **receiving** NAK `0x01`, do not send that command
again (§5.5).

**We send NAK `0x01`** for any Command Code we do not implement — which includes
spec-defined Phase 2/3 commands (auth `0x02`/`0x03`, retransmit `0x80`/`0x81`,
session reset `0x82`/`0x83`, the other Invitation Replies) as well as codes that
aren't in the spec at all. §5.5 requires an answer rather than silence.

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
agree.

**Done on this side:** `tests/conformance_vectors.cpp` checks all four A.1 vectors
in both directions (we must build exactly those bytes, and parse exactly those bytes
back):

| Vector | Figure | What it pins down |
|---|---|---|
| A.1.1 Invitation | 12 | csd1 = name length in words, csd2 = Capabilities; name gets its `0x00` terminator + padding, Product Instance Id already ends on a word boundary so it does not |
| A.1.2 UMP Data | 13 | one UMP (Timing Clock), Seq `0x0010` |
| A.1.3 UMP Data | 14 | **one** command carrying **two** UMPs, Seq `0x0011` |
| A.1.4 UMP Data ×2 | 15 | **two** commands in **one** datagram, Seq `0x3456`/`0x3457` — the FEC shape; catches a parser that stops after the first command |

Why this matters more than it looks: the loopback test runs our client against our
host, so a wire-format error is invisible to it — both ends make the same mistake
and it cancels out. Verified by fault injection: flipping `put32`/`get32` to
little-endian leaves `session_loopback` passing 24/24 while the conformance vectors
fail on byte 0.

No published vectors exist for Invitation Reply: Accepted, Ping/Ping Reply, Bye or
NAK, so those remain covered only by the loopback.
