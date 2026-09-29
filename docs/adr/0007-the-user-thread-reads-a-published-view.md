# The user thread reads a published view, never the connection records

Status: accepted.

RakPeer runs two threads: the network thread, which runs `RunUpdateCycle`, and the user's
thread, which calls `Receive`, the getters and the setters. Plugins run on the user's thread,
inside `Receive`. Upstream never stated which of these threads may touch the connection
records. Its code assumed both could: the lookups take a `calledFromNetworkThread` flag, and
the user-thread branch skips the hash chain and scans the array linearly, under the comment
"remoteSystemList in user and network thread". That made the reads *stale-but-safe* in
intent, but not in fact. No mutex guards the connection records, so every user-thread read
is a data race. A getter can see a connection record half built, with `isActive` already set
while the previous connection's `connectMode`, RakNetGUID or socket is still in place, and it
can tear the ping and statistics fields the network thread writes on every datagram.

**Decision.**

1. **Getters may be called from any thread, and each answer is a snapshot.** An answer is
   never undefined behaviour and never mixes two connection records or two moments. It can
   be up to one update cycle old, and it can describe a connection that closes right after
   the call returns.
2. **There is no read-your-writes guarantee.** `CloseConnection`, `Connect` and the
   setters are buffered commands. A getter called straight after one of them may show the
   state from before it, until the network thread's next cycle has run.
3. **Only open connection records are authoritative.** A getter answers from connection
   records that are open. What a closed connection left in storage is not a connection
   record. `IS_DISCONNECTED` stays in the API as a hint that may fail to appear. It is not
   a state anyone can rely on seeing.
4. **The user thread never reads or writes a connection record.** Once per cycle, the
   network thread copies what the getters need from each open connection record into a
   **published view**, under a mutex. User-thread getters read only the view.
   - Data too large to copy every cycle (`RakNetStatistics`, a client's public key) is a
     **blocking query**, like `GetSockets`: the user thread posts a request and the network
     thread answers at the end of its cycle.
   - Setters that change a connection record become buffered commands, which the network
     thread applies at the start of its next cycle.
5. **Checks that gate an action are advisory on the user thread.** `Connect`'s "already
   connected" answer comes from the view and can be stale. The network thread checks again
   against the connection records when it acts, and its answer wins.
6. **The rule is enforced by structure plus a debug assertion.** Lookups that go straight
   to the connection records have no user-thread path. In debug builds they assert that the
   caller is inside `RunUpdateCycle`. That is a flag the cycle sets, not a thread id, so the
   assertion holds under `RAKPEER_USER_THREADED` too.

With `RAKPEER_USER_THREADED == 1` there is no network thread and so no race. The view is
still built, so both modes run the same getters.

## Considered and rejected

- **Make `isActive` a `std::atomic<bool>` and store it last with release.** That publishes
  a fully built connection record and does nothing else. It doesn't cover teardown (the
  same race in reverse), `connectMode` changes, or the ping and statistics fields.
- **One mutex for the connection records.** Getters would be exact, but the network thread
  writes ping and reliability fields on every datagram, so the lock would sit on the hot
  path. It buys little: an exact answer is stale as soon as the getter returns.
- **Declare the getters network-thread-only.** That contradicts what upstream built, and it
  would make every plugin that calls a getter from `Receive` non-conforming.
- **Overlay pending commands on the view to give read-your-writes.** Every getter would scan
  the command queue for a guarantee nobody has asked for.

## Consequences

The staleness is now a documented property. Code that polls `GetConnectionState` right after
`CloseConnection`, or `GetStatistics` in a tight loop, sees the old state or waits up to one
cycle, and that's by design. Each cycle copies a small record per open connection, bounded by
`maxConnections`. No MSVC tool proves the absence of a data race: a stress test in the suite
checks that answers are coherent on every platform, and a clang ThreadSanitizer job in CI
checks for the race itself.
