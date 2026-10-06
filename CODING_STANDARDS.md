# Coding standards

Judgement calls a reviewer checks a diff against. The rules every change follows are in
`AGENTS.md` (exception-neutrality, conventions). `tools/lint-banned-apis.pl` and
clang-format already gate the mechanical rules in the pre-commit hook and CI.

## Comments that describe a set

- **Connect modes.** The comment on a `RemoteSystemStruct::ConnectMode` value in
  `Source/RakPeer.h` says which records enter that mode. A diff that adds a way into a mode
  extends its comment.
- **Mirrored API docs.** `Source/RakPeer.h` repeats the doc comments of
  `Source/RakPeerInterface.h`, and some functions in `Source/RakPeer.cpp` carry a third
  copy in the banner above them. A diff that changes one copy changes them all, to the
  same wording.

## Tests

- **Flags** a test sets on seeing a message are named `sawX` or `gotX`
  (`sawNotification`, `gotPong`).
- **Waking the network thread.** A test that needs an update cycle before the 10 ms timer
  or ack hold runs out wakes the thread, with a comment saying so, in one of two ways:
  - `GetStatistics`, which is answered on the network thread, so it returns once that
    cycle has run. Use it when the test waits for the cycle anyway, or polls in a loop.
  - A command at `IMMEDIATE_PRIORITY`, which wakes the thread and returns at once: a
    notifying `CloseConnection` with that `disconnectionNotificationPriority`, or, after a
    command that takes no priority such as a silent close, an `IMMEDIATE_PRIORITY` `Send`
    to the same System.
- **Waits** are a spin or poll bounded by a deadline (`ConnectionWaits::Expired`), with the
  budget a named constant whose comment says what it guards. A fixed sleep only lets traffic
  die down, and its comment says which traffic.
- **Ports** a test binds are named constants under a "Ports no other test uses." comment,
  and no other test file uses them.
