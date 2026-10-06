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
- **Waking the network thread.** `GetStatistics` is answered on the network thread, so
  calling it wakes the thread now. Tests use it to make an update cycle come before the
  10 ms ack hold runs out, with a comment saying so.
- **Waits** are a spin or poll bounded by a deadline (`ConnectionWaits::Expired`), with the
  budget a named constant whose comment says what it guards. A fixed sleep only lets traffic
  die down, and its comment says which traffic.
- **Ports** a test binds are named constants under a "Ports no other test uses." comment,
  and no other test file uses them.
