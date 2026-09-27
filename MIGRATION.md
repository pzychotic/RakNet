# Migrating from stock RakNet 4.081

This fork modernized the core of RakNet 4.081 to C++17. Most of that is invisible to you.
Five things are not: they will either stop your build, or — in exactly one case — change
bytes on the wire.

This document is written for someone who has a working 4.081 integration and wants to move
it onto this fork. It is not a changelog. Each entry says what the API was, what it is now,
and what to do about it.

Two hardening limits sit outside that count. They break no API and stop no build, but they
reject inputs stock accepted, so they have a section of their own at the end:
[Limits that stock did not have](#limits-that-stock-did-not-have).

Some plugins no longer act on a remote System's word alone. They break no build either, but
an integration that relied on them can stop working at runtime, so they are listed in
[Plugins that act only on Solicited or Designated messages](#plugins-that-act-only-on-solicited-or-designated-messages).

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
  source and target it asked about, while that request is outstanding. A final result
  retires the request; so do `timeoutOnNoDataMS` and the coordinator's connection closing.
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
