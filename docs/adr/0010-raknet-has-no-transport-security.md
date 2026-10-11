# RakNet has no transport security

Status: accepted.

Upstream offered encrypted, authenticated connections behind `LIBCAT_SECURITY`, built on
catid's libcat. libcat was never in this fork's tree, so the flag never compiled here and
every build anyone could make was insecure. The `#if` blocks could not be compiled, tested
or refactored, and changes around them were made blind.

**Decision.** RakNet has no transport security. The `LIBCAT_SECURITY` code,
`SecureHandshake.*` and the security API are removed, not kept frozen and not revived. An
application that needs confidentiality or authenticity of its traffic provides it outside
RakNet, for example with DTLS or an encrypted tunnel.

## Considered and rejected

- **Vendor libcat and support it.** libcat is unaudited and has not been maintained for a
  decade, and its handshake is a custom protocol. Supporting it would make this fork
  answerable for cryptography it cannot vouch for.
- **Keep it frozen.** It never compiled, so nobody depends on it at runtime, but every
  change to the code around it pays for it, and its defects can be neither confirmed nor
  fixed.

## Consequences

The wire is unchanged. A Peer still writes and reads the security fields of the
open-connection handshake as an insecure stock Peer does, and the three security message
IDs keep their names and values, because a stock Peer with security on still sends them.

`TwoWayAuthentication` hashes with SHA1 only, the branch every buildable configuration
already used.

`Connect` and `ConnectWithSocket` lose their `PublicKey*` parameter. Deleted overloads turn
a call written in the old argument order into a compile error, so it can't silently shift
every later argument by one slot.
