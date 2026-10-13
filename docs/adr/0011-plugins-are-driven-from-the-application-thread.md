# Plugins are driven from the application thread

Status: accepted.

A Peer calls into its plugins from two places. `Receive` runs every plugin's `Update`,
then hands each Message to `OnReceive` and the callbacks that Message triggers. A plugin
whose `UsesReliabilityLayer` returns true also gets the socket and reliability-layer
callbacks, which the update cycle calls on the network thread. Upstream never said which
threads may be inside `Receive`, or which may call a plugin's own API, at the same time.
Nothing in `Receive` stops two threads entering it, and `AttachPlugin` and `DetachPlugin`
change the plugin lists that `Receive` and the update cycle walk, with no lock. Router2
guarded its lists with three mutexes plus one per request, as if the application might
call `EstablishRouting` on one thread while another was in `Receive`. No other plugin did,
and those mutexes brought lock-order bugs of their own.

**Decision.**

1. **Calls to `Receive` must not overlap.** Any thread may call it, and a different thread
   may make the next call, but the application orders the calls so that one has returned
   before the next begins. That thread is the **application thread**.
2. **A plugin's public API is called only from the application thread.** A call such as
   `Router2::EstablishRouting` must not overlap `Receive`, or another call into the same
   plugin. A plugin needs no lock to keep its API and its `Receive` callbacks apart.
3. **`AttachPlugin` and `DetachPlugin` are application-thread calls.** A plugin whose
   `UsesReliabilityLayer` returns true is attached or detached only while the Peer is
   stopped, because the update cycle walks its list.
4. **Re-entry on the application thread is allowed.** A plugin callback may call back
   into the Peer or into a plugin, and that may call plugins again before it returns:
   `PushBackPacket` reaches `OnPushBackPacket` and `CloseConnection` reaches
   `OnClosedConnection`. Plugins are re-entrant. They are not called concurrently from
   the application side.
5. **A plugin that uses the reliability layer runs on two threads.** Its socket and
   reliability-layer callbacks run on the network thread, at the same time as its
   application-thread callbacks and API. It guards whatever the two share. `PacketLogger`
   is the one such plugin the library ships.
6. **Threads a plugin starts for itself are its own concern.** A plugin that opens its own
   sockets or runs its own thread, such as `NatTypeDetectionClient` or `UDPForwarder`,
   guards what that thread shares with the application thread.
7. **The rule is checked by debug assertions.** `Receive` asserts that no other thread is
   inside it, letting re-entry from the same thread pass. `AttachPlugin` and `DetachPlugin`
   assert that a plugin using the reliability layer is not attached or detached while the
   Peer is running. A plugin's own API is not checked.

The update cycle is never an application-thread call, even under `RAKPEER_USER_THREADED`
where the application's thread runs it. Whether it may overlap `Receive` is ADR-0007's
concern, not this one's.

## Considered and rejected

- **Let calls to `Receive` overlap.** Every plugin's `Update` and `OnReceive` would have to
  be safe against itself on another thread, for an embedder nobody has seen.
- **Fix `Receive` to one thread for the Peer's life.** Moving a game loop between job
  threads is common and harmless once the calls don't overlap.
- **Every plugin's API is thread-safe.** Every plugin would carry locks to guard against
  a use the application can avoid by ordering its calls, and every plugin would be one
  lock-order mistake from a hang. Router2 tried this and had such a mistake.
- **Each plugin states its own rule.** It hands the same unanswered question to every
  plugin header, and an application using several plugins would have to keep several
  rules.
- **Lock the network thread's plugin list so that a plugin using the reliability layer can
  be attached while the Peer runs.** The update cycle walks that list for every datagram,
  and nobody has asked to attach one at run time.
- **Check every plugin's public API.** Each entry point would need the same check, and
  `Receive` already catches the most likely mistake: two threads polling it.

## Consequences

An application that polls `Receive` from more than one thread at once, or calls a plugin
from a worker thread while its main loop is in `Receive`, is outside the contract, and a
debug build says so for the first case. Plugins that guard only against the application
thread can drop those locks. Router2's are of that kind. A plugin that uses the
reliability layer still needs locks for what its network-thread callbacks share with the
rest of it.
