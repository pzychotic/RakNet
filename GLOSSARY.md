# RakNet

A UDP networking library for games. This context covers the transport layer: the Peers
that connect to each other, how they are named, how those names are exchanged, and what they
send one another. It also covers `TCPInterface`, whose connections need not have a Peer at
either end.

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
What one end of a connection holds about the far end for the life of that one connection,
from the first handshake step until the connection closes. When a later connection is
made, even to the same far end, it gets a new connection record. A Peer holds a bounded
number of them, and at most one open connection record per System; a `TCPInterface` holds
a bounded number of them too.
_Avoid_: Slot, remote system entry, remote client

**Connect attempt**:
A request to open a connection, from the moment it is accepted until it ends exactly once,
completed or failed. While it runs it holds a connection record that is not yet open.
Stopping waits for every connect attempt to end, and asks each to end early.
_Avoid_: Pending connection, connection thread

**Closing connection record**:
A connection record whose connection is ending. Either this Peer asked to close it and is
waiting for the System to acknowledge, or the System asked and this Peer is acknowledging.
Asking to close it again never stops this Peer acknowledging a close the System asked for.
Asking to close it silently ends this Peer's wait for the System to acknowledge its own.
_Avoid_: Disconnecting connection, half-closed connection

**Update cycle**:
One pass of a Peer's network work: it reads what has arrived, applies the commands the
application has queued, advances every connection, sends what is due, and ends by
publishing a new view. A Peer runs one after another, on its own thread or when the
application calls for one.
_Avoid_: Tick, frame, update (unqualified)

**Published view**:
The snapshot a Peer makes at the end of each update cycle of every open connection record,
and of what the Peer has learned about itself from them, such as its external address.
Every question the application asks about Systems, or about what Systems have told the
Peer, is answered from the latest one. So an answer is coherent but may be up to one update
cycle old.
_Avoid_: Cache, mirror, copy

**External address**:
The address a System sees this Peer at, as that System reports it during the handshake.
Each connection has its own. The Peer's external address, unqualified, is the first one
any System reported after `Startup`, and the Peer has none once it shuts down.
_Avoid_: External ID, public IP

**Half-open System**:
A System that has begun the connection handshake but not completed it. It has a
connection record but is not yet connected, and may send nothing but its connection
request. It is dropped if the handshake does not finish in time.
_Avoid_: Unverified sender, pending connection

**Refused System**:
A System whose connection request this Peer turned down, for a wrong password or a missing
or invalid key. Its connection record lives only long enough to deliver the reason, and
never blocks a new attempt from the same address.
_Avoid_: Rejected system, denied connection

**Designated System**:
A connected System the application has named as entitled to act in a role for this Peer,
such as its proxy coordinator or an Intermediary it accepts reroutes from. It is named by the
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

**Timestamped Message**:
A Message that begins with `ID_TIMESTAMP` followed by a time on the sender's clock. The
receiving Peer rewrites that time onto its own clock before the application or any plugin
sees the Message, so a receiver can compare it directly with its own time. The rest of the
Message is untouched. Its Message ID is the one after the time; `ID_TIMESTAMP` marks the
prefix and is not the Message's ID.
_Avoid_: Timestamp packet, timed message

**Offline datagram**:
A single datagram exchanged with a System that has no connection record: a ping, a pong,
an advertisement or out-of-band data. It is not a Message: it never goes through `Send`,
is never split, and carries no delivery guarantee, so a Peer may drop it freely.
_Avoid_: Unconnected message, offline message

**Source**, **Intermediary**, **Endpoint**:
The three roles in a forwarded connection. The Source asks for a route to the Endpoint, the
Intermediary relays every datagram between them, and the Endpoint is the System being
reached, which asked for nothing. Each is a Peer connected to the Intermediary. Endpoint
names this role only; it is not a synonym for System.
_Avoid_: Router (except for the plugin itself), sender, relay, destination
