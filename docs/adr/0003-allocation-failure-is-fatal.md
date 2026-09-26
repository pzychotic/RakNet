# Allocation failure is fatal

Status: superseded by ADR-0004 (RakNet is exception-neutral). Amended ADR-0002.

ADR-0002 took two rules as one. The first is that nothing in `Source/` throws or catches,
so a `-fno-exceptions` build compiles and no exception crosses a `RAK_DLL_EXPORT`
boundary. The second is that nothing in `Source/` may call an API that *could* throw,
and it listed "anything allocating without `std::nothrow`" among those. The first rule
is what those targets need. The second costs a hand-written error path at every
container operation and buys nothing on the builds it was meant for. Under
`-fno-exceptions`, a failed `new` or a failed `std::vector` growth already terminates,
whatever the calling code does. Recovery is possible only in builds that have
exceptions, and there it means error paths nobody exercises. `b5e2a24` had to make one of
them reachable before it could be tested at all.

**Decision.** Allocation failure is treated as fatal. Standard-library APIs whose only
exception is `std::bad_alloc` may be used in `Source/`: containers, `std::string`,
`std::function`, `std::make_unique`, plain `new`. The rest of ADR-0002 stands. Nothing
in `Source/` throws or catches, and any API that throws for a reason *other* than
allocation is still off-limits.

## Where the line falls

Still off-limits, because their failures are real and recoverable:

- `std::stoi` and relatives. Use `std::from_chars`.
- `std::filesystem`'s throwing overloads. Use the `std::error_code` ones.
- `std::random_device`. ADR-0001's reasoning is unchanged.
- The `std::thread` constructor and `std::mutex::lock`, which throw `std::system_error`.
- `std::regex`, `.at()`, and anything else that reports bad input by throwing.

**Allocation sizes taken from outside the process are not covered by this ADR.** When a
length read from the wire or from a file drives a `resize`, `reserve` or `new[]`, the
request must be bounded against what is actually present, or against a documented
maximum, *before* allocating. Otherwise any System can make a Peer terminate by sending
one number. That is input validation, and it stays fully in scope. `9e1a453` (bounding
the `std::string` resize in `BitStream`) is the pattern.

APIs that report exhaustion by returning null are not covered either: `rakMalloc` and
friends, a user-installed allocator, or `malloc`. Dereferencing that null is undefined
behaviour, not a fatal error, so the result is still checked. The existing
`notifyOutOfMemory` + return-failure paths are the right shape for this.

## Consequences

The ~dozen existing `notifyOutOfMemory` and `std::nothrow` sites stay. `SetNotifyOutOfMemory`
is public API, and a user allocator may rely on it to log before aborting. New code does
not add `std::nothrow` paths, and a missing one is no longer a defect.

In a build with exceptions, a `std::bad_alloc` from `Source/` now propagates to the
application, possibly mid-update and with RakNet's state partly modified, or reaches
`std::terminate` when it escapes a RakNet thread. That is the accepted outcome. RakNet
promises nothing after allocation failure.

As before, this is a convention: the build sets no `-fno-exceptions`.
