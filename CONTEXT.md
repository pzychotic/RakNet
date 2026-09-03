# RakNet

A UDP networking library for games. This context covers the transport layer: the Peers
that connect to each other, how they are named, and how those names are exchanged.

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
