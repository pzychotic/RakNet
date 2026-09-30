# RakNet

A UDP networking library for games. This context covers the transport layer: the Peers
that connect to each other, how they are named, how those names are exchanged, and what they
send one another.

## Language

**Peer**:
One running copy of the library, created by `RakPeerInterface::GetInstance()`. A Peer
owns exactly one RakNetGUID for its lifetime.
_Avoid_: Node, client, instance

**System**:
A Peer as seen from another Peer — the far side of a connection, held in a connection
record. The same running program is a Peer to itself and a System to everyone else.
_Avoid_: Remote peer, host, endpoint

**Connection record**:
What a Peer holds about one System for the life of one connection to it, from the first
handshake step until the connection closes. When a later connection is made, even to the
same System, it gets a new connection record. A Peer holds a bounded number of them.
_Avoid_: Slot, remote system entry

**Update cycle**:
One pass of a Peer's network work: it reads what has arrived, applies the commands the
application has queued, advances every connection, sends what is due, and ends by
publishing a new view. A Peer runs one after another, on its own thread or when the
application calls for one.
_Avoid_: Tick, frame, update (unqualified)

**Published view**:
The snapshot of every open connection record that a Peer makes at the end of each update
cycle. Every question the application asks about Systems is answered from the latest one.
So an answer is coherent but may be up to one update cycle old.
_Avoid_: Cache, mirror, copy

**Half-open System**:
A System that has begun the connection handshake but not completed it. It has a
connection record but is not yet connected, and may send nothing but its connection
request. It is dropped if the handshake does not finish in time.
_Avoid_: Unverified sender, pending connection

**Designated System**:
A connected System the application has named as entitled to act in a role for this Peer,
such as its proxy coordinator or a router it accepts reroutes from. It is named by the
address its connection was made at, never by its RakNetGUID, and loses the role when that
connection closes.
_Avoid_: Trusted system, authorised system

**Solicited**:
Said of a Message that answers something this Peer started itself, such as a reply to a
request it sent and has not yet seen answered. A Message that is neither solicited nor
from a Designated System entitles its sender to nothing.
_Avoid_: Expected, requested

**RakNetGUID**:
The 64-bit name a Peer answers to, chosen once at construction and stable across the
address changes a System may go through. It is:

- **Unique** among all Peers that could meet — different machines, different processes,
  cold restarts.
- **Totally ordered**, and both ends of a connection agree on the order. Some protocols
  elect roles by comparing two RakNetGUIDs and relying on the two Peers reaching
  opposite conclusions.
- **Not a secret.** A RakNetGUID is broadcast in plaintext and may be claimed by anyone.
  Never treat holding one as proof of anything.

_Avoid_: Peer ID, GUID (unqualified), identity, token

**Message**:
The unit an application hands to `Send` and gets back from `Receive`. A Message is
delivered whole or not at all: in transit it may be split across many datagrams and is
reassembled before the receiving application sees it, so the number of datagrams a Message
took to arrive is invisible to both ends. Its size is bounded — a Peer refuses to emit one
larger than it can guarantee any other Peer will accept.
_Avoid_: Packet (which is the struct `Receive` returns, and separately a chunk of a split
Message — it means at least three things in this codebase already)

**Offline datagram**:
A single datagram exchanged with a System that has no connection record: a ping, a pong,
an advertisement or out-of-band data. It is not a Message: it never goes through `Send`,
is never split, and carries no delivery guarantee, so a Peer may drop it freely.
_Avoid_: Unconnected message, offline message
