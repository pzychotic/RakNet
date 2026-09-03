# Unique identifiers draw from the platform CSPRNG, through one seam

Status: accepted

RakNet had no random number source worth the name: the only RNG in the tree is the
Mersenne Twister in `Source/Rand.cpp`, and the `LIBCAT_SECURITY` crypto path it appears
to offer is unusable — `Source/SecureHandshake.cpp` includes `cat/src/...` headers from a
`cat/` directory that does not exist in this fork, so the flag has not compiled for as
long as it has been here. Identifiers were therefore built from the clock.

**Decision.** Values that must be unique — RakNetGUIDs today — draw their bytes from the
operating system's CSPRNG through a single internal seam, `RakNet::FillRandomBytes` in
`Source/PlatformRandom.h`: `BCryptGenRandom` on Windows, `/dev/urandom` elsewhere.
`<random>` engines remain available for simulation and jitter, but never for naming a
Peer. Clock values are never an entropy source.

## Considered options

`std::random_device` was the obvious choice and is rejected for one reason: it reports
failure only by throwing, and RakNet uses no exceptions (ADR-0002). Its determinism on
MinGW before GCC 9.2 would also have needed a documented caveat, which the platform call
removes entirely rather than papering over.

`LIBCAT_SECURITY` is not an option at all until libcat is vendored, and making the naming
of Peers depend on an optional subsystem would be wrong even then.

## Consequences

The uniqueness of a RakNetGUID is a contract other code may rely on. Its
*unpredictability* is a property of this implementation and is deliberately **not**
promised — see the `RakNetGUID` entry in `CONTEXT.md`. The distinction is load-bearing:
several paths act on a RakNetGUID supplied by a System (`Router2::OnRerouted` →
`RakPeer::ChangeSystemAddress` re-points a connection found by RakNetGUID alone;
`NatPunchthroughServer` discloses a Peer's addresses to whoever names its RakNetGUID), so
an unguessable value is worth having as defence in depth — but writing "unguessable" into
the contract would invite someone to build authorization on it, turning a latent weakness
into a real one. Harden the implementation; do not promise the property.

Adding `bcrypt.lib` to `RAKNET_LIBRARY_LIBS` is a new Windows link dependency.
