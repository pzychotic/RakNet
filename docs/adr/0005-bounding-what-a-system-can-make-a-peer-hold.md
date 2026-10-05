# Bounding what a System can make a Peer hold

Status: accepted. Refines the bounding rule in ADR-0004.

ADR-0004 says every size or count a System drives is bounded before allocating, and that
objects created per remote action each need a cap. It does not say *whose* cap, or what
happens when a System reaches it. A trace of the tree found the reliability layer's split
channels and ordering heaps, several plugin tables and the receive queues growing without
limit, and showed that the obvious answer — refuse the newest arrival — is wrong in two
different ways depending on where it is applied.

**Decision.**

1. **Caps are per System.** State a System owns (its split channels, its nonces, its
   forwarding requests) is capped per System. Shared state (relay rooms, proxy tables) is
   capped by how much each System may *create*. A global-only cap is not enough: one
   System fills it and every other System is locked out, which trades a memory attack for
   a denial-of-service attack.
2. **A chain of caps is a bound.** A per-System cap times the maximum number of Systems
   is acceptable when every link is a documented maximum and the product is survivable
   at the default settings. Where it is not — the reliability layer, since one conforming
   Message may be `MAXIMUM_MESSAGE_SIZE` — a Peer-wide budget sits on top of the
   per-System one.
3. **At a cap, refuse where the protocol allows dropping; close where it does not.**
   Offline datagrams, unreliable and sequenced data, and plugin requests are refused:
   dropped, with the protocol's existing failure reply where it has one. A reliable
   Message has been ACKed by the time it is buffered, and a TCP stream cannot be
   re-framed after skipping a frame, so there refusing would silently break a delivery
   guarantee. The connection is closed instead. Evicting the oldest entry is reserved for
   soft state that expires anyway, where refusing would break a legitimate System that
   retries.
4. **At the Peer-wide budget, the heaviest connection is closed** — the one holding the
   most buffered bytes — not the one whose datagram arrived last. Otherwise an attacker
   at the budget makes every legitimate large Message fail.
5. **No System is trusted.** A System the application chose, such as a proxy
   coordinator, gets a generous cap, not an exemption. Whether a System is *entitled* to
   send a message is a separate question from how much it may make a Peer hold.
6. **Half-open Systems hold nothing.** Before its handshake completes, a System may send
   only its single-datagram connection request. Split chunks and anything else are
   refused before reassembly and ordering, so half-open state needs no cap of its own.
7. **Queues the application drains are the application's responsibility** where
   dropping would lose ACKed data: Messages waiting in `Receive`'s queue are not capped,
   and the header says so. Queues of offline datagrams and raw received datagrams are
   capped, since dropping there is what a full socket buffer does anyway.

**Configuration and visibility.** Core caps are `#ifndef` macros in `RakNetDefines.h`,
overridable per build. Plugin caps are runtime `SetMaxX()` setters with a documented
default. Every cap has a counter (in `RakNetStatistics` for the core, a getter on the
plugin) and a `RAKNET_DEBUG_PRINTF`, so an embedder whose legitimate traffic hits a
default can find out why. A cap-driven close is reported locally as an ordinary lost
connection and sends the remote end a disconnection notification. No new lost-connection
reason and no new callback API.

**Clarified in applying it** (reliability layer):

- *Point 3.* `RELIABLE_SEQUENCED` is reliable data for this purpose, so it closes the
  connection rather than being dropped. It is acknowledged and not resent, so dropping the newest
  one loses it. "Sequenced" in point 3 means `UNRELIABLE_SEQUENCED`.
- *Point 6.* "Refused" means refused before reassembly or ordering could hold it. A
  Half-open System's unsplit, unordered message holds nothing, so it is passed up. There
  the connection request is parsed, and anything else closes the connection and bans the
  address, as before.

**Clarified in applying it** (receive queues):

- *Counters.* A counter for a cap that belongs to no connection is a getter on
  `RakPeerInterface`, not a field of `RakNetStatistics`. Offline datagrams are dropped
  while no connection may exist, and then there are no statistics to read.
- *Debug print.* "A `RAKNET_DEBUG_PRINTF`" means one per cap per Peer, on the first drop,
  as the reliability layer does. A flood should not flood the console as well; the counter
  carries the rest.
- *Point 7.* Dropping an offline datagram drops it whole, so a dropped unconnected ping is
  not answered, just as it would not be if a full socket buffer had dropped it.

**Clarified in applying it** (TCP):

- *Point 7.* TCP has flow control, so a queue the application drains can be capped without
  losing anything: at the cap `TCPInterface` stops reading the client's socket, and the
  sender stalls. That is neither refusing nor closing, and it is the reason TCP's incoming
  queue is capped where `Receive`'s Message queue is not.
- *Point 3.* Bytes waiting to go to a client that does not read are the other end's doing,
  and dropping part of a TCP stream breaks it, so at that cap the connection is closed.
- *Configuration.* `TCPInterface` and `PacketizedTCP` are not the core's reliability layer:
  their caps are runtime setters with a documented default, like a plugin's, and their
  counters are getters on the class.
- *Close reporting.* A cap-driven close is the TCP close itself, which the remote end sees
  as its connection ending; locally it is reported by `HasLostConnection`.

**Clarified in applying it** (plugins):

- *Point 1.* Where a table is shared, capping what each System may create can mean
  restoring an invariant rather than adding a number. `RelayPlugin` holds one participant
  per System in at most one group, and a group lives only while it has a participant, so
  fixing the replace that left a stale copy behind bounds its groups by the connection count.
  What is left to cap is the length of the names.
- *Point 3.* "Refuse" can also mean *replace* or *truncate* when the message is a request for
  work, not data. `UDPProxyClient` keeps one ping group per coordinator, the newest, and pings
  at most `SetMaxServersPerPingGroup` of the servers a request lists.
- *Point 5.* A Designated coordinator can still send malformed ping requests. It gets the
  same caps as anyone, which is what "a generous cap, not an exemption" means here.
- *Configuration.* `NatTypeDetectionServer` and `NatTypeDetectionClient` queue raw
  datagrams, exactly as the core's receive queue does, so they share its
  `MAX_BUFFERED_RECEIVED_DATAGRAMS` macro rather than get a setter of their own.
- *Debug print.* Once per plugin instance, on the first time the cap bites, as for the core.
- *Point 7.* A queue the application drains is capped when its entries are not ACKed data.
  `ThreadsafePacketLogger` refuses lines at the cap, and the next `Update` writes how many.

## Considered and rejected

- **Global caps only.** Simpler, but turns every cap into a lock-out lever.
- **Always refuse, never disconnect.** Breaks the Message guarantee — delivered whole or
  not at all — for reliable traffic, and desynchronises TCP framing.
- **Disconnect as the default response to any cap.** Kicking a System is policy. For
  data the protocol may drop, dropping is enough, and the embedder can add policy on top.
- **A separate cap on live split channels.** Each channel's pointer array is charged
  against the per-connection byte budget, which bounds the channel count as a side effect.

## Consequences

Legitimate traffic can now be refused or disconnected at a default that is too small for
it. That is the trade: the defaults are sized for a conforming System, and the counters
make the cause visible. Embedders running many connections on constrained hardware lower
the budgets; embedders exchanging many large Messages raise them.
