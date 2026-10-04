# A replaced callback has finished when its setter returns

Status: accepted.

A Peer calls two application callbacks from its own threads. `SetUserUpdateThread`'s
callback runs on the network thread at the top of each pass of its loop.
`SetIncomingDatagramEventHandler`'s handler runs on the receive threads, one per bound
socket, for every datagram. Upstream stored each as plain fields and said nothing about when
a new one takes effect. So the network thread could call the new function with the old data
pointer, the callback's first touch of its data raced the code that built it, and nothing
told an application when it could free what the old callback used.

**Decision.**

1. **A callback and its data are published together.** The network thread never sees a new
   function with an old data pointer. Everything the setting thread did before the setter
   happens before the first call of the new callback.
2. **When the setter returns, the old callback is not running and will not be called
   again.** If a call is in flight, the setter waits for it to return. An application can
   clear a callback and then destroy what it used.
3. **A callback may replace or clear itself.** Called from inside the callback it replaces,
   the setter swaps without waiting, and the new callback applies from the next call. That
   is the only case that skips the wait. Calling the other setter from a callback waits as
   usual.
4. **Calling a setter while holding something the old callback waits for deadlocks.** It is
   documented on both setters, not detected.
5. **Datagram handler calls on different sockets still overlap.** Each call holds a shared
   lock and the setter takes it exclusively. A Peer with no handler installed takes no lock
   per datagram. A datagram that arrives while a handler is being installed may miss it.

## Considered and rejected

- **Publication only (1 without 2).** It's cheaper, and the setter never blocks. But clearing
  a callback and then destroying its target is what people write, and the suite's own
  `UpdateThreadGate` does it. Without 2, that is a use-after-free.
- **Forbid calling a setter from its own callback.** In release builds that would be a
  silent deadlock, for a pattern as natural as a callback that removes itself.
- **One plain mutex around every datagram handler call.** It would make calls on different
  sockets take turns, which upstream never did.

## Consequences

Both setters can block for as long as the old callback runs. An application whose callback
waits on the thread that calls the setter has to release the callback first, as
`UpdateThreadGate` does.
