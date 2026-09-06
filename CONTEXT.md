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
