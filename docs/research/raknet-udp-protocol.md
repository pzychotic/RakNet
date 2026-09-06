# RakNet UDP Wire Protocol

Reference description of the network protocol RakNet layers on top of UDP: datagram framing, message
framing, reliability, ordering, splitting, acknowledgement and the connection handshake.

**Sources.** Everything below was derived by reading this tree's source at commit `9708b41`. Where a
claim is non-obvious it cites `path:line`. Plugin protocols (NAT punchthrough, FileListTransfer,
ReplicaManager, RPC4, autopatcher, lobby/cloud, etc.) and the object-replication layers are
deliberately **out of scope** — only `RakPeer` + `ReliabilityLayer` + `BitStream` traffic is described.
The doxygen material in `Help/` and `3.x_to_4.x_upgrade.txt` was skimmed for orientation only; no claim
here rests on it.

**Compile-time configuration assumed.** This document describes the default build of *this* tree. The
switches that change the wire format are called out in
[Build switches that change the wire format](#build-switches-that-change-the-wire-format).

---

## 1. Layering overview

```
+-------------------------------------------------------------+
| IP header (20) + UDP header (8)   -- "UDP_HEADER_SIZE" = 28 |
+-------------------------------------------------------------+
| RakNet datagram                                             |
|  +-------------------------------------------------------+  |
|  | datagram header (1 or 4 bytes, see 4)                 |  |
|  +-------------------------------------------------------+  |
|  | message 0: internal-packet header + payload           |  |
|  | message 1: internal-packet header + payload           |  |
|  | ...                                                   |  |
|  +-------------------------------------------------------+  |
+-------------------------------------------------------------+
```

* One UDP datagram carries exactly one RakNet datagram. There is no framing above UDP other than the
  datagram header; the receive buffer is `MAXIMUM_MTU_SIZE` bytes
  ([RakNetSocket2.h:48](Source/RakNetSocket2.h:48), [RakNetSocket2_Berkley.cpp:454](Source/RakNetSocket2_Berkley.cpp:454)).
* A RakNet datagram is one of four shapes, discriminated by the first bits of byte 0:
  1. **Offline / connectionless message** — `isValid` bit clear (see §2).
  2. **ACK datagram** — carries only a range list of acked datagram numbers.
  3. **NAK datagram** — carries only a range list of missing datagram numbers.
  4. **Data datagram** — carries a datagram sequence number followed by 1..N *internal packets*.
* Each internal packet ("message") is a reliability header plus the user payload. The payload's first
  byte is the `ID_*` message identifier ([MessageIdentifiers.h](Source/MessageIdentifiers.h)).
* ACK/NAK datagrams never carry messages; data datagrams never carry ACK/NAK ranges. They are separate
  UDP datagrams ([ReliabilityLayer.cpp:1493](Source/ReliabilityLayer.cpp:1493),
  [ReliabilityLayer.cpp:3077](Source/ReliabilityLayer.cpp:3077)).
* Coalescing: `ReliabilityLayer::Update` packs as many queued messages as fit under the MTU into a
  single datagram before emitting it ([ReliabilityLayer.cpp:1662](Source/ReliabilityLayer.cpp:1662)).

### Dispatch on receive

`ProcessNetworkPacket` first offers the datagram to `ProcessOfflineNetworkPacket`
([RakPeer.cpp:4842](Source/RakPeer.cpp:4842)). Only if that returns false and the sender is a known
connected system is the datagram handed to
`ReliabilityLayer::HandleSocketReceiveFromConnectedPlayer` ([RakPeer.cpp:4855](Source/RakPeer.cpp:4855)).

---

## 2. Offline (connectionless) messages

### 2.1 The magic

```c
static const unsigned char OFFLINE_MESSAGE_DATA_ID[16] = {
    0x00,0xFF,0xFF,0x00, 0xFE,0xFE,0xFE,0xFE,
    0xFD,0xFD,0xFD,0xFD, 0x12,0x34,0x56,0x78 };
```
[RakPeer.cpp:123](Source/RakPeer.cpp:123)

An offline message is recognised by (a) a known `ID_*` byte at offset 0 and (b) the 16 magic bytes at
the offset appropriate for that ID ([RakPeer.cpp:3982](Source/RakPeer.cpp:3982)–4024). Note the magic
is at a **different offset per message type** — see the tables below.

The comment at [RakPeer.cpp:121](Source/RakPeer.cpp:121) explains the key invariant: the first byte of
every offline message must have its **high bit clear**, so that a `ReliabilityLayer` parse of the same
bytes would see `isValid == false` and discard it. All offline `ID_*` values are `< 0x80`, so this
holds.

Any datagram of `length <= 2` is treated as an offline message and ignored
([RakPeer.cpp:3982](Source/RakPeer.cpp:3982)); the reliability layer also drops `length <= 2`
([ReliabilityLayer.cpp:522](Source/ReliabilityLayer.cpp:522)).

### 2.2 Relevant `ID_*` values

The enum starts at 0 with no explicit values ([MessageIdentifiers.h:47](Source/MessageIdentifiers.h:47)):

| Value | Name | Role |
|---|---|---|
| 0  | `ID_CONNECTED_PING` | connected ping |
| 1  | `ID_UNCONNECTED_PING` | offline |
| 2  | `ID_UNCONNECTED_PING_OPEN_CONNECTIONS` | offline |
| 3  | `ID_CONNECTED_PONG` | connected pong |
| 4  | `ID_DETECT_LOST_CONNECTIONS` | received-and-ignored only (never sent by this tree) |
| 5  | `ID_OPEN_CONNECTION_REQUEST_1` | offline handshake |
| 6  | `ID_OPEN_CONNECTION_REPLY_1` | offline handshake |
| 7  | `ID_OPEN_CONNECTION_REQUEST_2` | offline handshake |
| 8  | `ID_OPEN_CONNECTION_REPLY_2` | offline handshake |
| 9  | `ID_CONNECTION_REQUEST` | first *connected* (reliable) message |
| 10 | `ID_REMOTE_SYSTEM_REQUIRES_PUBLIC_KEY` | security only |
| 11 | `ID_OUR_SYSTEM_REQUIRES_SECURITY` | security only |
| 12 | `ID_PUBLIC_KEY_MISMATCH` | security only |
| 13 | `ID_OUT_OF_BAND_INTERNAL` | offline |
| 14 | `ID_SND_RECEIPT_ACKED` | locally generated only |
| 15 | `ID_SND_RECEIPT_LOSS` | locally generated only |
| 16 | `ID_CONNECTION_REQUEST_ACCEPTED` | connected |
| 17 | `ID_CONNECTION_ATTEMPT_FAILED` | offline reply / local |
| 18 | `ID_ALREADY_CONNECTED` | offline reply |
| 19 | `ID_NEW_INCOMING_CONNECTION` | connected |
| 20 | `ID_NO_FREE_INCOMING_CONNECTIONS` | offline reply |
| 21 | `ID_DISCONNECTION_NOTIFICATION` | connected |
| 22 | `ID_CONNECTION_LOST` | locally generated only |
| 23 | `ID_CONNECTION_BANNED` | offline reply |
| 24 | `ID_INVALID_PASSWORD` | connected reply |
| 25 | `ID_INCOMPATIBLE_PROTOCOL_VERSION` | offline reply |
| 26 | `ID_IP_RECENTLY_CONNECTED` | offline reply |
| 27 | `ID_TIMESTAMP` | user message prefix |
| 28 | `ID_UNCONNECTED_PONG` | offline reply |
| 29 | `ID_ADVERTISE_SYSTEM` | offline (via OOB) |
| 30 | `ID_DOWNLOAD_PROGRESS` | locally generated |
| 134 | `ID_USER_PACKET_ENUM` | first user ID (`0x86`) |

Values 31..133 are plugin identifiers and are out of scope.

### 2.3 `ID_UNCONNECTED_PING` / `ID_UNCONNECTED_PING_OPEN_CONNECTIONS`

Built by `RakPeer::Ping(host, port, ...)` ([RakPeer.cpp:1716](Source/RakPeer.cpp:1716)):

| Offset | Size | Field | Encoding |
|---|---|---|---|
| 0 | 1 | `ID_UNCONNECTED_PING` (1) or `..._OPEN_CONNECTIONS` (2) | byte |
| 1 | 8 | `RakNet::Time` send time | big-endian uint64 |
| 9 | 16 | `OFFLINE_MESSAGE_DATA_ID` | raw |
| 25 | 8 | sender `RakNetGUID` | big-endian uint64 |

Total 33 bytes. The magic-detection check only requires `length >= 1 + 8 + 16`
([RakPeer.cpp:3988](Source/RakPeer.cpp:3988)), so the GUID is optional from the parser's point of view
(it reads into `UNASSIGNED_RAKNET_GUID` if absent, [RakPeer.cpp:4044](Source/RakPeer.cpp:4044)).

`ID_UNCONNECTED_PING_OPEN_CONNECTIONS` is only answered when `AllowIncomingConnections()`
([RakPeer.cpp:4036](Source/RakPeer.cpp:4036)).

### 2.4 `ID_UNCONNECTED_PONG`

Reply built at [RakPeer.cpp:4047](Source/RakPeer.cpp:4047):

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | `ID_UNCONNECTED_PONG` (28) |
| 1 | 8 | the ping's `RakNet::Time`, echoed verbatim |
| 9 | 8 | responder `RakNetGUID` |
| 17 | 16 | `OFFLINE_MESSAGE_DATA_ID` |
| 33 | n | `offlinePingResponse` blob set by `SetOfflinePingResponse()` |

Note the magic here sits **after** the GUID, unlike the ping.

The packet delivered to the *local* application is rewritten
([RakPeer.cpp:4079](Source/RakPeer.cpp:4079)–4100): `ID_UNCONNECTED_PONG`, then a 4-byte
`RakNet::TimeMS` (truncated from the echoed 8-byte time), then the response blob. The magic and GUID are
stripped; the GUID is exposed via `Packet::guid`.

### 2.5 `ID_OPEN_CONNECTION_REQUEST_1` (client -> server)

Built in `RakPeer::RunUpdateCycle` ([RakPeer.cpp:5074](Source/RakPeer.cpp:5074)):

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | `ID_OPEN_CONNECTION_REQUEST_1` (5) |
| 1 | 16 | `OFFLINE_MESSAGE_DATA_ID` |
| 17 | 1 | `RAKNET_PROTOCOL_VERSION` = **6** ([RakNetVersion.h:21](Source/RakNetVersion.h:21)) |
| 18 | pad | zero padding to `mtuSizes[i] - UDP_HEADER_SIZE` **total** bytes |

The datagram is sent with `IP_DONTFRAGMENT` set for the duration of the send
([RakPeer.cpp:5099](Source/RakPeer.cpp:5099)–5137), so it doubles as a path-MTU probe. If `sendto`
returns `10040` (`WSAEMSGSIZE`) the client jumps straight to the next smaller MTU
([RakPeer.cpp:5110](Source/RakPeer.cpp:5110)); if the send blocked for more than 100 ms it drops to the
smallest MTU ([RakPeer.cpp:5118](Source/RakPeer.cpp:5118)).

### 2.6 `ID_OPEN_CONNECTION_REPLY_1` (server -> client)

Built at [RakPeer.cpp:4546](Source/RakPeer.cpp:4546):

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | `ID_OPEN_CONNECTION_REPLY_1` (6) |
| 1 | 16 | `OFFLINE_MESSAGE_DATA_ID` |
| 17 | 8 | server `RakNetGUID` (big-endian uint64) |
| 25 | 1 | `hasCookie` — always `0` in this build (`LIBCAT_SECURITY == 0`) |
| — | 4 | cookie (only when `hasCookie`) |
| — | 64 | server public key (only when `hasCookie`) |
| 26 | 2 | negotiated `mtu` (big-endian uint16) |
| 28 | pad | zeros to `mtu - 28` total payload bytes |

`mtu = min(MAXIMUM_MTU_SIZE, receivedLength + UDP_HEADER_SIZE)`
([RakPeer.cpp:4566](Source/RakPeer.cpp:4566)). Because the header written so far is exactly 28 bytes,
`PadWithZeroToByteLength(mtu - GetNumberOfBytesUsed())` produces a UDP payload of `mtu - 28`
bytes — i.e. an IP datagram of exactly `mtu` bytes, which probes the MTU in the reverse direction. The
reply is also sent with `IP_DONTFRAGMENT` ([RakPeer.cpp:4580](Source/RakPeer.cpp:4580)).

If the protocol byte does not equal `RAKNET_PROTOCOL_VERSION`, the server instead replies
([RakPeer.cpp:4519](Source/RakPeer.cpp:4519)):

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | `ID_INCOMPATIBLE_PROTOCOL_VERSION` (25) |
| 1 | 1 | the server's own `RAKNET_PROTOCOL_VERSION` |
| 2 | 16 | `OFFLINE_MESSAGE_DATA_ID` |
| 18 | 8 | server `RakNetGUID` |

(26 bytes; the receiver requires exactly this length,
[RakPeer.cpp:4017](Source/RakPeer.cpp:4017).)

### 2.7 `ID_OPEN_CONNECTION_REQUEST_2` (client -> server)

Built at [RakPeer.cpp:4155](Source/RakPeer.cpp:4155)–4247 in response to reply 1:

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | `ID_OPEN_CONNECTION_REQUEST_2` (7) |
| 1 | 16 | `OFFLINE_MESSAGE_DATA_ID` |
| — | 4 | cookie echo (only if the server said `hasCookie`) |
| — | 1 | `clientWroteChallenge` byte, then 64-byte challenge (only if `hasCookie`) — **`0` in this build** ([RakPeer.cpp:4215](Source/RakPeer.cpp:4215)) |
| 17 | 7 | `SystemAddress` the client used to reach the server ("binding address"), IPv4 encoding |
| 24 | 2 | `mtu` echoed from reply 1 (big-endian uint16) |
| 26 | 8 | client `RakNetGUID` |

Total 34 bytes with security off. No padding — this datagram is not an MTU probe.

### 2.8 `ID_OPEN_CONNECTION_REPLY_2` (server -> client)

Built at [RakPeer.cpp:4691](Source/RakPeer.cpp:4691):

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | `ID_OPEN_CONNECTION_REPLY_2` (8) |
| 1 | 16 | `OFFLINE_MESSAGE_DATA_ID` |
| 17 | 8 | server `RakNetGUID` |
| 25 | 7 | the client's `SystemAddress` as the server sees it |
| 32 | 2 | `mtu` echoed (big-endian uint16) |
| 34 | **1 bit** | `requiresSecurityOfThisClient` — a `bool`, so a single bit, **not** a byte |
| — | 64 | handshake answer (only when security is on) |

`bsAnswer.Write(requiresSecurityOfThisClient)` writes a `bool`, i.e. one bit
([BitStream.h:895](Source/BitStream.h:895)), so the datagram is 34 bytes + 1 bit = **35 bytes** of UDP
payload with the last 7 bits zero-padded. The reader mirrors this with `bs.Read(bool)`
([RakPeer.cpp:4284](Source/RakPeer.cpp:4284)).

Failure replies sent instead of reply 2, all with the same 25-byte shape
`ID(1) | magic(16) | serverGUID(8)`:

| Condition | Reply | Site |
|---|---|---|
| IP is banned (checked before anything else) | `ID_CONNECTION_BANNED` | [RakPeer.cpp:3960](Source/RakPeer.cpp:3960) |
| GUID or address already in use | `ID_ALREADY_CONNECTED` | [RakPeer.cpp:4726](Source/RakPeer.cpp:4726) |
| No free slots | `ID_NO_FREE_INCOMING_CONNECTIONS` | [RakPeer.cpp:4745](Source/RakPeer.cpp:4745) |
| Same IP connected < 100 ms ago | `ID_IP_RECENTLY_CONNECTED` | [RakPeer.cpp:4767](Source/RakPeer.cpp:4767) |

If the request-2 is an exact duplicate of one already handled (packet loss on reply 2), the server
simply re-sends the identical reply 2 ([RakPeer.cpp:4699](Source/RakPeer.cpp:4699)).

### 2.9 `ID_CONNECTION_REQUEST` (client -> server, **reliable, connected**)

From here on the connection is established at the reliability-layer level: the client allocates a
`RemoteSystemStruct` in state `UNVERIFIED_SENDER`, then sends this as a normal reliable message
(`IMMEDIATE_PRIORITY, RELIABLE`) — i.e. wrapped in a datagram header and an internal-packet header
([RakPeer.cpp:4396](Source/RakPeer.cpp:4396)–4424):

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | `ID_CONNECTION_REQUEST` (9) |
| 1 | 8 | client `RakNetGUID` |
| 9 | 8 | `RakNet::GetTime()` (big-endian uint64) |
| 17 | 1 | `doSecurity` byte — `0` in this build |
| — | 32 | proof, then 1 byte `doIdentity` (+ identity) when security is on |
| 18 | n | outgoing password bytes, raw (may be empty) |

The password is whatever remains of the message ([RakPeer.cpp:3060](Source/RakPeer.cpp:3060)). Total
message length is limited to 512 bytes by convention (`MAX_OFFLINE_DATA_LENGTH = 400`,
[RakPeer.cpp:119](Source/RakPeer.cpp:119)).

On mismatch the server replies with a reliable `ID_INVALID_PASSWORD | serverGUID(8)`
([RakPeer.cpp:3067](Source/RakPeer.cpp:3067)) and flags the connection `DISCONNECT_ASAP_SILENTLY`.

A system in state `UNVERIFIED_SENDER` that sends anything other than `ID_CONNECTION_REQUEST` is closed
and temporarily banned for `timeoutTime` ms ([RakPeer.cpp:5269](Source/RakPeer.cpp:5269)–5276).

### 2.10 `ID_CONNECTION_REQUEST_ACCEPTED` (server -> client, reliable-ordered)

Built by `RakPeer::OnConnectionRequest` ([RakPeer.cpp:3081](Source/RakPeer.cpp:3081)), sent
`IMMEDIATE_PRIORITY, RELIABLE_ORDERED` on channel 0:

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | `ID_CONNECTION_REQUEST_ACCEPTED` (16) |
| 1 | 7 | the client's `SystemAddress` as the server sees it (its external address) |
| 8 | 2 | `SystemIndex` (big-endian uint16) |
| 10 | 70 | 10 × `SystemAddress` = the server's local IP list (`MAXIMUM_NUMBER_OF_INTERNAL_IDS` = 10) |
| 80 | 8 | `incomingTimestamp` echoed from `ID_CONNECTION_REQUEST` |
| 88 | 8 | `RakNet::GetTime()` on the server |

Total 96 bytes. The client requires `byteSize > 25` ([RakPeer.cpp:5447](Source/RakPeer.cpp:5447)) and
uses the last two times as a ping/pong pair ([RakPeer.cpp:5481](Source/RakPeer.cpp:5481)).

### 2.11 `ID_NEW_INCOMING_CONNECTION` (client -> server, reliable-ordered)

Sent by the client immediately on accepting ([RakPeer.cpp:5518](Source/RakPeer.cpp:5518)):

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | `ID_NEW_INCOMING_CONNECTION` (19) |
| 1 | 7 | the server's `SystemAddress` as the client sees it |
| 8 | 70 | 10 × `SystemAddress` — the client's local IP list |
| 78 | 8 | `sendPongTime` (the server's clock value from the accept) |
| 86 | 8 | `RakNet::GetTime()` on the client |

Total 94 bytes. Receiving it moves the server from `HANDLING_CONNECTION_REQUEST` to `CONNECTED`
([RakPeer.cpp:5312](Source/RakPeer.cpp:5312)).

### 2.12 `ID_OUT_OF_BAND_INTERNAL` / `ID_ADVERTISE_SYSTEM`

`WriteOutOfBandHeader` ([RakPeer.cpp:2543](Source/RakPeer.cpp:2543)):

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | `ID_OUT_OF_BAND_INTERNAL` (13) |
| 1 | 8 | sender `RakNetGUID` |
| 9 | 16 | `OFFLINE_MESSAGE_DATA_ID` |
| 25 | n | user data (`n < MAX_OFFLINE_DATA_LENGTH = 400`) |

If the first user byte is `ID_ADVERTISE_SYSTEM`, the receiver rewrites the local packet to
`ID_ADVERTISE_SYSTEM` + the remaining bytes ([RakPeer.cpp:4115](Source/RakPeer.cpp:4115)).

### 2.13 Handshake sequence and retry schedule

```
client                                        server
  |  ID_OPEN_CONNECTION_REQUEST_1 (padded, DF)   |
  |--------------------------------------------->|   protocol check
  |  ID_OPEN_CONNECTION_REPLY_1 (padded, DF)     |
  |<---------------------------------------------|   (or ID_INCOMPATIBLE_PROTOCOL_VERSION)
  |  ID_OPEN_CONNECTION_REQUEST_2                |
  |--------------------------------------------->|   slot allocated, state UNVERIFIED_SENDER
  |  ID_OPEN_CONNECTION_REPLY_2                  |
  |<---------------------------------------------|   (or ALREADY_CONNECTED / NO_FREE / IP_RECENTLY)
  |= = = = reliability layer active both ways = =|
  |  ID_CONNECTION_REQUEST  (RELIABLE)           |
  |--------------------------------------------->|   password check -> HANDLING_CONNECTION_REQUEST
  |  ID_CONNECTION_REQUEST_ACCEPTED (REL_ORDERED)|
  |<---------------------------------------------|   (or ID_INVALID_PASSWORD)
  |  ID_NEW_INCOMING_CONNECTION (REL_ORDERED)    |
  |--------------------------------------------->|   server -> CONNECTED
  |  ID_CONNECTED_PING / ID_CONNECTED_PONG       |
```

`Connect()` defaults are `sendConnectionAttemptCount = 12` and
`timeBetweenSendConnectionAttemptsMS = 500` ([RakPeerInterface.h:131](Source/RakPeerInterface.h:131)).
The MTU index used for attempt *n* is `n / (sendConnectionAttemptCount / NUM_MTU_SIZES)`
([RakPeer.cpp:5068](Source/RakPeer.cpp:5068)) — with the defaults that is 4 attempts at each of
1492, 1200 and 576. After all attempts, `ID_CONNECTION_ATTEMPT_FAILED` is generated locally
([RakPeer.cpp:5052](Source/RakPeer.cpp:5052)).

A half-open connection (`REQUESTED_CONNECTION`, `HANDLING_CONNECTION_REQUEST`, `UNVERIFIED_SENDER`)
that has not reached `CONNECTED` within 10 000 ms is dropped
([RakPeer.cpp:5194](Source/RakPeer.cpp:5194)–5198).

---

## 3. Bit-packing conventions (`BitStream`)

Everything RakNet serialises goes through `BitStream`. The rules matter because most of the protocol is
bit-, not byte-, aligned.

### 3.1 Bit order

Bits are written **MSB-first within each byte**. `Write1` sets `0x80 >> (bitsUsed & 7)`
([BitStream.cpp:290](Source/BitStream.cpp:290)); `Write0` just advances, zeroing the byte when it is
new ([BitStream.cpp:278](Source/BitStream.cpp:278)). `ReadBit` mirrors this
([BitStream.cpp:305](Source/BitStream.cpp:305)).

`WriteBits(src, n, rightAlignedBits=true)` writes `n` bits taken from `src`; for a trailing partial byte
the source bits are taken from the **low** end of the byte and shifted into the **high** end of the
stream byte ([BitStream.cpp:423](Source/BitStream.cpp:423)).

`AlignWriteToByteBoundary()` / `AlignReadToByteBoundary()` simply round the offset up to the next
multiple of 8 ([BitStream.h:571](Source/BitStream.h:571)); the skipped bits are not explicitly zeroed,
though in practice they are 0 because a freshly started byte is zeroed by `Write0`/`Write1`.

### 3.2 Endianness

`__BITSTREAM_NATIVE_END` is **not defined** in this tree
([RakNetDefines.h:33](Source/RakNetDefines.h:33) — the `#define` is commented out) and nothing in
`CMakeLists.txt` defines it. Therefore:

* `DoEndianSwap()` returns `IsNetworkOrder() == false` ([BitStream.h:642](Source/BitStream.h:642)),
  and `IsNetworkOrderInternal()` tests `htonl(12345) == 12345`
  ([BitStream.cpp:1002](Source/BitStream.cpp:1002)). On x86/ARM-LE this is **true**, so all
  multi-byte values written via the generic `Write<T>` are **byte-reversed into big-endian (network)
  order** ([BitStream.h:852](Source/BitStream.h:852)).
* `WriteAlignedVar16` / `WriteAlignedVar32` likewise byte-swap on little-endian hosts
  ([BitStream.cpp:1043](Source/BitStream.cpp:1043), [BitStream.cpp:1083](Source/BitStream.cpp:1083)) —
  **big-endian on the wire**.
* **`uint24_t` is the exception.** `Write(const uint24_t&)` branches on `IsBigEndian()` rather than
  `DoEndianSwap()` and copies the three least-significant native bytes in ascending significance
  ([BitStream.h:930](Source/BitStream.h:930)). The result is **little-endian on the wire on every
  host**. `Read(uint24_t&)` matches ([BitStream.h:1225](Source/BitStream.h:1225)). Both also
  byte-align first.

  This matters: `DatagramSequenceNumberType`, `MessageNumberType`, `OrderingIndexType` are all
  `uint24_t`, so datagram numbers, reliable message numbers, ordering and sequencing indices are
  **little-endian 3-byte fields** sitting next to big-endian 2- and 4-byte fields.
* `WriteAlignedVar8` is a plain byte copy ([BitStream.cpp:1026](Source/BitStream.cpp:1026)).

### 3.3 Compressed writes

`WriteCompressed(const unsigned char*, size, unsignedData)`
([BitStream.cpp:462](Source/BitStream.cpp:462)) writes, from most-significant byte down: a `1` bit for
each byte equal to `byteMatch` (`0x00` unsigned / `0xFF` signed); on the first non-matching byte a `0`
bit followed by all remaining bytes. If all upper bytes matched, a final bit says whether the low
byte's high nibble also matches, and then 4 or 8 bits are written. `ReadCompressed` is the inverse
([BitStream.cpp:589](Source/BitStream.cpp:589)).

Note `WriteCompressed<T>` for multi-byte `T` byte-swaps first
([BitStream.h:1022](Source/BitStream.h:1022)), and there are specialisations that are *not* compressed
at all: `SystemAddress`, `RakNetGUID`, `uint24_t` and `bool` all fall back to plain `Write`
([BitStream.h:1035](Source/BitStream.h:1035)–1057). `WriteCompressed<float>` maps `[-1,1]` onto a
`uint16` ([BitStream.h:1061](Source/BitStream.h:1061)).

**No compressed write is used anywhere in the core protocol described here** — only
`WriteAlignedBytesSafe`/`ReadAlignedBytesSafe` use it, and those are not part of the datagram or
message headers.

### 3.4 Wire encodings of the standard types

| Type | Bytes | Encoding | Site |
|---|---|---|---|
| `bool` | 1 **bit** | 1 = true | BitStream.h:895 |
| `unsigned char` / `MessageID` | 1 | as-is | BitStream.h:852 |
| `uint16_t`, `SystemIndex` | 2 | big-endian | BitStream.h:852 |
| `uint32_t`, `float` | 4 | big-endian | BitStream.h:852 |
| `RakNet::Time` (`uint64_t`, because `__GET_TIME_64BIT == 1`) | 8 | big-endian | RakNetDefines.h:22, RakNetTime.h:22 |
| `RakNet::TimeMS` | 4 | big-endian | RakNetTime.h:23 |
| `RakNetGUID` | 8 | big-endian `uint64` | BitStream.h:952 |
| `uint24_t` | 3 | **little-endian**, byte-aligned | BitStream.h:930 |
| `SystemAddress` (IPv4) | 7 | see below | BitStream.h:907 |

`SystemAddress` IPv4 encoding ([BitStream.h:907](Source/BitStream.h:907)):

```
byte 0      : IP version = 4
bytes 1..4  : ~s_addr, raw memory, NOT byte-swapped (so network byte order, bitwise inverted)
bytes 5..6  : sin_port, raw memory (network byte order)
```

The bitwise inversion of the address is deliberate ("Hide the address so routers don't modify it").
`SystemAddress::size()` returns `4 + 2 + 1 = 7` for the non-IPv6 build
([RakNetTypes.cpp:185](Source/RakNetTypes.cpp:185)). `RAKNET_SUPPORT_IPV6` is 0 by default
([RakNetDefines.h:107](Source/RakNetDefines.h:107)); with it on, the encoding is the version byte plus a
raw `sockaddr_in6`.

### 3.5 `ID_TIMESTAMP`

A user message beginning with `ID_TIMESTAMP` (27) followed by a `RakNet::Time` has that time rewritten
on receipt by subtracting the estimated clock differential
([RakPeer.cpp:1191](Source/RakPeer.cpp:1191), `ShiftIncomingTimestamp`
[RakPeer.cpp:3273](Source/RakPeer.cpp:3273)). The differential is derived from
`ID_CONNECTED_PING`/`PONG` exchanges ([RakPeer.cpp:3846](Source/RakPeer.cpp:3846)). The bytes on the
wire are unmodified; only the local copy handed to the application is shifted.

---

## 4. Datagram header

Defined by `struct DatagramHeaderFormat` ([ReliabilityLayer.cpp:84](Source/ReliabilityLayer.cpp:84)),
serialised at [ReliabilityLayer.cpp:117](Source/ReliabilityLayer.cpp:117) and parsed at
[ReliabilityLayer.cpp:159](Source/ReliabilityLayer.cpp:159).

In this build `INCLUDE_TIMESTAMP_WITH_DATAGRAMS == 0`
([ReliabilityLayer.h:33](Source/ReliabilityLayer.h:33)–39), so **there is no timestamp field**.

### 4.1 Data datagram

```
bit:   7     6     5     4     3     2     1     0
     +-----+-----+-----+-----+-----+-----+-----+-----+
byte0| V=1 | A=0 | N=0 | PP  | CS  | NB  |  pad(2)   |
     +-----+-----+-----+-----+-----+-----+-----+-----+
byte1|                                               |
byte2|   datagramNumber : uint24, LITTLE-endian      |
byte3|                                               |
     +-----------------------------------------------+
byte4.. : internal packet 0, internal packet 1, ...
```

| Bit | Field | Meaning |
|---|---|---|
| `V`  | `isValid` | always written as 1; a 0 here means "this is an offline message, discard" ([ReliabilityLayer.cpp:549](Source/ReliabilityLayer.cpp:549)) |
| `A`  | `isACK` | 0 |
| `N`  | `isNAK` | 0 |
| `PP` | `isPacketPair` | always 0 in this build — packet pairing is disabled ([ReliabilityLayer.cpp:2958](Source/ReliabilityLayer.cpp:2958), `countdownToNextPacketPair` initialised to 15 and never decremented, [ReliabilityLayer.cpp:329](Source/ReliabilityLayer.cpp:329)) |
| `CS` | `isContinuousSend` | set from `bandwidthExceededStatistic` (data was still queued after the previous flush), and forced true for every datagram after the first in one `Update` ([ReliabilityLayer.cpp:1507](Source/ReliabilityLayer.cpp:1507), [ReliabilityLayer.cpp:1789](Source/ReliabilityLayer.cpp:1789)) |
| `NB` | `needsBAndAs` | `congestionManager.GetIsInSlowStart()` ([ReliabilityLayer.cpp:1506](Source/ReliabilityLayer.cpp:1506)); with the sliding-window controller `IsInSlowStart()` is `cwnd <= ssThresh \|\| ssThresh == 0` and `ssThresh` starts at 0, so this is normally 1 ([CCRakNetSlidingWindow.cpp:364](Source/CCRakNetSlidingWindow.cpp:364)) |

`datagramNumber` comes from `GetAndIncrementNextDatagramSequenceNumber()`
([CCRakNetSlidingWindow.cpp:107](Source/CCRakNetSlidingWindow.cpp:107)) — a monotonically increasing
24-bit counter starting at 0, incremented once per emitted data datagram. ACK and NAK datagrams do
**not** consume a sequence number.

Header size: **4 bytes**.

### 4.2 ACK datagram

```
bit:   7     6     5     4  3  2  1  0
     +-----+-----+-----+------------------+
byte0| V=1 | A=1 | BAS |     pad(5)       |
     +-----+-----+-----+------------------+
byte1.. : [float AS, 4 bytes big-endian]  -- only if BAS == 1
        : range list (see 4.4)
```

`BAS` = `hasBAndAS`. With the default sliding-window controller
`OnSendAckGetBAndAS` always reports `false` ([CCRakNetSlidingWindow.cpp:262](Source/CCRakNetSlidingWindow.cpp:262)),
so **`AS` is never present in a default build**. (With `CCRakNetUDT` it is the receiver's measured data
arrival rate in bytes/µs, sent at most once per SYN, [CCRakNetUDT.cpp:546](Source/CCRakNetUDT.cpp:546).)
`hasBAndAS` is also gated on the peer having asked for it via the `needsBAndAs` bit
(`remoteSystemNeedsBAndAS`, [ReliabilityLayer.cpp:735](Source/ReliabilityLayer.cpp:735),
[ReliabilityLayer.cpp:3095](Source/ReliabilityLayer.cpp:3095)).

The serialiser explicitly aligns to a byte boundary after the 3 header bits
([ReliabilityLayer.cpp:129](Source/ReliabilityLayer.cpp:129)).

### 4.3 NAK datagram

```
bit:   7     6     5     4  3  2  1  0
     +-----+-----+-----+------------------+
byte0| V=1 | A=0 | N=1 |     pad(5)       |
     +-----+-----+-----+------------------+
byte1.. : range list (see 4.4)
```

The NAK branch of `Serialize` writes only 3 bits and does **not** align
([ReliabilityLayer.cpp:139](Source/ReliabilityLayer.cpp:139)); the alignment comes from
`RangeList::Serialize`, which calls `AlignWriteToByteBoundary()` before writing the count
([DS_RangeList.h:96](Source/DS_RangeList.h:96)). The read path is symmetric
([DS_RangeList.h:121](Source/DS_RangeList.h:121)), so the encoding is consistent.

### 4.4 ACK/NAK range-list encoding

`DataStructures::RangeList<uint24_t>::Serialize` ([DS_RangeList.h:67](Source/DS_RangeList.h:67)):

```
align to byte boundary
uint16  count                 (big-endian) -- number of ranges that follow
repeat count times:
   uint8   minEqualsMax       (1 => single value; 0 => a real range)
   uint24  minIndex           (little-endian, byte-aligned)
   uint24  maxIndex           (little-endian) -- present ONLY if minEqualsMax == 0
```

The `minEqualsMax` flag deliberately uses a whole byte rather than a bit ("for speed, as this is done a
lot", [DS_RangeList.h:85](Source/DS_RangeList.h:85)). A single acked datagram therefore costs 4 bytes;
a contiguous run of any length costs 7.

`Insert()` coalesces adjacent values into ranges ([DS_RangeList.h:161](Source/DS_RangeList.h:161)), so
the list is a sorted set of disjoint, non-adjacent ranges.

The serialiser stops adding ranges when
`16 + bitsWritten + sizeof(range_type)*8*2 + 1 > maxBits` ([DS_RangeList.h:78](Source/DS_RangeList.h:78)).
`sizeof(uint24_t)` is 4 (it wraps a `uint32_t`), so the budget check reserves 64 bits per range where
only 32 or 56 are used — the packing is conservative but safe. `maxBits` is
`GetMaxDatagramSizeExcludingMessageHeaderBits()`.

`SendACKs` loops until the pending-ack list is empty, emitting one datagram per iteration
([ReliabilityLayer.cpp:3081](Source/ReliabilityLayer.cpp:3081)); `clearSerialized = true` removes the
ranges that were written.

### 4.5 Reserved header size vs. actual

`DatagramHeaderFormat::GetDataHeaderByteLength()` returns
`2 + 3 + sizeof(float) == 9` in this build ([ReliabilityLayer.cpp:107](Source/ReliabilityLayer.cpp:107)),
even though a data datagram header is only 4 bytes. This value is used solely to compute the maximum
payload, so RakNet under-fills each datagram by 5 bytes. Reimplementers should replicate the *budget*
(9) if they want byte-identical packing behaviour, but only 4 bytes are actually emitted.

---

## 5. Internal packet (message) header

Written by `ReliabilityLayer::WriteToBitStreamFromInternalPacket`
([ReliabilityLayer.cpp:2194](Source/ReliabilityLayer.cpp:2194)); read by
`CreateInternalPacketFromBitStream` ([ReliabilityLayer.cpp:2269](Source/ReliabilityLayer.cpp:2269)).

### 5.1 Field order

```
align to byte boundary
+---------------------------------------------------------------+
| bits 7..5 : reliability (3 bits)                              |
| bit  4    : hasSplitPacket                                    |
| bits 3..0 : padding (align)                                   |
+---------------------------------------------------------------+
| uint16    : dataBitLength           BIG-endian, byte-aligned  |
+---------------------------------------------------------------+
| uint24    : reliableMessageNumber   LITTLE-endian   [R]       |
+---------------------------------------------------------------+
| uint24    : sequencingIndex         LITTLE-endian   [S]       |
+---------------------------------------------------------------+
| uint24    : orderingIndex           LITTLE-endian   [O]       |
| uint8     : orderingChannel                         [O]       |
+---------------------------------------------------------------+
| uint32    : splitPacketCount        BIG-endian      [X]       |
| uint16    : splitPacketId           BIG-endian      [X]       |
| uint32    : splitPacketIndex        BIG-endian      [X]       |
+---------------------------------------------------------------+
| payload   : BITS_TO_BYTES(dataBitLength) bytes, byte-aligned  |
+---------------------------------------------------------------+
```

Presence rules:

* **[R]** present iff reliability ∈ {`RELIABLE`, `RELIABLE_SEQUENCED`, `RELIABLE_ORDERED`,
  `RELIABLE_WITH_ACK_RECEIPT`, `RELIABLE_ORDERED_WITH_ACK_RECEIPT`}
  ([ReliabilityLayer.cpp:2221](Source/ReliabilityLayer.cpp:2221)).
* **[S]** present iff reliability ∈ {`UNRELIABLE_SEQUENCED`, `RELIABLE_SEQUENCED`}
  ([ReliabilityLayer.cpp:2229](Source/ReliabilityLayer.cpp:2229)).
* **[O]** present iff reliability ∈ {`UNRELIABLE_SEQUENCED`, `RELIABLE_SEQUENCED`, `RELIABLE_ORDERED`,
  `RELIABLE_ORDERED_WITH_ACK_RECEIPT`} ([ReliabilityLayer.cpp:2235](Source/ReliabilityLayer.cpp:2235)).
* **[X]** present iff `hasSplitPacket` ([ReliabilityLayer.cpp:2245](Source/ReliabilityLayer.cpp:2245)).

There are `AlignWriteToByteBoundary()` calls after the length field's implicit alignment and after
`reliableMessageNumber` ([ReliabilityLayer.cpp:2227](Source/ReliabilityLayer.cpp:2227)) — both no-ops in
practice since every field is a whole number of bytes and `uint24_t`/`WriteAlignedVarN` self-align. The
net effect is that **every internal packet is byte-aligned and a whole number of bytes long**, which is
what makes multiple messages concatenate cleanly in one datagram.

### 5.2 Ack-receipt reliabilities are not transmitted

The three bits carry a *downgraded* value ([ReliabilityLayer.cpp:2203](Source/ReliabilityLayer.cpp:2203)):

| Local reliability | Value on the wire |
|---|---|
| `UNRELIABLE_WITH_ACK_RECEIPT` (5) | `UNRELIABLE` (0) |
| `RELIABLE_WITH_ACK_RECEIPT` (6) | `RELIABLE` (2) |
| `RELIABLE_ORDERED_WITH_ACK_RECEIPT` (7) | `RELIABLE_ORDERED` (3) |

so only values 0..4 ever appear in the 3-bit field. The receiver's reader mirrors this — it never
expects the `_WITH_ACK_RECEIPT` variants ([ReliabilityLayer.cpp:2298](Source/ReliabilityLayer.cpp:2298)).
Ack receipts are a purely local sender-side notification.

### 5.3 Header sizes

`GetMessageHeaderLengthBits` ([ReliabilityLayer.cpp:2142](Source/ReliabilityLayer.cpp:2142)) gives, in
bytes:

| Reliability | Base | + split | Total (split) |
|---|---|---|---|
| `UNRELIABLE` (0) | 3 | 10 | 13 |
| `UNRELIABLE_SEQUENCED` (1) | 10 | 10 | 20 |
| `RELIABLE` (2) | 6 | 10 | 16 |
| `RELIABLE_ORDERED` (3) | 10 | 10 | 20 |
| `RELIABLE_SEQUENCED` (4) | 13 | 10 | **23** |
| `UNRELIABLE_WITH_ACK_RECEIPT` (5) | 3 | 10 | 13 |
| `RELIABLE_WITH_ACK_RECEIPT` (6) | 6 | 10 | 16 |
| `RELIABLE_ORDERED_WITH_ACK_RECEIPT` (7) | 10 | 10 | 20 |

`GetMaxMessageHeaderLengthBits()` returns the worst case — `RELIABLE_SEQUENCED` with a split header,
23 bytes / 184 bits ([ReliabilityLayer.cpp:2134](Source/ReliabilityLayer.cpp:2134)).

### 5.4 Parser validation

`CreateInternalPacketFromBitStream` refuses the message (and thereby stops parsing the datagram) if
([ReliabilityLayer.cpp:2347](Source/ReliabilityLayer.cpp:2347)):

* a read failed, or
* `dataBitLength == 0`, or
* `reliability >= NUMBER_OF_RELIABILITIES` (8), or
* `orderingChannel >= 32`, or
* `hasSplitPacket && splitPacketIndex >= splitPacketCount`.

Parsing of the datagram stops when fewer than 32 unread bits remain
([ReliabilityLayer.cpp:2277](Source/ReliabilityLayer.cpp:2277)) — this is what lets a padded datagram
(e.g. the second of a packet pair) terminate cleanly.

`dataBitLength` is a `uint16`, so a single message's payload cannot exceed 65 535 bits
([ReliabilityLayer.cpp:2217](Source/ReliabilityLayer.cpp:2217)); in practice it is bounded by the MTU
because larger messages are split.

---

## 6. Reliability types

`enum PacketReliability` ([PacketPriority.h:45](Source/PacketPriority.h:45)):

| # | Name | Duplicate suppression | Retransmit | Ordering | Extra header |
|---|---|---|---|---|---|
| 0 | `UNRELIABLE` | no | no | none | — |
| 1 | `UNRELIABLE_SEQUENCED` | via sequencing index | no | late messages discarded | seq + ord + channel |
| 2 | `RELIABLE` | yes (message number) | yes | none | msg# |
| 3 | `RELIABLE_ORDERED` | yes | yes | strict, buffered | msg# + ord + channel |
| 4 | `RELIABLE_SEQUENCED` | yes | yes | late messages discarded | msg# + seq + ord + channel |
| 5 | `UNRELIABLE_WITH_ACK_RECEIPT` | no | no | none | — (wire = 0) |
| 6 | `RELIABLE_WITH_ACK_RECEIPT` | yes | yes | none | msg# (wire = 2) |
| 7 | `RELIABLE_ORDERED_WITH_ACK_RECEIPT` | yes | yes | strict | msg# + ord + channel (wire = 3) |

`NUMBER_OF_RELIABILITIES` = 8. The commented-out `..._SEQUENCED_WITH_ACK_RECEIPT` entries explain why
sequenced + receipt is unsupported ([PacketPriority.h:68](Source/PacketPriority.h:68),
[ReliabilityLayer.cpp:1703](Source/ReliabilityLayer.cpp:1703)).

Notes:

* `Send()` clamps out-of-range reliabilities to `RELIABLE`
  ([ReliabilityLayer.cpp:1258](Source/ReliabilityLayer.cpp:1258)).
* Splitting force-upgrades unreliable types: `UNRELIABLE` -> `RELIABLE`,
  `UNRELIABLE_WITH_ACK_RECEIPT` -> `RELIABLE_WITH_ACK_RECEIPT`,
  `UNRELIABLE_SEQUENCED` -> `RELIABLE_SEQUENCED`
  ([ReliabilityLayer.cpp:1310](Source/ReliabilityLayer.cpp:1310)).
* Only reliable messages get a `reliableMessageNumber` assigned, and only at the moment they are
  actually placed into a datagram ("late assignment",
  [ReliabilityLayer.cpp:1712](Source/ReliabilityLayer.cpp:1712)). `sendReliableMessageNumberIndex`
  increments per reliable message. Unreliable messages carry no message number and the receiver sets
  it to `0x00FFFFFF` ([ReliabilityLayer.cpp:2309](Source/ReliabilityLayer.cpp:2309)).

### 6.1 Ordering and sequencing

There are `NUMBER_OF_ORDERED_STREAMS = 32` channels ([ReliabilityLayer.h:46](Source/ReliabilityLayer.h:46)),
though `orderingChannel` is transmitted as a full byte.

The blending algorithm is documented in the source at
[ReliabilityLayer.h:434](Source/ReliabilityLayer.h:434) and implemented at
[ReliabilityLayer.cpp:1326](Source/ReliabilityLayer.cpp:1326) (send) and
[ReliabilityLayer.cpp:949](Source/ReliabilityLayer.cpp:949) (receive):

* Sender: each ordered message takes `orderingIndex = orderedWriteIndex[ch]++` and resets
  `sequencedWriteIndex[ch] = 0`. Each sequenced message takes the *current* `orderedWriteIndex[ch]`
  plus `sequencingIndex = sequencedWriteIndex[ch]++`.
* Receiver keeps `orderedReadIndex[ch]` and `highestSequencedReadIndex[ch]`.
  * `orderingIndex == orderedReadIndex` and sequenced: deliver if
    `sequencingIndex >= highestSequencedReadIndex`, then set
    `highestSequencedReadIndex = sequencingIndex + 1`; otherwise discard.
  * `orderingIndex == orderedReadIndex` and ordered: deliver immediately, increment
    `orderedReadIndex`, reset `highestSequencedReadIndex`, then drain the per-channel heap while its
    top matches the new `orderedReadIndex`.
  * `orderingIndex` newer: buffer in a min-heap keyed on
    `(orderingIndex - heapIndexOffset) * 1048576 + (sequencingIndex | 1048575)`
    ([ReliabilityLayer.cpp:1145](Source/ReliabilityLayer.cpp:1145)) so that, within an ordering index,
    sequenced messages come out before the ordered one.
  * `orderingIndex` older (`IsOlderOrderedPacket`, [ReliabilityLayer.cpp:2422](Source/ReliabilityLayer.cpp:2422)):
    discard.

The heap is a `std::priority_queue<WeightedPacket, std::vector<WeightedPacket>, WeightedPacket>` in
this tree ([ReliabilityLayer.h:419](Source/ReliabilityLayer.h:419)), replacing the old
`DataStructures::Heap`; behaviour is unchanged.

### 6.2 Reliable duplicate suppression

`hasReceivedPacketQueue` is a `std::deque<bool>` of "hole" flags relative to
`receivedPacketsBaseIndex` ([ReliabilityLayer.h:463](Source/ReliabilityLayer.h:463)–472). On each
reliable message ([ReliabilityLayer.cpp:800](Source/ReliabilityLayer.cpp:800)):

* `holeCount = reliableMessageNumber - receivedPacketsBaseIndex` (24-bit wrapping subtraction).
* `holeCount == 0` -> expected: pop front, `++receivedPacketsBaseIndex`.
* `holeCount > 0x7FFFFF` (more than half the 24-bit range) -> duplicate/old: discard.
* `holeCount < queue.size()` -> fills a hole if the flag is set, otherwise duplicate: discard.
* otherwise -> push `true` (hole) entries up to `holeCount`, then `false`. A `holeCount > 1 000 000`
  is rejected as an attack/corruption ([ReliabilityLayer.cpp:868](Source/ReliabilityLayer.cpp:868)).
* Finally pop all leading non-holes.

---

## 7. Priorities, queueing and the send tick

### 7.1 Priorities

`enum PacketPriority` ([PacketPriority.h:20](Source/PacketPriority.h:20)):
`IMMEDIATE_PRIORITY` (0), `HIGH_PRIORITY` (1), `MEDIUM_PRIORITY` (2), `LOW_PRIORITY` (3),
`NUMBER_OF_PRIORITIES` = 4.

Priority is **not transmitted**; it only orders the local outgoing queue.

The queue is a min-heap on a weight computed by `GetNextWeight(priorityLevel)`
([ReliabilityLayer.cpp:3376](Source/ReliabilityLayer.cpp:3376)) with initial weights
`(1 << p) * p + p` ([ReliabilityLayer.cpp:3370](Source/ReliabilityLayer.cpp:3370)) — 0, 3, 10, 27. Each
enqueue advances that priority's next weight by `(1 << p) * (p + 1) + p`, which produces roughly the
documented 2:1 ratio between adjacent priority levels.

### 7.2 The send tick

`RakPeer::RunUpdateCycle` runs on its own thread and waits up to 10 ms per iteration
([RakPeer.cpp:5619](Source/RakPeer.cpp:5619)); `quitAndDataEvents.SetEvent()` wakes it early for pings.
Per connection it calls `ReliabilityLayer::Update`
([RakPeer.cpp:5188](Source/RakPeer.cpp:5188)).

`ReliabilityLayer::Update` ([ReliabilityLayer.cpp:1368](Source/ReliabilityLayer.cpp:1368)) does, in
order:

1. Cull unreliable messages older than `unreliableTimeout` from the send queue
   ([ReliabilityLayer.cpp:1426](Source/ReliabilityLayer.cpp:1426)). `RakPeer` sets this to **1000 ms**
   by default ([RakPeer.cpp:220](Source/RakPeer.cpp:220)).
2. If anything is in the resend buffer and `AckTimeout()` fires, mark the connection dead
   ([ReliabilityLayer.cpp:1477](Source/ReliabilityLayer.cpp:1477)).
3. `SendACKs()` if `congestionManager.ShouldSendACKs()`.
4. Send one NAK datagram if `NAKs` is non-empty ([ReliabilityLayer.cpp:1493](Source/ReliabilityLayer.cpp:1493)).
5. Generate `ID_SND_RECEIPT_LOSS` for expired unreliable-with-receipt entries
   ([ReliabilityLayer.cpp:1533](Source/ReliabilityLayer.cpp:1533)).
6. Fill datagrams from the **resend list** first, up to `GetRetransmissionBandwidth()`
   ([ReliabilityLayer.cpp:1573](Source/ReliabilityLayer.cpp:1573)).
7. Fill datagrams from the **outgoing heap**, up to `GetTransmissionBandwidth()`
   ([ReliabilityLayer.cpp:1652](Source/ReliabilityLayer.cpp:1652)), stopping if the resend buffer
   would overflow (`RESEND_BUFFER_ARRAY_LENGTH` = 512,
   [RakNetDefines.h:90](Source/RakNetDefines.h:90)).
8. Serialise and send each accumulated datagram
   ([ReliabilityLayer.cpp:1787](Source/ReliabilityLayer.cpp:1787)–1861).

A message is added to the current datagram while
`datagramSizeSoFar + headerBits + dataBits <= GetMaxDatagramSizeExcludingMessageHeaderBits()`
([ReliabilityLayer.cpp:1681](Source/ReliabilityLayer.cpp:1681)); when it does not fit, the datagram is
closed but the loop continues, so a smaller lower-priority message can still slip in.

If `bitsPerSecondLimit` (`RakPeer::SetPerConnectionOutgoingBandwidthLimit`) is set, the outgoing loop
stops once the measured `USER_MESSAGE_BYTES_SENT` rate exceeds it
([ReliabilityLayer.cpp:1660](Source/ReliabilityLayer.cpp:1660)).

---

## 8. Split packets

### 8.1 When splitting happens

In `ReliabilityLayer::Send` ([ReliabilityLayer.cpp:1305](Source/ReliabilityLayer.cpp:1305)):

```
maxDataSizeBytes = GetMaxDatagramSizeExcludingMessageHeaderBytes()
                 - BITS_TO_BYTES(GetMaxMessageHeaderLengthBits())
splitPacket = numberOfBytesToSend > maxDataSizeBytes
```

with ([ReliabilityLayer.cpp:3353](Source/ReliabilityLayer.cpp:3353))

```
GetMaxDatagramSizeExcludingMessageHeaderBytes()
    = congestionManager.GetMTU() - DatagramHeaderFormat::GetDataHeaderByteLength()
    = (MTUSize - UDP_HEADER_SIZE) - 9
```

(`congestionManager.GetMTU()` returns what `Init()` was given, which is `MTUSize - 28`,
[ReliabilityLayer.cpp:282](Source/ReliabilityLayer.cpp:282); the member name
`MAXIMUM_MTU_INCLUDING_UDP_HEADER` is misleading.)

Worked example for `MTUSize = 1492`:

```
payload budget   = 1492 - 28 - 9        = 1455 bytes
max message hdr  = 23 bytes (RELIABLE_SEQUENCED + split)
maxDataSizeBytes = 1455 - 23            = 1432 bytes
```

For the other probe sizes: 1200 -> 1140, 576 -> 516.

### 8.2 Splitting

`ReliabilityLayer::SplitPacket` ([ReliabilityLayer.cpp:2448](Source/ReliabilityLayer.cpp:2448)):

* `maximumSendBlockBytes` is recomputed the same way, so each chunk carries at most that many bytes.
* `splitPacketCount = (dataByteLength - 1) / maximumSendBlockBytes + 1`
  ([ReliabilityLayer.cpp:2460](Source/ReliabilityLayer.cpp:2460)).
* `splitPacketId` is a per-connection `uint16` incremented once per split message and allowed to wrap
  ([ReliabilityLayer.cpp:2517](Source/ReliabilityLayer.cpp:2517)).
* `splitPacketIndex` runs 0..count-1. Each chunk inherits the parent's reliability, priority,
  ordering channel, ordering index and sequencing index; each gets its own reliable message number when
  it is actually sent.
* The chunks share one refcounted data block rather than being copied
  ([ReliabilityLayer.cpp:2502](Source/ReliabilityLayer.cpp:2502)).

Because `splitPacketCount` and `splitPacketIndex` are transmitted as full `uint32`s and
`splitPacketId` as a `uint16`, the theoretical limits are 2^32 chunks and 65 536 concurrent split
messages per connection; the practical limit is memory.

### 8.3 Reassembly

`InsertIntoSplitPacketList` ([ReliabilityLayer.cpp:2546](Source/ReliabilityLayer.cpp:2546)) looks up (or
creates) a `SplitPacketChannel` keyed on `splitPacketId` and preallocates an array of
`splitPacketCount` slots on first arrival ([ReliabilityLayer.h:81](Source/ReliabilityLayer.h:81)).
Duplicate indices within a channel are dropped ([ReliabilityLayer.h:93](Source/ReliabilityLayer.h:93),
[ReliabilityLayer.cpp:2648](Source/ReliabilityLayer.cpp:2648)).

When all slots are filled, `BuildPacketFromSplitPacketList`
([ReliabilityLayer.cpp:2739](Source/ReliabilityLayer.cpp:2739)) **first flushes ACKs immediately**
("for large files this can take a long time") and then concatenates the chunk payloads in index order
([ReliabilityLayer.cpp:2696](Source/ReliabilityLayer.cpp:2696)). Only then does ordering/sequencing
processing run on the reassembled message.

Non-ordered, non-sequenced split messages have their `orderingChannel` forced to 255 before insertion
([ReliabilityLayer.cpp:923](Source/ReliabilityLayer.cpp:923)) — this is a local marker, and it happens
*after* the `orderingChannel >= 32` validation.

`splitMessageProgressInterval` is 0 by default ([RakPeer.cpp:218](Source/RakPeer.cpp:218)), so no
`ID_DOWNLOAD_PROGRESS` messages are generated. When set, a locally-generated
`ID_DOWNLOAD_PROGRESS | partCount(u32 native) | partTotal(u32 native) | partLength(u32 native) | firstChunkBytes`
is pushed to the application every N chunks ([ReliabilityLayer.cpp:2671](Source/ReliabilityLayer.cpp:2671)).

There is **no timeout on partial split-packet channels** in this tree — `lastUpdateTime` is recorded
([ReliabilityLayer.cpp:2654](Source/ReliabilityLayer.cpp:2654)) but never checked; incomplete channels
are freed only when the connection is reset ([ReliabilityLayer.cpp:382](Source/ReliabilityLayer.cpp:382)).

---

## 9. ACK, NAK, RTT and retransmission

### 9.1 Generating ACKs

Every **data** datagram that passes `congestionManager.OnGotPacket` is acked, including datagrams whose
messages are all unreliable ("Ack even unreliable messages for congestion control, just don't resend
them on no ack", [ReliabilityLayer.cpp:738](Source/ReliabilityLayer.cpp:738)). The datagram number is
inserted into the `acknowlegements` range list
([ReliabilityLayer.cpp:2116](Source/ReliabilityLayer.cpp:2116)).

ACKs are flushed when `CCRakNetSlidingWindow::ShouldSendACKs` returns true
([CCRakNetSlidingWindow.cpp:87](Source/CCRakNetSlidingWindow.cpp:87)): immediately if no RTT sample
exists yet, otherwise once `curTime >= oldestUnsentAck + SYN` where `SYN = 10 000 µs` (10 ms)
([CCRakNetSlidingWindow.cpp:29](Source/CCRakNetSlidingWindow.cpp:29)).

### 9.2 Generating NAKs

On receiving a data datagram, the congestion manager reports how many datagram numbers were skipped
relative to `expectedNextSequenceNumber` ([CCRakNetSlidingWindow.cpp:127](Source/CCRakNetSlidingWindow.cpp:127)).
Each skipped number is inserted into the `NAKs` range list
([ReliabilityLayer.cpp:731](Source/ReliabilityLayer.cpp:731)); the NAK datagram goes out on the next
`Update`. Skip counts above 1000 are clamped, and above 50 000 the datagram is rejected outright.

### 9.3 Processing an ACK

[ReliabilityLayer.cpp:558](Source/ReliabilityLayer.cpp:558)–657:

1. Deserialise the range list; reject if `minIndex > maxIndex` or `maxIndex == 0xFFFFFF`.
2. For each acked datagram number: emit `ID_SND_RECEIPT_ACKED` for any matching
   unreliable-with-receipt entry, then look up `datagramHistory` for the linked list of reliable
   message numbers that datagram carried.
3. `congestionManager.OnAck(...)` with `ping = timeRead - whenSent` (the send time recorded in
   `datagramHistory`, since there is no timestamp on the wire in this build)
   ([ReliabilityLayer.cpp:641](Source/ReliabilityLayer.cpp:641)).
4. `RemovePacketFromResendListAndDeleteOlderReliableSequenced` for each message number
   ([ReliabilityLayer.cpp:2035](Source/ReliabilityLayer.cpp:2035)): clears
   `resendBuffer[msgNum & RESEND_BUFFER_ARRAY_MASK]`, unlinks from the resend list, and emits
   `ID_SND_RECEIPT_ACKED` for `>= RELIABLE_WITH_ACK_RECEIPT` types (for split messages only on the
   last chunk).

`datagramHistory` is bounded by `DATAGRAM_MESSAGE_ID_ARRAY_LENGTH = 512`
([RakNetDefines.h:83](Source/RakNetDefines.h:83)); an ack for an older datagram is ignored and causes a
spurious resend.

### 9.4 Processing a NAK

[ReliabilityLayer.cpp:658](Source/ReliabilityLayer.cpp:658)–712: for each naked datagram number, look up
its message numbers and set `nextActionTime = now` so they are resent on the next `Update`. Also calls
`congestionManager.OnNAK`.

### 9.5 RTT estimation

`CCRakNetSlidingWindow::OnAck` ([CCRakNetSlidingWindow.cpp:200](Source/CCRakNetSlidingWindow.cpp:200))
uses a Jacobson-style estimator with `d = 0.05`:

```
estimatedRTT += 0.05 * (rtt - estimatedRTT)
deviationRtt += 0.05 * (|rtt - estimatedRTT_prev| - deviationRtt)
```

seeded from the first sample. Times are in microseconds (`CC_TIME_TYPE_BYTES == 8`,
[CCRakNetSlidingWindow.h:65](Source/CCRakNetSlidingWindow.h:65)).

### 9.6 Retransmission timing

`GetRTOForRetransmission` ([CCRakNetSlidingWindow.cpp:285](Source/CCRakNetSlidingWindow.cpp:285)):

```
RTO = 2 * estimatedRTT + 4 * deviationRtt + 30 000 µs,  capped at 2 000 000 µs
RTO = 2 000 000 µs when no RTT sample exists yet
```

It ignores `timesSent` — there is **no exponential backoff per message**.

A message's `nextActionTime = now + RTO` is set when it is first sent
([ReliabilityLayer.cpp:1714](Source/ReliabilityLayer.cpp:1714)) and recomputed on each resend
([ReliabilityLayer.cpp:1607](Source/ReliabilityLayer.cpp:1607)). The resend list is a circular doubly
linked list ordered by `nextActionTime`; resent messages go to the tail
([ReliabilityLayer.cpp:2812](Source/ReliabilityLayer.cpp:2812)).

Resends re-serialise the message with a **new datagram number** but the **same reliable message
number** — that is what makes duplicate suppression work.

### 9.7 Connection death

`AckTimeout` ([ReliabilityLayer.cpp:2891](Source/ReliabilityLayer.cpp:2891)):

```c
return (timeLastDatagramArrived - curTime) > 10000 && curTime - timeLastDatagramArrived > timeoutTime;
```

`timeLastDatagramArrived` is refreshed on **any** received datagram
([ReliabilityLayer.cpp:531](Source/ReliabilityLayer.cpp:531)). Default `timeoutTime` is 10 000 ms
release / 30 000 ms debug ([ReliabilityLayer.cpp:232](Source/ReliabilityLayer.cpp:232),
[RakPeer.cpp:229](Source/RakPeer.cpp:229)). The check only runs when something is in the resend buffer
([ReliabilityLayer.cpp:1477](Source/ReliabilityLayer.cpp:1477)); the keepalive in §12 guarantees that
is periodically true.

---

## 10. Congestion control

`USE_SLIDING_WINDOW_CONGESTION_CONTROL` defaults to **1**
([RakNetDefines.h:98](Source/RakNetDefines.h:98)), selecting `CCRakNetSlidingWindow`
([ReliabilityLayer.h:515](Source/ReliabilityLayer.h:515)) and setting
`INCLUDE_TIMESTAMP_WITH_DATAGRAMS 0` ([ReliabilityLayer.h:38](Source/ReliabilityLayer.h:38)).

`CCRakNetSlidingWindow` is a plain TCP-Reno-style AIMD controller
([CCRakNetSlidingWindow.cpp](Source/CCRakNetSlidingWindow.cpp)):

* `cwnd` starts at the MTU payload size, `ssThresh = 0` (meaning "unset" -> slow start)
  ([CCRakNetSlidingWindow.cpp:42](Source/CCRakNetSlidingWindow.cpp:42)).
* Slow start: `cwnd += MTU` per ack; on crossing `ssThresh`, switch to
  `cwnd = ssThresh + MTU²/cwnd` ([CCRakNetSlidingWindow.cpp:238](Source/CCRakNetSlidingWindow.cpp:238)).
* Congestion avoidance: `cwnd += MTU²/cwnd` once per "congestion control block" (one `cwnd`-worth of
  datagram numbers).
* On a timeout resend during continuous send: `ssThresh = cwnd/2` (floor MTU), `cwnd = MTU`, re-enter
  slow start ([CCRakNetSlidingWindow.cpp:162](Source/CCRakNetSlidingWindow.cpp:162)).
* On NAK during continuous send: `ssThresh = cwnd/2`
  ([CCRakNetSlidingWindow.cpp:185](Source/CCRakNetSlidingWindow.cpp:185)).
* `GetTransmissionBandwidth() = max(0, cwnd - unacknowledgedBytes)`;
  `GetRetransmissionBandwidth() = unacknowledgedBytes` (retransmits are never throttled)
  ([CCRakNetSlidingWindow.cpp:65](Source/CCRakNetSlidingWindow.cpp:65)).
* Rate limiting only kicks in when `isContinuousSend` is true — a peer that sends sporadically is
  effectively unthrottled.

**Wire-visible consequences of this choice:**

| Field | Sliding window (default) | `CCRakNetUDT` |
|---|---|---|
| 4-byte `sourceSystemTime` in data and ACK headers | absent | present |
| `hasBAndAS` bit in ACKs | always 0 | set at most once per SYN |
| `AS` float in ACKs | never | present when `hasBAndAS` |
| `needsBAndAs` bit in data headers | usually 1 (slow start) | reflects UDT slow start |
| `isPacketPair` | always 0 (pairing code commented out) | also 0 in this tree |

Switching congestion controllers therefore changes the byte layout of the datagram header and is not
interoperable.

---

## 11. MTU

| Constant | Value | Site |
|---|---|---|
| `MAXIMUM_MTU_SIZE` | 1492 | [MTUSize.h:32](Source/MTUSize.h:32) |
| `MINIMUM_MTU_SIZE` | 400 | [MTUSize.h:33](Source/MTUSize.h:33) (declared, not used in the probe) |
| `mtuSizes[]` | `{1492, 1200, 576}` | [RakPeer.cpp:115](Source/RakPeer.cpp:115) |
| `UDP_HEADER_SIZE` | 28 | [CCRakNetSlidingWindow.h:51](Source/CCRakNetSlidingWindow.h:51) |
| `defaultMTUSize` | 576 (`mtuSizes[NUM_MTU_SIZES-1]`) | [RakPeer.cpp:197](Source/RakPeer.cpp:197) |

Discovery, in full:

1. The client sends `ID_OPEN_CONNECTION_REQUEST_1` padded to `mtuSizes[i] - 28` payload bytes with the
   don't-fragment bit set, four times per size, 500 ms apart.
2. The server measures the *received* payload length and answers with
   `mtu = min(1492, length + 28)`, itself padded to `mtu - 28` bytes with DF set — so the probe is
   bidirectional.
3. The client echoes that `mtu` in `ID_OPEN_CONNECTION_REQUEST_2`; the server echoes it again in
   `ID_OPEN_CONNECTION_REPLY_2`.
4. Both sides call `AssignSystemAddressToRemoteSystemList`, which takes
   `MTUSize = max(defaultMTUSize, incomingMTU)` ([RakPeer.cpp:3177](Source/RakPeer.cpp:3177)–3182), so
   the effective floor is 576.
5. `ReliabilityLayer::Reset(true, MTUSize, ...)` hands `MTUSize - 28` to the congestion manager
   ([ReliabilityLayer.cpp:282](Source/ReliabilityLayer.cpp:282)).

There is no in-connection MTU renegotiation; the value is fixed at connect time.

---

## 12. Ping, pong, keepalive and disconnection

### 12.1 `ID_CONNECTED_PING` / `ID_CONNECTED_PONG`

`PingInternal` ([RakPeer.cpp:3558](Source/RakPeer.cpp:3558)):

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | `ID_CONNECTED_PING` (0) |
| 1 | 8 | `RakNet::GetTime()` |

Sent `IMMEDIATE_PRIORITY`, reliability chosen by the caller (`UNRELIABLE` for the periodic ping,
`RELIABLE` for the keepalive).

Reply ([RakPeer.cpp:5395](Source/RakPeer.cpp:5395)), sent `IMMEDIATE_PRIORITY, UNRELIABLE`:

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | `ID_CONNECTED_PONG` (3) |
| 1 | 8 | the ping's time, echoed |
| 9 | 8 | the responder's `RakNet::GetTime()` |

Exactly 17 bytes; the handler requires that exact length ([RakPeer.cpp:5378](Source/RakPeer.cpp:5378)).
`OnConnectedPong` ([RakPeer.cpp:3846](Source/RakPeer.cpp:3846)) records
`ping = now - sendPingTime` and `clockDifferential = sendPongTime - (now/2 + sendPingTime/2)` into a
5-entry ring ([RakNetTypes.h:431](Source/RakNetTypes.h:431), `PING_TIMES_ARRAY_SIZE`).

The same ping/pong time pair is piggybacked on `ID_CONNECTION_REQUEST_ACCEPTED` and
`ID_NEW_INCOMING_CONNECTION`, so a clock estimate exists as soon as the handshake completes.

### 12.2 Periodic ping

Every 5000 ms while `CONNECTED`, **only if** `occasionalPing` is on or no pong has ever arrived
(`lowestPing == 0xFFFF`) ([RakPeer.cpp:5233](Source/RakPeer.cpp:5233)). `occasionalPing` defaults to
**false** ([RakPeer.cpp:208](Source/RakPeer.cpp:208)) unless `GET_TIME_SPIKE_LIMIT` is defined (it is
not defined anywhere in this tree), so in a default build this fires only until the first pong.

### 12.3 Keepalive

The real liveness mechanism ([RakPeer.cpp:5174](Source/RakPeer.cpp:5174)): if more than
`timeoutTime / 2` ms have elapsed since the last reliable send and the resend buffer is empty, send a
`RELIABLE` `ID_CONNECTED_PING`. That guarantees an outstanding reliable message roughly every 5 s
(default), which in turn arms the `AckTimeout` path in §9.7.

### 12.4 Disconnection

`NotifyAndFlagForShutdown` ([RakPeer.cpp:3098](Source/RakPeer.cpp:3098)) sends a 1-byte
`ID_DISCONNECTION_NOTIFICATION` (21) as `RELIABLE_ORDERED` and sets the local state to
`DISCONNECT_ASAP`. The peer that receives it moves to `DISCONNECT_ON_NO_ACK`
([RakPeer.cpp:5413](Source/RakPeer.cpp:5413)) so it can still ack the notification before tearing down.

Locally generated (never on the wire): `ID_CONNECTION_LOST` (22) when a `CONNECTED` system dies,
`ID_CONNECTION_ATTEMPT_FAILED` (17) when a `REQUESTED_CONNECTION` dies
([RakPeer.cpp:5208](Source/RakPeer.cpp:5208)–5214).

### 12.5 Ack receipts

`ID_SND_RECEIPT_ACKED` (14) and `ID_SND_RECEIPT_LOSS` (15) are 5-byte locally-generated messages:
the ID byte plus the 4-byte `sendReceiptSerial` copied with `memcpy`, i.e. in **native** byte order,
not big-endian ([ReliabilityLayer.cpp:2084](Source/ReliabilityLayer.cpp:2084),
[ReliabilityLayer.cpp:1542](Source/ReliabilityLayer.cpp:1542)). They never traverse the network.

---

## 13. Build switches that change the wire format

| Macro | Default | Effect if changed |
|---|---|---|
| `USE_SLIDING_WINDOW_CONGESTION_CONTROL` | 1 ([RakNetDefines.h:103](Source/RakNetDefines.h:103)) | 0 selects `CCRakNetUDT` and sets `INCLUDE_TIMESTAMP_WITH_DATAGRAMS 1`, adding a 4-byte `TimeMS` to data and ACK headers |
| `__GET_TIME_64BIT` | 1 ([RakNetDefines.h:22](Source/RakNetDefines.h:22)) | 0 makes `RakNet::Time` 4 bytes, halving every timestamp field in the handshake and ping messages |
| `__BITSTREAM_NATIVE_END` | undefined ([RakNetDefines.h:33](Source/RakNetDefines.h:33)) | defining it removes all byte swapping, making multi-byte fields host-endian (little-endian on x86) and breaking interop with default builds on LE hosts |
| `LIBCAT_SECURITY` | 0 ([NativeFeatureIncludes.h:42](Source/NativeFeatureIncludes.h:42)) | 1 adds cookies, public keys, challenge/answer and per-datagram authenticated encryption ([SecureHandshake.cpp:17](Source/SecureHandshake.cpp:17), [ReliabilityLayer.cpp:533](Source/ReliabilityLayer.cpp:533), [ReliabilityLayer.cpp:1930](Source/ReliabilityLayer.cpp:1930)) |
| `RAKNET_SUPPORT_IPV6` | 0 ([RakNetDefines.h:107](Source/RakNetDefines.h:107)) | 1 changes `SystemAddress` from 7 bytes to `1 + sizeof(sockaddr_in6)` |
| `RAKNET_PROTOCOL_VERSION` | 6 ([RakNetVersion.h:21](Source/RakNetVersion.h:21)) | must match exactly or `ID_INCOMPATIBLE_PROTOCOL_VERSION` is returned |
| `MAXIMUM_MTU_SIZE` | 1492 ([MTUSize.h:32](Source/MTUSize.h:32)) | changes the largest probe and the receive buffer |
| `NUMBER_OF_ORDERED_STREAMS` | 32 ([ReliabilityLayer.h:46](Source/ReliabilityLayer.h:46)) | receiver rejects `orderingChannel >= 32` regardless ([ReliabilityLayer.cpp:2350](Source/ReliabilityLayer.cpp:2350)) |
| `MAXIMUM_NUMBER_OF_INTERNAL_IDS` | 10 ([RakNetDefines.h:68](Source/RakNetDefines.h:68)) | changes the length of the IP list in `ID_CONNECTION_REQUEST_ACCEPTED` / `ID_NEW_INCOMING_CONNECTION` |
| `DATAGRAM_MESSAGE_ID_ARRAY_LENGTH` / `RESEND_BUFFER_ARRAY_LENGTH` | 512 / 512 | how far back acks are honoured, and how many reliable messages may be in flight |

---

## 14. Other wire-visible details

* **Encryption.** With `LIBCAT_SECURITY == 0` (default) nothing is encrypted: the datagram is written
  to the socket exactly as serialised ([ReliabilityLayer.cpp:1962](Source/ReliabilityLayer.cpp:1962)).
  There is no checksum or MAC; the `GetSHA1`/`CheckSHA1` helpers
  ([ReliabilityLayer.cpp:2394](Source/ReliabilityLayer.cpp:2394)) exist but are never called.
* **No sequence-number randomisation.** Datagram numbers, reliable message numbers, ordering and
  sequencing indices all start at 0 for every connection
  ([ReliabilityLayer.cpp:305](Source/ReliabilityLayer.cpp:305),
  [CCRakNetSlidingWindow.cpp:52](Source/CCRakNetSlidingWindow.cpp:52)).
* **Padding.** The second datagram of a "packet pair" would be zero-padded to the first's length
  ([ReliabilityLayer.cpp:1844](Source/ReliabilityLayer.cpp:1844)); since pairing is disabled this never
  happens. Trailing zero bytes are nonetheless safe: the message parser stops when fewer than 32 bits
  remain, and a zero reliability/length combination is rejected.
* **Banned peers** are answered with `ID_CONNECTION_BANNED` before any other processing, including
  before the offline-magic check on the incoming datagram
  ([RakPeer.cpp:3953](Source/RakPeer.cpp:3953)).
* **Socket options.** `SO_RCVBUF` 256 KiB, `SO_SNDBUF` 16 KiB, `SO_LINGER` 0, non-blocking
  ([SocketLayer.cpp:71](Source/SocketLayer.cpp:71)). `IP_DONTFRAGMENT` is toggled only around the two
  MTU-probe sends.
* **Same-IP flood protection.** A second connection from an IP that connected within the last 100 ms is
  refused with `ID_IP_RECENTLY_CONNECTED` ([RakPeer.cpp:3150](Source/RakPeer.cpp:3150)).
* **Loopback sends** bypass the wire entirely ([RakPeer.cpp:1014](Source/RakPeer.cpp:1014)).
* **Modernisation in this tree** (`List` -> `std::vector`, `LinkedList` -> `std::list`,
  `Heap` -> `std::priority_queue`, `Queue` -> `std::deque`) is confined to container choice. Every
  serialisation path — `DatagramHeaderFormat`, `WriteToBitStreamFromInternalPacket`,
  `RangeList::Serialize`, `BitStream` — is byte-for-byte identical to upstream RakNet 4.081, so this
  build interoperates with stock RakNet 4.081 peers.

---

## 15. Complete worked example

A `RELIABLE_ORDERED` 4-byte user message `86 01 02 03` on channel 0, the first message on a fresh
connection, sent as the only message in a datagram with datagram number 7, ordering index 0, reliable
message number 0:

```
offset  bytes                       meaning
------  --------------------------  ------------------------------------------
  0     84                          1000 0100b
                                      bit7 isValid   = 1
                                      bit6 isACK     = 0
                                      bit5 isNAK     = 0
                                      bit4 isPacketPair    = 0
                                      bit3 isContinuousSend= 0
                                      bit2 needsBAndAs     = 1
                                      bits1-0 pad
  1     07 00 00                    datagramNumber = 7 (uint24 LE)
  4     60                          0110 0000b
                                      bits7-5 reliability = 3 (RELIABLE_ORDERED)
                                      bit4 hasSplitPacket = 0
                                      bits3-0 pad
  5     00 20                       dataBitLength = 32 (uint16 BE)
  7     00 00 00                    reliableMessageNumber = 0 (uint24 LE)
 10     00 00 00                    orderingIndex = 0 (uint24 LE)
 13     00                          orderingChannel = 0
 14     86 01 02 03                 payload (ID_USER_PACKET_ENUM + 3 bytes)
------  --------------------------
total 18 bytes of UDP payload (46 bytes on the wire with IP+UDP)
```

The peer replies, up to 10 ms later, with an ACK datagram:

```
offset  bytes                       meaning
------  --------------------------  ------------------------------------------
  0     C0                          1100 0000b -> isValid=1, isACK=1, hasBAndAS=0, pad
  1     00 01                       range count = 1 (uint16 BE)
  3     01                          minEqualsMax = 1
  4     07 00 00                    minIndex = 7 (uint24 LE)
------  --------------------------
total 7 bytes
```

---

## 16. Known inconsistencies and open questions

These are places where the code in this tree is self-inconsistent, dead, or where behaviour could not be
pinned down from the source alone.

1. **`ID_CONNECTION_REQUEST` is parsed two different ways.**
   `ParseConnectionRequestPacket` reads `MessageID | GUID | Time | doSecurity`
   ([RakPeer.cpp:2990](Source/RakPeer.cpp:2990)), matching the writer. But the "already connected, reply
   anyway" path reads `MessageID | OFFLINE_MESSAGE_DATA_ID | GUID | Time`
   ([RakPeer.cpp:5297](Source/RakPeer.cpp:5297)) — 16 bytes too far. The timestamp echoed back in that
   case is garbage. This matches upstream RakNet 4.081; it is not a regression in this tree.
2. **`GetDataHeaderByteLength()` returns 9 for a 4-byte header** — see §4.5. Harmless but wasteful.
3. **`RangeList::Serialize` can loop forever in principle.** `SendACKs` loops
   `while (acknowlegements.Size() > 0)`; if `maxBits` were ever too small for a single range,
   `countWritten` would be 0, nothing would be removed, and the loop would not terminate
   ([ReliabilityLayer.cpp:3081](Source/ReliabilityLayer.cpp:3081),
   [DS_RangeList.h:78](Source/DS_RangeList.h:78)). With the smallest supported MTU (576) this cannot
   happen.
4. **`ID_DETECT_LOST_CONNECTIONS` (4) is received and discarded but never sent** by anything in
   `Source/` outside of `Plugins/PacketLogger.cpp`'s name table.
5. **Packet pairs are dead code.** The `isPacketPair` bit, `TagMostRecentPushAsSecondOfPacketPair`,
   `countdownToNextPacketPair` and `OnGotPacketPair` all exist, but the tagging call is commented out
   ([ReliabilityLayer.cpp:2958](Source/ReliabilityLayer.cpp:2958)). A reimplementation must still parse
   the bit and must still tolerate a zero-padded datagram.
6. **`ID_OPEN_CONNECTION_REPLY_2` ends on a bit, not a byte.** The `requiresSecurityOfThisClient`
   `bool` occupies one bit, so the message is 35 bytes with 7 pad bits. Easy to get wrong when
   reimplementing.
7. **Padding bits are not explicitly zeroed** by `AlignWriteToByteBoundary`
   ([BitStream.h:571](Source/BitStream.h:571)). In practice they are 0 because bytes are zeroed as they
   are started, but nothing in the code guarantees it, so a reimplementation should not depend on the
   pad bits' value and should not reject non-zero padding.
8. **`unreliableWithAckReceiptHistory` expiry uses wrapping arithmetic**
   (`time - it->nextActionTime < ((CCTimeType)-1)/2`,
   [ReliabilityLayer.cpp:1536](Source/ReliabilityLayer.cpp:1536)). This is intentional overflow-safe
   comparison, but the same idiom appears in the resend loop and is easy to misread as a bug.
9. **`MINIMUM_MTU_SIZE` (400) is unused.** The real floor is `defaultMTUSize` = 576.
10. **No partial split-packet timeout.** `SplitPacketChannel::lastUpdateTime` is written but never
    read; a peer that sends chunk 0 of a 1000-chunk message and then goes quiet leaves the memory
    allocated until the connection is reset. Whether upstream intended a reaper here is not
    determinable from the code.
11. **`isContinuousSend` semantics are subtle.** It is `bandwidthExceededStatistic` from the *previous*
    flush ([ReliabilityLayer.cpp:1507](Source/ReliabilityLayer.cpp:1507)) and is then force-set for all
    but the first datagram of a burst ([ReliabilityLayer.cpp:1789](Source/ReliabilityLayer.cpp:1789)).
    Its only effect on the receiver is being handed to `OnGotPacket`, which the sliding-window
    controller ignores; it matters to `CCRakNetUDT`.
