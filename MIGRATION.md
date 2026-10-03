# Migrating from stock RakNet 4.081

This fork modernized the core of RakNet 4.081 to C++17. Most of that is invisible to you.
Five things are not: they will either stop your build, or — in exactly one case — change
bytes on the wire.

This document is written for someone who has a working 4.081 integration and wants to move
it onto this fork. It is not a changelog. Each entry says what the API was, what it is now,
and what to do about it.

Hardening limits sit outside that count. They break no API and stop no build, but they
reject inputs stock accepted, so they have a section of their own at the end:
[Limits that stock did not have](#limits-that-stock-did-not-have). One of them adds fields
to `RakNetStatistics`, which changes that struct's layout, and another adds two pure virtual
getters to `RakPeerInterface`, which only a class of your own implementing it would notice.

Some plugins no longer act on a remote System's word alone. They break no build either, but
an integration that relied on them can stop working at runtime, so they are listed in
[Plugins that act only on Solicited or Designated messages](#plugins-that-act-only-on-solicited-or-designated-messages).

If you hooked out-of-memory with `SetNotifyOutOfMemory`, it now hears about far less. See
[Exceptions and out-of-memory](#exceptions-and-out-of-memory).

`RakPeer`'s getters now answer from a snapshot, and a closed connection no longer answers
at all. See [Getters answer only for open connections](#getters-answer-only-for-open-connections).

On a little-endian host, a Timestamped Message now carries the time its sender wrote, shifted
to your clock, where stock handed it out with its bytes reversed. Nothing on the wire changed.
See
[Timestamped Messages arrive shifted](#timestamped-messages-arrive-shifted).

**The short version:** four of the five breaks are source-only. The core wire protocol is
byte-identical to stock 4.081, and this fork interoperates with a stock 4.081 peer. The one
wire-visible change is confined to a single RPC4 error payload that stock 4.081 could not
parse anyway. One further reader-side difference — how an empty `std::string` is read back
— is described under break 3; it fixes a bug stock had, and it cannot reach the core
protocol.

| # | Break | Kind |
|---|-------|------|
| 1 | Everything moved into `namespace RakNet` | Source only |
| 2 | `DataStructures::List` gone from public signatures | Source only |
| 3 | `RakString` / `RakWString` replaced by `std::string` | Source only |
| 4 | `BitStream` rejects raw character buffers | Source only |
| 5 | `ID_RPC_REMOTE_ERROR`'s payload is length-prefixed | **Wire-visible** |

---

## What did *not* change: the core protocol

This is the claim you most need and are least likely to believe, so here is the evidence
rather than the assertion.

**Protocol version is still 6.** `RAKNET_PROTOCOL_VERSION` in `Source/RakNetVersion.h` is
unchanged. It is exchanged during the connection handshake, and a mismatch is what produces
`ID_INCOMPATIBLE_PROTOCOL_VERSION`. A stock 4.081 peer and a peer built from this fork
agree on it.

**All 147 `ID_*` enumerators are unchanged and in the same order.**
`Source/MessageIdentifiers.h` declares 12 `OutOfBandIdentifiers` and 135
`DefaultMessageIDTypes` — 147 in total, ending at `ID_USER_PACKET_ENUM`. Extracting the
enumerator sequence from the pre-modernization tree and from this one yields two identical
lists. Nothing was inserted, removed, or reordered, so no message id shifted — including
`ID_USER_PACKET_ENUM`, which is where your own ids start. This survived the plugin deletion
below intact: the ids belonging to deleted plugins are still declared, still in place,
holding their numbering.

**The serialization call sequences in `RakPeer.cpp` and `ReliabilityLayer.cpp` are
identical.** These two files are where the core protocol is actually written and read — the
connection handshake, pings, and the datagram header and message framing in
`DatagramHeaderFormat` and `InternalPacket`. Extracting every `Write`, `Read`,
`WriteCompressed`, `ReadCompressed`, `WriteBits`, `ReadBits`, `WriteAlignedBytes`,
`ReadAlignedBytes`, `WriteDelta`, `ReadDelta`, `AlignWriteToByteBoundary`,
`AlignReadToByteBoundary`, `IgnoreBytes` and `IgnoreBits` call from both revisions gives
two identical sequences, matching on argument text and not merely on the method names.
Counting only live calls — that is, discarding everything after a `//` on each line first
— there are 160 in `RakPeer.cpp` and 59 in `ReliabilityLayer.cpp`, the same before and
after. Count *without* discarding comments and `ReliabilityLayer.cpp` gives 72 before and
70 after; those two lost entries are commented-out calls that went with their comments,
and they are the only difference the comparison turns up at all.

**The StringCompressor wire form is unchanged for the default language.** The per-language
Huffman trees were removed and `EncodeString`/`DecodeString` lost their `languageId`
parameter, but the surviving tree is generated from the same 256-entry
`englishCharacterFrequencies` table, value for value, and the encode path still writes a
compressed `uint32` bit length followed by that many bits. `languageId` defaulted to 0
everywhere in stock, so anything that used the default — which is everything in stock's own
core — produces identical bytes. If you registered a *non-default* language tree with
`GenerateTreeFromStrings` and passed a non-zero `languageId`, that facility is gone and
there is no replacement.

Core RakNet does not serialize strings on the wire at all: neither `RakPeer.cpp` nor
`ReliabilityLayer.cpp` contains a single length-prefixed or compressed string write. Every
string on the wire belongs to a plugin or to your own code. That is why breaks 3, 4 and 5
below, which are all about strings, cannot reach the core protocol.

---

## 1. Everything moved into `namespace RakNet`

**Source break.** Most of RakNet was already in `namespace RakNet` in stock 4.081. A
handful of headers were not, and they are now: `MessageIdentifiers.h`, `PacketPriority.h`,
`DR_SHA1.h`, `SingleProducerConsumer.h`, `SuperFastHash.h`, `WSAStartupSingleton.h`, and
the surviving `DS_*` headers.

The two that matter to ordinary code are `PacketPriority` and `PacketReliability`, which
were global enums and are now `RakNet::PacketPriority` and `RakNet::PacketReliability`.
Their enumerators — `HIGH_PRIORITY`, `RELIABLE_ORDERED`, and the rest — moved with them.
So did every `ID_*` enumerator in `MessageIdentifiers.h`.

The `DataStructures` namespace is now nested: `RakNet::DataStructures`.

### What to do

Add `using namespace RakNet;` in the translation units that need it, or qualify. If you
already had `using namespace RakNet;` — which stock's own samples did — you will not notice
this break at all.

Watch for one thing a `using` directive will not fix: a forward declaration of a RakNet
type at global scope, such as `class RakPeerInterface;` or `enum PacketPriority : int;`,
now declares a *different* type. Include the header instead.

---

## 2. `DataStructures::List` disappeared from public signatures

**Source break.** `DS_List.h` no longer exists. `DataStructures::List` was a hand-rolled
dynamic array with `Size()`, `Insert()`, `Push()` and `operator[]`; every use of it is now
`std::vector`.

Three methods on `RakPeerInterface` change signature:

```cpp
// Before
virtual void GetSystemList( DataStructures::List<SystemAddress>& addresses,
                            DataStructures::List<RakNetGUID>& guids ) const = 0;
virtual void GetSockets( DataStructures::List<RakNetSocket2*>& sockets ) = 0;
virtual void GetStatisticsList( DataStructures::List<SystemAddress>& addresses,
                                DataStructures::List<RakNetGUID>& guids,
                                DataStructures::List<RakNetStatistics>& statistics ) = 0;

// After
virtual void GetSystemList( std::vector<SystemAddress>& addresses,
                            std::vector<RakNetGUID>& guids ) const = 0;
virtual void GetSockets( std::vector<RakNetSocket2*>& sockets ) = 0;
virtual void GetStatisticsList( std::vector<SystemAddress>& addresses,
                                std::vector<RakNetGUID>& guids,
                                std::vector<RakNetStatistics>& statistics ) = 0;
```

The same substitution ran through the rest of the library — `TCPInterface`,
`ConsoleServer`, `LogCommandParser` and the plugin implementations all hold `std::vector`
or `std::deque` members now — but those are internal. The three above are the ones you call.

`PluginInterface2`'s own virtual callbacks are unaffected: none of them ever took a
`DataStructures::List`, and their signatures are unchanged in this fork. What does reach a
plugin is the namespace nesting in break 1 and the `std::string` substitution in break 3.

### What to do

Change the variables you pass to `std::vector`. If you derive from `RakPeerInterface` or
override any of these three, update the overrides too — they are pure virtual, so a missed
one is a compile error and not a silent non-override. Elsewhere, `Size()` becomes `size()`,
`Push( x, _FILE_AND_LINE_ )` becomes `push_back( x )`, and `Insert( x, pos,
_FILE_AND_LINE_ )` becomes `insert( begin() + pos, x )` — the trailing `_FILE_AND_LINE_`
arguments every mutator took for the allocation tracker have no counterpart and are simply
dropped.

---

## 3. `RakString` and `RakWString` are gone

**Source break. The wire encodings are unchanged**, with one reader-side caveat noted at
the end of this section. `RakString.h` and `RakWString.h` no longer exist. Everywhere they
appeared, this fork uses `std::string`.

`RakString` was a reference-counted copy-on-write string with a pooled allocator, format
helpers, URL and path utilities and its own serialization. `std::string` replaces the
string; nothing replaces the utilities. `RakWString` has no replacement at all — nothing in
this fork serializes wide strings any more.

The wire encodings are unchanged. `RakString::Serialize` wrote a `uint16` length followed
by that many aligned bytes; `BitStream::Write( std::string )` writes exactly that.
`RakString::SerializeCompressed` called `StringCompressor::EncodeString`; so does
`BitStream::WriteCompressed( std::string )`. Anything that was on the wire as a `RakString`
is on the wire identically as a `std::string`.

Related signature changes:

```cpp
// StringCompressor - the RakString overloads are gone and every remaining one lost its
// trailing languageId parameter. The char* pair survives; the std::string pair is new.
void EncodeString( const char* input, int maxCharsToWrite, BitStream* output );
bool DecodeString( char* output, int maxCharsToWrite, BitStream* input );
void EncodeString( const std::string& input, int maxCharsToWrite, BitStream* output );
bool DecodeString( std::string& output, int maxCharsToWrite, BitStream* input );

// SocketLayer
static std::string GetSubNetForSocketAndIp( __UDPSOCKET__ inSock,
                                            const std::string& inIpString );
```

The `RAKSTRING_TYPE` macro in `RakNetDefines.h`, which selected `RakString` or `RakWString`
depending on `_UNICODE`, is gone with them.

### What to do

Replace `RakNet::RakString` with `std::string`. `C_String()` becomes `c_str()`,
`GetLength()` becomes `size()`. For the utility methods there is no drop-in: `RakString`'s
`URLEncode`, `MakeFilePath`, `FormatForPUTOrPost` and friends need replacing with whatever
your project already uses.

For `RakWString`: pick an encoding — UTF-8 is the obvious one — convert at your own
boundary, and put a `std::string` on the wire.

### One behavioural difference to know about

`BitStream::Deserialize( std::string& )` calls `AlignReadToByteBoundary()` for a
zero-length string. `RakString::Deserialize` did not: it read the `uint16` length and, on
zero, returned without touching the read offset — while the *writer* had aligned
unconditionally, because `WriteAlignedBytes` aligns before it looks at the length. Stock
4.081 therefore could not round-trip an empty string written at a non-byte-aligned offset;
every field after it came back garbage. This fork fixes the reader.

The consequence for you: this fork's reader and a stock 4.081 reader disagree about an
empty string written at a non-aligned offset. The stock one is the one that is wrong, and
core RakNet never reaches the case because it never serializes strings, so this cannot
affect the connection handshake or the reliability layer. It can affect a plugin or an
application payload that writes an empty string after a bit-packed field.

---

## 4. `BitStream` and raw character buffers

**Source break, not a wire break.** `Write( std::string )` emits exactly what
`RakString::Serialize` emitted.

### Before (stock 4.081)

```cpp
bs.Write( "hello" );        // five non-template overloads, char*/const char*/
bs.Write( charPtr );        // unsigned char*/const unsigned char*/const wchar_t*,
                            // each calling RakString::Serialize - length-prefixed
```

### After

```cpp
bs.Write( std::string( s ) );    // same wire format as RakString::Serialize
bs.Write( s, lengthInBytes );    // for a raw byte range, which is a different thing
```

All the spellings deduction really produces — `char*`, `const char*`, `unsigned char*`,
`const unsigned char*`, and arrays of `char`/`unsigned char` of any extent, which is what
a string literal is — are now `= delete`d in the class declaration, so both calls above
are compile errors. The `wchar_t` spellings upstream also had go with them, and those
have no replacement: nothing in this fork serializes wide strings any more, so the caller
picks an encoding and writes a `std::string`.

The same treatment covers `WriteCompressed()`, `Read()` and `ReadCompressed()`. The rule is
one sentence: **a raw character buffer is never a valid argument to the one-argument form of
any of the four**, whatever its constness and whether it is narrow, wide or an array. Every
such call is a compile error naming a `std::string` alternative.

```cpp
bs.WriteCompressed( std::string( s ) );   // instead of WriteCompressed( charPtr )
bs.Read( str );                           // std::string&, instead of Read( charPtr )
bs.ReadCompressed( str );                 // likewise
bs.Read( output, numberOfBytes );         // still there, for a raw byte range
```

The deleted overloads carry an unused trailing tag parameter so that the alternative appears
in the compiler's diagnostic; C++17 has no `= delete( "reason" )`. The tag names the
direction — `Write_a_std_string_instead` on the write side, `Read_a_std_string_instead` on
the read side — so a rejected `Read` does not advise writing.

`Read( RakString& )` and `Read( RakWString& )` are gone with the types; `Read( std::string& )`
replaces them.

### The entry points that forward

`WriteDelta()`, `Serialize()`, `WriteCompressedDelta()`, `SerializeCompressed()`,
`ReadDelta()` and `ReadCompressedDelta()` have no deletions of their own. They forward a raw
buffer into one of the four guarded functions, so they are compile errors through it —
`WriteDelta( charPtr, charPtr )` names the deleted `Write( char*, … )`. The only difference
is that the diagnostic is reported inside `BitStream.h` rather than at your call site.

### The hazard this replaced (only in this fork, between `42353af` and the fix in `67276b7`)

For that window the two calls compiled and silently mis-encoded: `Write( "hello" )`
deduced `templateType = char[6]` and wrote six raw bytes with no length prefix
(byte-reversed on a big-endian target), and `Write( charPtr )` deduced
`templateType = char*` and wrote the pointer value onto the wire. The two
declared-but-undefined specializations meant to force a linker error were never
reached by deduction. Anyone who built against this fork in that window, rather than
against stock 4.081, has to re-check those call sites for produced-and-stored data.

### Why deleting rather than restoring `Write( const char* )` with `std::string` semantics

Deleting forces every call site to be read. A caller who wrote `Write( p )` may have
meant the string at `p`, in which case `Write( std::string( p ) )` is right — or the
bytes at `p`, in which case the string form is wrong and `Write( p, length )` is the
replacement. A restored overload would silently pick one of those for them.

---

## 5. `ID_RPC_REMOTE_ERROR`'s payload is length-prefixed

**Wire-visible.** This is the one place this fork's bytes differ from stock 4.081 by
intent.

### Before (stock 4.081)

The RPC4 plugin wrote the name of the unregistered function as a raw C string with its
terminator and no length prefix, from all three sites that produce the error — the two
in `RPC4::OnReceive` and the loopback one in `RPC4::CallLoopback`:

```cpp
bsOut.Write( functionName.C_String(), functionName.GetLength() + 1 );  // network
strcpy( (char*)p->data + 2, uniqueID );                                // loopback
```

Its only reader, the receive loop in `RPC4::CallBlocking`, read it back as a
length-prefixed `RakString`:

```cpp
bsIn.IgnoreBytes( 2 );
bsIn.Read( functionName );   // uint16 length, then that many bytes
```

So upstream could not parse its own error packet: the reader took the first two
characters of the function name as the length and then ran off the end. There was no
working behaviour here to be compatible with.

### After

All three sites write the length-prefixed form, `BitStream::Write( std::string )` — a
`uint16` length followed by the characters, no terminator — which is what the reader
already expected. The reader is unchanged.

### What to do about it

It is confined to the RPC4 plugin's payload; the core protocol is untouched. A peer running
stock 4.081 and a peer running this fork will disagree on the contents of an
`ID_RPC_REMOTE_ERROR` packet — but since neither stock peer could parse the stock form,
what is lost is a message that never worked. Code that reached into `packet->data + 2`
and treated it as a C string must read it with `BitStream::Read( std::string )` instead.
`ID_RPC4_CALL` is unaffected: it used `StringCompressor` before and after.

---

## Also removed

Not API breaks in the sense above — these features are simply not here. If you used one,
this fork is not a drop-in replacement, and you will need to keep the stock implementation
or do without.

**Plugins:** `ReplicaManager3`, `FullyConnectedMesh2`, `ConnectionGraph2`, `ReadyEvent`,
`TeamBalancer`, `TeamManager`, `FileListTransfer`, `DirectoryDeltaTransfer`,
`CloudClient`/`CloudServer`, `HTTPConnection`/`HTTPConnection2`, `EmailSender`, `DynDNS`,
`Rackspace`, the Autopatcher, `NetworkIDManager`/`NetworkIDObject`, `StringTable`,
`TableSerializer`, `VariableDeltaSerializer`, and `DataCompressor`.

**Data structures:** `DS_List`, `DS_Map`, `DS_Queue`, `DS_LinkedList`,
`DS_QueueLinkedList`, `DS_BPlusTree`, `DS_BinarySearchTree`, `DS_Heap`, `DS_Hash`,
`DS_Multilist`, `DS_OrderedChannelHeap`, `DS_Table`, `DS_Tree`, `DS_WeightedGraph`,
`DS_BytePool`. The standard library covers all of them.

**Utilities:** `RakString`, `RakWString`, `RakNetSmartPtr`, `RefCountedObj`, `SimpleMutex`,
`RakSleep`, `LocklessTypes`, `Base64Encoder`, `CheckSum`, `FormatString`, `Itoa`,
`EpochTimeToString`, `GridSectorizer`, `ThreadPool`, and the
`Gets`/`Getche`/`Kbhit`/`_FindFirst` platform shims. `<mutex>`, `<thread>`, `<atomic>` and
`<chrono>` replace the threading and timing ones.

**Console platform headers:** `PS3Includes.h`, `PS4Includes.h`, `VitaIncludes.h`,
`XBox360Includes.h`.

**Build switches:** `PREALLOCATE_LARGE_MESSAGES` and `USE_THREADED_SEND`. Neither
compiles in 4.081, so nothing you were doing stops working.

- `PREALLOCATE_LARGE_MESSAGES` selected a different strategy for reassembling split packets.
  Its code never compiled in any RakNet release, so no build can ever have had it set to `1`.
- `USE_THREADED_SEND` sent outgoing datagrams from a worker thread, and `SendToThread.h` went
  with it. Its code stopped compiling when stock moved sends onto `RakNetSocket2`, and it
  does not compile in 4.081. A build that still defines it now compiles and sends inline
  from the update cycle.

**`RakAlloca.h`, `USE_ALLOCA`, `MAX_ALLOCA_STACK_ALLOCATION`.** Source-only, not
wire-visible. `RakAlloca.h` is gone, and RakNet no longer uses `alloca` anywhere. The public
`RakMemoryOverride.h` used to include it, which pulled `<malloc.h>` (and `<alloca.h>` outside
Windows and FreeBSD) into every file that included a RakNet header. Code that called
`alloca`, `_alloca` or `malloc.h` functions without including the header itself will now fail
to compile. Fix: include `<malloc.h>`/`<alloca.h>` directly.

`USE_ALLOCA` and `MAX_ALLOCA_STACK_ALLOCATION` are no longer read. A build that defines
them still compiles, and it loses nothing: the stack buffers they controlled have been
replaced by code that needs no scratch buffer at all.

The plugins that remain are `NatPunchthroughClient`/`Server`, `NatTypeDetectionClient`/
`Server`, `Router2`, `RelayPlugin`, `UDPProxyClient`/`Coordinator`/`Server`,
`UDPForwarder`, `RPC4Plugin`, `MessageFilter`, `TwoWayAuthentication`, `StatisticsHistory`,
`RakNetTransport2`, `TelnetTransport`, and the `PacketLogger` family.

## Limits that stock did not have

Neither of these is an API break — no signature changed and nothing stops compiling. Both
reject inputs that stock 4.081 accepted, so an integration that was moving very large
messages should read this. Both are the *same* limit seen from its two ends, and they are
derived from one another rather than chosen separately.

**A Peer will not reassemble a message split into more than 65536 chunks.** Stock read
`splitPacketCount` off the wire with no upper bound and immediately allocated a pointer
array of that size, so a single 14-byte message could ask for 16 GiB and a single datagram
carrying 105 of them could ask for over a terabyte. 65536 is the keyspace of the 16-bit
split-packet id, which already bounded the number of simultaneously live channels, so both
split-packet limits now come from the same wire field. A datagram claiming more is dropped.
Stalled channels are also reaped on the connection's timeout rather than held until reset.

`MAXIMUM_SPLIT_PACKET_COUNT`, `Source/ReliabilityLayer.h`.

**A Peer will not send a message larger than 33,816,576 bytes.** `Send`, `Send( BitStream* )`
and `SendList` return 0 — their documented "bad input" answer — for anything larger, where
stock computed `length * 8` in `int` and had undefined behaviour from 268435456 bytes up.
`SendList` applies the limit to the concatenation of its blocks, since that is the message
the far end reassembles. Loopback sends are held to the same limit.

`MAXIMUM_MESSAGE_SIZE`, `Source/MTUSize.h`.

The value is the receive-side cap times the payload one datagram carries at 576, the lowest
MTU the handshake can settle on: `65536 x ( 576 - 28 - 9 - 23 )`. Deriving it at the *floor*
rather than at the MTU actually negotiated is what makes the two ends agree by construction —
a message this Peer will emit is one every Peer will accept, whichever MTU the two ends land
on and whichever target a broadcast reaches. A connection at MTU 1492 could physically carry
93.8 MiB, but sizing the limit to that would mean `Send` succeeding or failing on the outcome
of a handshake the caller never saw.

Neither limit is configurable, and neither is reachable by a conforming stock 4.081 sender in
practice: a real sender's chunk count is bounded by its own message size and MTU. If your
application does move messages above 33.8 MiB, it has to chunk them itself — which it was
already doing implicitly, and now finds out synchronously instead of by having the far end
drop every piece.

### What a System can make a Peer hold while Messages are incomplete

Stock held, for as long as a connection lived, whatever a System sent that could not yet be
delivered: chunks of split messages still being reassembled, and ordered or sequenced
messages waiting behind a missing one. Nothing bounded either. One System could open split
messages under every one of the 65,536 split-packet ids, or send ordered messages past a gap
it never filled, until the Peer ran out of memory. A System still in the middle of the
connection handshake could do the same.

Now:

- **A System that has not finished the handshake may send only its connection request.**
  Split chunks and ordered or sequenced messages from it are dropped before they are held.
  Stock's own connection request is one unsplit reliable message, so a stock 4.081 peer
  connects as before.
- **Each connection may make the Peer hold at most `RELIABILITY_LAYER_CONNECTION_BYTE_BUDGET`
  bytes**, twice `MAXIMUM_MESSAGE_SIZE` by default: enough to reassemble one largest message
  with room beside it. Past it, unreliable data is dropped and counted, and the connection
  stays open. Reliable or ordered data closes the connection instead, since it was already
  acknowledged and dropping it would lose it silently.
- **All connections together may make the Peer hold at most
  `RELIABILITY_LAYER_PEER_BYTE_BUDGET` bytes**, eight times `MAXIMUM_MESSAGE_SIZE` by default.
  Past it, the connection holding the most is closed.

A connection closed at a budget is reported to your application as `ID_CONNECTION_LOST`,
exactly as a timed-out one is, and plugins see `LCR_CONNECTION_LOST`. The far end is sent
`ID_DISCONNECTION_NOTIFICATION`. No new message id or reason was added.

Both budgets are `#ifndef` macros in `Source/RakNetDefines.h`; override them in
`RakNetDefinesOverrides.h` like any other. Lower them for many connections on constrained
hardware. Raise them if your application exchanges several near-maximum messages at once over
one connection, or across many.

**`RakNetStatistics` has four new fields**, appended after `packetlossTotal`, which changes the
struct's size and layout. Code that copies it field by field or reads it by name is
unaffected. Rebuild anything that shares the struct across a binary boundary.

| Field | Meaning |
|---|---|
| `bytesHeldForReassemblyAndOrdering` | What this connection holds against its budget now |
| `messagesDroppedOverConnectionBudget` | Unreliable data dropped at this connection's budget |
| `connectionsClosedOverConnectionBudget` | Connections closed at their own budget, over the Peer's lifetime |
| `connectionsClosedOverPeerBudget` | Connections closed at the Peer-wide budget, over the Peer's lifetime |

The last two are Peer-wide totals, the same in every connection's statistics, since a closed
connection has no statistics left to read. If a legitimate System trips a budget, these say
which one.

### What unconnected senders can make a Peer queue

Stock queued, with no limit, every datagram its receive thread read until the update thread
got to it, and every unconnected ping, pong, out-of-band message and advertisement until the
application called `Receive`. A sender that never connected could grow either queue while the
update thread was busy or the application was not draining.

Now:

- **At most `MAX_BUFFERED_RECEIVED_DATAGRAMS` datagrams (8192) wait for the update thread.**
  Past that the newest is dropped, as a full socket buffer would drop it, and the reliability
  layer recovers the same way.
- **At most `MAX_PENDING_OFFLINE_MESSAGES` Packets from unconnected Systems (1024) wait for
  `Receive`.** Past that a new unconnected ping, pong, out-of-band message or advertisement is
  dropped whole; a dropped ping is not answered. Messages from connected Systems are neither
  counted nor capped, since they were acknowledged on arrival: draining `Receive` every tick
  remains your application's job, and `Receive`'s header now says so.

Both are `#ifndef` macros in `Source/RakNetDefines.h`. `RakPeerInterface` gains two getters
that count the drops over the Peer's lifetime, `GetReceivedDatagramsDroppedAtCap` and
`GetOfflineMessagesDroppedAtCap`. They are pure virtual, so a class of your own that
implements `RakPeerInterface` has to add them.

A datagram your `SetIncomingDatagramEventHandler` callback rejects is now given back to the
receive pool. Stock leaked it, one buffer per rejected datagram; the callback's contract
already said the struct was valid only for the call, so nothing that honoured it changes.

### What a TCP client can make `TCPInterface` and `PacketizedTCP` hold

Stock read every TCP client as fast as it sent, into a queue only `Receive` drains; buffered
every `Send` to a client that never read; and let a `PacketizedTCP` client announce a message
of up to 4 GiB and buffer toward it. Any client, before the application had said a word to
it, could grow any of these without limit.

Now, each set at runtime on the interface:

- **`SetMaxIncomingBytesPerClient`, default 1 MiB.** At the cap the receive thread stops
  reading that client's socket until `Receive` drains it, so TCP flow control slows the
  sender. Nothing is dropped and nothing is closed; a client your application polls slowly
  just sends slowly. Counted by `GetIncomingBytesCapStallCount`.
- **`SetMaxOutgoingBytesPerClient`, default 2 × `MAXIMUM_MESSAGE_SIZE`.** A `Send` that would
  take a client's unsent bytes past the cap closes the connection instead, reported by
  `HasLostConnection`: the far end is not reading. A single `Send` larger than the cap does
  too. `GetOutgoingDataBufferSize` still tells you how close a client is. Counted by
  `GetOutgoingBytesCapCloseCount`.
- **`PacketizedTCP::SetMaxMessageLength`, default `MAXIMUM_MESSAGE_SIZE`.** A header
  announcing more closes the connection, reported by `HasLostConnection`, since a TCP stream
  cannot be re-framed after skipping a frame. Plugins see it as `LCR_CONNECTION_LOST`.
  Counted by `GetMessageLengthCapCloseCount`.

`PacketizedTCP` also stops losing a client that reconnects from the address it last
connected from before your application saw the old connection's loss. Stock leaked a buffer
and then dropped everything the reconnect sent. Each `Receive` and `Has...` call now takes
every pending connection event from `TCPInterface` rather than one of each, so data from a
connection whose new event was still queued is no longer dropped either.

A subclass of `PacketizedTCP` sees its protected `connections` map hold a `Connection`
rather than a `ByteQueue`, and `AddToConnectionList` and `RemoveFromConnectionList` are
replaced by `CountConnection`. `RemoteClient::SendOrBuffer` takes the cap and returns
whether it hit it.

### What a System can make a plugin hold

Several plugins kept, per remote message, an entry nothing bounded. Now each is capped by a
runtime setter with a counter getter, and prints once per plugin the first time the cap
bites:

- **`RelayPlugin::SetMaxNameLength`, default 256 bytes.** An add request with a longer name
  is answered `RPE_ADD_CLIENT_NOT_ALLOWED`, and a join request with a longer group name
  `RPE_JOIN_GROUP_FAILURE`. Counted by `GetNamesRefused`. A repeated add request from a
  participant now takes it out of its group first, so the group's other members see
  `RPE_USER_LEFT_ROOM`. Stock left its old copy in the group, which then never emptied: an
  add, join, add, join loop leaked a group per cycle, past the System's disconnect.
- **`UDPProxyCoordinator::SetMaxForwardingRequestsPerSystem`, default 8**, and
  **`SetMaxServerSelectionBitstreamBytes`, default 1024.** A forwarding request past either
  is answered `ID_UDP_PROXY_ALL_SERVERS_BUSY`. A request counts from when it arrives until it
  is answered, and after a success for its own `timeoutOnNoDataMS`. Counted by
  `GetForwardingRequestsRefused` and `GetServerSelectionBitstreamsRefused`.
- **`UDPProxyClient::SetMaxServersPerPingGroup`, default 64.** Servers past the cap are not
  pinged; counted by `GetPingServersTruncated`. A coordinator now has at most one group of
  pings in flight: a new request to ping replaces the live one, which first reports what it
  has, as a timeout would. Counted by `GetPingServerGroupsReplaced`. A request listing no
  server, or cut short, makes no group or only what it lists, and groups go with the
  coordinator's connection.
- **`TwoWayAuthentication::SetMaxNoncesPerSystem`, default 4.** A nonce request past the cap
  evicts that System's oldest nonce, so a System that retries is never locked out. Counted by
  `GetNoncesEvicted`. `Update` now frees every nonce older than `NONCE_TIMEOUT_MS` (10 s);
  stock freed at most one per call, after 5 s, so a flood outgrew it.
- **`NatTypeDetectionServer` and `NatTypeDetectionClient`** queue at most
  `MAX_BUFFERED_RECEIVED_DATAGRAMS` datagrams from their own sockets, the core's macro, and
  drop the newest past it. Counted by `GetReceivedDatagramsDroppedAtCap`.

`StatisticsHistoryPlugin::SetTrackConnections` now documents that tracking new connections
without removing lost ones grows the statistics on every reconnect.

Each plugin gained protected or `\internal` members, so a subclass or a binary that shares
the class layout has to be rebuilt.

## Plugins that act only on Solicited or Designated messages

Stock plugins believed any connected System that claimed to have set something up for
them. This fork acts on such a claim only if it answers a request the Peer made itself
(*Solicited*), or comes from a System the application named for that role (*Designated*).
Nothing is designated by default, and an undesignated System's claim is consumed and
dropped. No signature changed, so nothing here stops a build. The reasoning is
[ADR-0006](docs/adr/0006-entitlement-comes-from-solicitation-or-designation.md).

**`Router2`.** Stock moved a connection to `<sender's IP>:<any port>` for any connected
System that sent `ID_ROUTER_2_FORWARDING_ESTABLISHED` or `ID_ROUTER_2_REROUTED` naming
it. Any System could put itself in the middle of any other connection.

- As the source, a Peer now takes `ID_ROUTER_2_FORWARDING_ESTABLISHED` only from the
  router it asked, while that request is outstanding. Nothing to do.
- As the endpoint, a Peer takes `ID_ROUTER_2_REROUTED` only from a router designated with
  `Router2::AddIntermediary( SystemAddress )`, and moves a live connection only if it is
  already forwarded. A direct connection is never moved. A designation lapses when the
  router's connection closes, and must be made again if it reconnects.

New forwarded connections work without any designation, since the endpoint has nothing to
move until the source connects. What needs it is the endpoint surviving the loss of its
router: if your endpoints must stay connected when the source re-routes through another
System, call `AddIntermediary` on the endpoint for **every** System that may route to it,
including the first. An undesignated router's `ID_ROUTER_2_REROUTED` no longer reaches
`Receive`.

A Designated router may announce at most 16 forwarded connections that have not yet
connected. `Router2::SetMaxPendingForwardsPerIntermediary` changes the cap, and
`GetPendingForwardsRefused` counts what it dropped.

**`UDPProxyClient`.** Stock took every `ID_UDP_PROXY_GENERAL` message from any connected
System. A request to ping the proxy servers made the Peer ping every address it listed, a
forwarding notification made it ping an address of the sender's choosing and fire
`OnForwardingNotification`, and any System could fire any of the requester's result
callbacks with a proxy address of its choosing.

- As the requester, a Peer now takes a result only from the coordinator it asked, for a
  target it asked about, while that request is outstanding. A final result retires the
  request; so do `timeoutOnNoDataMS` and the coordinator's connection closing.
  At most `UDPProxyClient::MAX_OUTSTANDING_REQUESTS` (64) may be outstanding, and
  `RequestForwarding` returns `false` at the cap. A result later than `timeoutOnNoDataMS`
  fires no callback, so pass comfortably more than the coordinator's three-second ping wait.
- As the target, a Peer takes a forwarding notification only from a coordinator designated
  with `UDPProxyClient::AddCoordinator( SystemAddress )`. The same goes for the request to
  ping the proxy servers, which both ends receive. A designation lapses when the
  coordinator's connection closes, and must be made again if it reconnects.

**Call `AddCoordinator` on every Peer that may be a forwarding target.** Without it the
target never pings the proxy server, so a target behind NAT never opens its router to it and
is unreachable, and `OnForwardingNotification` never fires. The requester's own results need
no designation. An undesignated requester ignores the ping request, and the coordinator
goes on without its pings after its three-second ping timeout.

**`UDPProxyCoordinator`.** Stock forwarded from whatever source address a forwarding
request named, so any connected System could have a proxy server forward from an address
that was not its own. It also filed a ping reply under the `(source, target)` pair the reply
named, taking any sender other than the source as the target, so any connected System could
choose which proxy server another pair was given.

- The source is now always the requester, at its address as the coordinator sees it. The
  `sourceAddress` argument of `UDPProxyClient::RequestForwarding` is still sent and ignored,
  and every result names the requester's address as its source. Nothing to do, unless you
  requested forwarding *on behalf of* another System: that System now has to request it
  itself. A target named by address still need not be connected to the coordinator.
- A ping reply counts only from one of the pair's own ends, the two Systems the coordinator
  asked. Anything else is dropped. Nothing to do.

## Router2 reports a route it can't make

**`ID_ROUTER_2_FORWARDING_NO_PATH` reaches `Receive`.** `EstablishRouting` is documented to end
in `ID_ROUTER_2_FORWARDING_ESTABLISHED` or `ID_ROUTER_2_FORWARDING_NO_PATH`. Stock's Router2
consumed its own `NO_PATH` before `Receive` returned it, so the source never learned that no
router could reach the endpoint. If you guarded `EstablishRouting` with a timer of your own,
you can drop it.

**A router waits about 3 s for the punch, not 0.3 s.** Before forwarding, the router waits for
the source and endpoint to answer a punch through the new route, and they answer only inside
`Receive`. Stock gave up after eight pings plus 300 ms, so on a LAN any application that went
a third of a second without calling `Receive` lost the route. The margin is now 3 s. A route
that really can't be made is reported that much later, and a source with several routers
spends that much longer on a dead one before trying the next.

## Getters answer only for open connections

Stock read the connection records from your thread while the network thread was changing
them, with no lock. Here the network thread publishes a snapshot of the open connections at
the end of every update cycle, and the getters read that
([ADR-0007](docs/adr/0007-the-user-thread-reads-a-published-view.md)). An answer can be up
to one cycle old. The identity, state, ping and clock getters have moved so far:
`GetConnectionState`, `GetIndexFromSystemAddress`, `GetSystemAddressFromIndex`,
`GetGUIDFromIndex`, `GetSystemList`, `NumberOfConnections`, `GetConnectionList`,
`GetGuidFromSystemAddress`, `GetSystemAddressFromGuid`, `GetInternalID`, `GetExternalID`,
`GetMTUSize`, `GetTimeoutTime`, `GetAveragePing`, `GetLastPing`, `GetLowestPing` and
`GetClockDifferential`, as has the clock differential `Receive` subtracts from an
`ID_TIMESTAMP`. The statistics functions and `GetClientPublicKeyFromSystemAddress` ask
the network thread instead, and wait for its answer. `Connect` and `ConnectWithSocket`
take their "already connected" answer from the snapshot too, and the setters that change
open connections queue the change for the network thread. `GetSockets` and `GetSocket`
answer without asking the network thread. Six things change at runtime.
One return type changed, which stops a build only in the narrow cases in the last
paragraph.

A handler can trust the snapshot about the Message it is handling, as it could trust the
records in stock. When `Receive` hands out a Message from the network thread, the snapshot
reflects at least the cycle that produced it, so on `ID_NEW_INCOMING_CONNECTION` or
`ID_CONNECTION_REQUEST_ACCEPTED` the new connection is there, and on
`ID_DISCONNECTION_NOTIFICATION` or `ID_CONNECTION_LOST` it's gone. The snapshot can be newer,
so a connection that closed straight after opening can already be gone when its
`ID_NEW_INCOMING_CONNECTION` arrives. Its closing Message follows. A Message made on your
thread for a queued command can arrive before the command runs: `ID_ROUTER_2_REROUTED`
before the connection takes its new address, and the `ID_CONNECTION_LOST` that
`CloseConnection` queues without a notification before the connection closes.

**`GetConnectionState` reports a closed connection as `IS_NOT_CONNECTED`.** Stock returned
`IS_DISCONNECTED` for as long as the closed connection's storage still held its address or
RakNetGUID, until a new connection reused it. That's gone: nothing returns `IS_DISCONNECTED`
any more. The enumerator stays, as a hint you may never see. If you wait for a connection to
end, wait for "not `IS_CONNECTED` and not `IS_DISCONNECTING`", or for `IS_NOT_CONNECTED`, and
not for `IS_DISCONNECTED`.

**A closed connection has no index, address, RakNetGUID or per-connection value.** Stock
fell back to the leftovers of a closed connection when no open one matched, so these getters
went on answering for it after it closed. Now, once a connection is closed:

- `GetIndexFromSystemAddress` returns -1.
- `GetSystemAddressFromGuid` returns `UNASSIGNED_SYSTEM_ADDRESS`, and
  `GetGuidFromSystemAddress` returns `UNASSIGNED_RAKNET_GUID`.
- `GetInternalID` and `GetExternalID` return `UNASSIGNED_SYSTEM_ADDRESS`.
- `GetMTUSize` and `GetTimeoutTime` return the defaults, as for an address never connected.
- `GetStatistics( SystemAddress )` returns 0.
- `GetAveragePing`, `GetLastPing` and `GetLowestPing` return -1, and `GetClockDifferential`
  returns 0, whether you ask by address or by RakNetGUID.

If you need one of these after the connection closes, read it while the connection is open,
or when `ID_DISCONNECTION_NOTIFICATION` or `ID_CONNECTION_LOST` arrives, since
`Packet::systemAddress` and `Packet::guid` carry the identity.

**`GetStatistics`, `GetStatisticsList` and `GetClientPublicKeyFromSystemAddress` wait up
to one update cycle.** What they return is too large to copy into the snapshot every cycle,
so each call wakes the network thread and blocks until it answers, at the end of its current
cycle. That's up to one cycle per call, usually a few milliseconds. A loop that calls
`GetStatistics` once per connection waits once per connection, so call `GetStatisticsList`
once instead: it answers for every connection from one cycle. `StatisticsHistoryPlugin`
calls `GetStatisticsList` from its `Update`, so with it attached every `Receive` waits
once too. If the answer takes longer than `BLOCKING_QUERY_TIMEOUT_MS`
(1000 by default, in `RakNetDefines.h`), or the peer is shut down, the call fails:
`GetStatistics` returns 0 or `false`, `GetStatisticsList` returns empty lists, and
`GetClientPublicKeyFromSystemAddress` returns `false`. That includes
`GetStatistics( UNASSIGNED_SYSTEM_ADDRESS )`, which stock answered even after `Shutdown`.
Under `RAKPEER_USER_THREADED`, and from the callback `SetUserUpdateThread` installs, they
answer at once, since the calling thread is the one that runs the cycle.

**`ALREADY_CONNECTED_TO_ENDPOINT` is best-effort.** `Connect` and `ConnectWithSocket` check
the snapshot, so for up to one cycle the answer can lag. Just after a connection closes they
can still return `ALREADY_CONNECTED_TO_ENDPOINT`. Call again on a later cycle. Just after one
opens they can return `CONNECTION_ATTEMPT_STARTED`. The open connection carries on, and
`Receive` may get `ID_ALREADY_CONNECTED` for the redundant attempt.

**Setters reach open connections one cycle later.** `SetTimeoutTime`,
`SetSplitMessageProgressInterval`, `SetUnreliableTimeout` and `ApplyNetworkSimulator`
used to write into every open connection from your thread. Now they queue the change, and
the network thread applies it at the start of its next cycle, in order with your `Send`
calls. The Peer-wide value changes at once, so `GetTimeoutTime( UNASSIGNED_SYSTEM_ADDRESS )`,
`GetSplitMessageProgressInterval` and `IsNetworkSimulatorActive` answer with it straight
away, and a connection opened afterwards gets it too. `GetTimeoutTime` for an open
connection can return the old value for up to one cycle after `SetTimeoutTime`. If you need
to see it, poll until it shows. Called before `Startup`, the setters change only the
Peer-wide value, which is all they need to do.

**`GetSockets` and `GetSocket` don't block.** Stock queued a question for the network
thread and waited for the answer, which you could lean on to wait out the commands queued
before it. Now `Startup` publishes the bound sockets once it succeeds and `Shutdown` clears
them, so both answer at once, the same way under `RAKPEER_USER_THREADED`, where stock always
returned nothing. To see the effect of a command you queued, such as `ChangeSystemAddress`,
poll the getter that shows it. `GetSocket( UNASSIGNED_SYSTEM_ADDRESS )` is still the first
bound socket, and `GetSocket` for a connection answers from the snapshot. The pointers stay
valid until `Shutdown`, which frees the sockets. Stock's documentation promised a
reference-counted pointer that outlived `Shutdown`, but it never was one. `ReleaseSockets`
still only clears the vector. `GetMyBoundAddress` returns `UNASSIGNED_SYSTEM_ADDRESS` for an
index outside the bound sockets, where stock read past the end of the list.

**`GetGuidFromSystemAddress` returns `RakNetGUID`, not `const RakNetGUID&`.** The answer is a
copy out of the snapshot, so there's nothing for a reference to point at. Code that copies
the result, or binds it to a `const RakNetGUID&`, compiles unchanged. Code that takes its
address, or binds it to `auto&`, doesn't: copy it into a `RakNetGUID` instead. A class of
your own that implements `RakPeerInterface` has to change the return type of its override.

## Timestamped Messages arrive shifted

**`Receive` shifts a Timestamped Message's time instead of byte-swapping it.** For a Message
that starts with `ID_TIMESTAMP`, `Receive` subtracts the sender's clock differential from the
`RakNet::Time` after that byte, so the time reads against your clock. Stock 4.081 did too,
but it reversed that time's bytes before reading it, and `Read`, which converts from network
order, reversed them again. So on a little-endian host, which covers x86 and nearly all ARM,
you read the byte-swapped time minus the differential. Big-endian hosts and
`__BITSTREAM_NATIVE_END` builds were already right. If you worked around the garbage value,
remove the workaround. The fix is on the receiving side and the wire is unchanged, so a stock
peer on the other end is unaffected.

**NAT punchthrough's timed attempts now start.** `NatPunchthroughServer` tells each client
when to start punching in a Timestamped Message. On a little-endian host stock read that time
as one far in the future, so the attempt never started. It now starts at the time the server
named, on the client's clock.

**`MessageFilter` judges a Timestamped Message by its Message ID.** Stock skipped a
`RakNet::TimeMS` instead of a `RakNet::Time`, so it judged a byte of the time, which the
sender chooses. Now it reads the Message ID after the whole time, and your disallowed-message
callback gets that ID, where stock always passed `ID_TIMESTAMP`. A filtered System's
Timestamped Message that is too short to hold a Message ID is disallowed, and the callback
gets `ID_TIMESTAMP`. Stock dropped short ones from every System without calling back; an
unfiltered System's now pass through.

## Exceptions and out-of-memory

No signature changed here, but where you hook out-of-memory did. RakNet is
*exception-neutral* ([ADR-0004](docs/adr/0004-raknet-is-exception-neutral.md)): it builds
with exceptions disabled, never throws or catches, and fails the same way whether your build
has exceptions or not. Recoverable failures come back as return values. Allocation failure
and `std::mutex::lock` failure are fatal, and RakNet promises nothing after one. Catching an
exception thrown through RakNet is unsupported, including in the mixed build where RakNet is
compiled without exceptions and your application with them: the catch works but skips
RakNet's destructors, so its locks stay held. To log before the process dies, install
`std::set_new_handler`. The runtime calls it before throwing in both modes; on MSVC a
fail-fast skips `std::set_terminate`, so that is not a reliable hook. To control RakNet's
memory, replace the global `operator new`/`delete`. `SetMalloc` and friends redirect only
the `rakMalloc` family, and `_USE_RAK_MEMORY_OVERRIDE` is frozen and not recommended. In
stock 4.081, `SetNotifyOutOfMemory` fired wherever a `rakMalloc`-family call returned null
and was checked, and those checks covered every `RakString` and `RakWString` buffer. With
`_USE_RAK_MEMORY_OVERRIDE` at 1, `SetMalloc` also reached the `DataStructures` containers.
Here those are `std::string` and standard containers, which fail through `std::bad_alloc`
and which neither hook reaches, so `SetNotifyOutOfMemory` is left with packet and buffer
allocations in the core. If you relied on it as your OOM hook, move that code into a new
handler.
