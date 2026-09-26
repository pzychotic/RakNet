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
