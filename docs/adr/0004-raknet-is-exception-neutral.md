# RakNet is exception-neutral

Status: accepted. Supersedes ADR-0002 and ADR-0003.

ADR-0002 said "nothing in `Source/` throws or catches". ADR-0003 carved out allocation
failure. Neither described what an embedder actually needs. Consoles, Emscripten, Unreal,
Godot and most large engines build with exceptions off. Upstream clang, for one, disables
them by default for the PlayStation targets. Such an embedder needs RakNet to build in
that configuration and to behave the same way in it as it does with exceptions on. Both
ADRs also stated as fact things the tree and the toolchains contradict: the guarded
`catch` in `RakThread.cpp`, 146 `std::mutex` lock sites under a rule that banned them, and
a claim about `-fno-exceptions` that holds for no toolchain.

**Decision.** RakNet is *exception-neutral*:

1. **It builds with exceptions disabled.** `Source/` compiles under GCC/Clang
   `-fno-exceptions` and MSVC `/EHs-c-`, and CI checks it. `Source/` contains no `throw`,
   `try` or `catch`. Library code reports failure by return value or out-parameter.
2. **It fails the same way in both modes.** No failure may be recoverable with exceptions
   enabled and fatal without them. A failure is either reported by return value in both
   modes, or it is fatal in both.
3. **Fatal paths are observable.** An embedder can log before the process dies, in either
   mode.
4. **The embedder controls memory.** Every allocation can be routed to the embedder's
   allocator.

Tests are exempt from all of this. Catch2 is built with exceptions.

## What is fatal

These are the only failures that may reach the standard library's throw. RakNet does not
report them, and it promises nothing after one:

- **Allocation failure from genuine exhaustion.** This is `std::bad_alloc` from `new`,
  containers, `std::string`, `std::function` and `std::make_unique`. Under
  `-fno-exceptions` the throw still comes from the prebuilt runtime and ends the process.
  With exceptions enabled it propagates. Hand-written recovery would be an untested path in
  one mode and dead code in the other. Chromium, LLVM and Unreal make the same choice.
- **`std::mutex::lock` failure.** Its `std::system_error` means misuse (a recursive lock or
  a destroyed mutex) or OS resource exhaustion. There is no recovery to write.

## What is input validation, not "allocation"

Size failures that come from bad input are covered by the bounding rule, not by
"allocation failure is fatal":

- `std::bad_array_new_length`. It derives from `bad_alloc`, but it signals a negative or
  overflowing count.
- `std::length_error` from `resize`, `reserve` or `insert` past `max_size()`.

**Any size or count driven by a System is bounded before allocating.** Bound it against
what is actually present, or against a documented maximum. That covers a length read from
the wire or from a file that sizes a `resize`, `reserve` or `new[]`, as `9e1a453` does in
`BitStream`. It equally covers objects allocated *per remote action*: connections, users,
groups, nonces and split-packet channels each need a cap. Without one, a System that
repeats a cheap message can make a Peer terminate.

## What is off-limits in `Source/`

These are APIs that report a recoverable failure by throwing:

- `std::stoi` and relatives. Use `std::from_chars`.
- `std::filesystem`'s throwing overloads. Use the `std::error_code` ones.
- `std::random_device`. ADR-0001's reasoning is unchanged.
- `std::regex`, and `.at()` on any container.
- `std::string`'s position-taking members: `substr`, `erase(pos, …)`, `compare(pos, …)`,
  `insert(pos, …)`, `replace(pos, …)`. Bound the position first, or use an iterator or
  `std::string_view` form.
- `std::expected::value()`, if `std::expected` is ever adopted. Check `has_value()` and
  use `*`/`->`.
- The `std::thread` constructor. It throws `std::system_error` for a failure the caller can
  handle, so thread creation goes through the native API.

## Null-returning allocators

`rakMalloc` and friends, a user-installed allocator, and `malloc` report exhaustion by
returning null. Dereferencing that null is undefined behaviour, not a fatal error, so the
result is always checked. Report the failure through `notifyOutOfMemory` and return failure,
which is the shape the existing sites already use. One caveat: after `_set_new_mode(1)` on
MSVC, `malloc` calls the new handler and can throw. An embedder can set that for the whole
application without RakNet knowing. The throw is then allocation failure, so it is fatal as
above.

## What RakNet promises the embedder

- **Catching an exception thrown through RakNet is unsupported.** Afterwards RakNet's state
  and locks are undefined. This includes the mixed build, where RakNet is compiled with
  `-fno-exceptions` and the application with exceptions. There the catch works, but it
  skips RakNet's frames without running their destructors, so locks stay held.
- **`std::set_new_handler` is the OOM hook.** The runtime calls it before throwing, in both
  modes. Use it to log, release a reserve, or abort deliberately. On MSVC a fail-fast skips
  `std::set_terminate`, so a terminate handler is not a reliable hook.
- **`SetNotifyOutOfMemory` covers only the null-returning `rakMalloc` paths**, eight
  sites. It is kept as public API. It is not an OOM hook for the library as a whole.
- **Replacing global `operator new`/`delete` is the way to control memory.** It covers the
  standard containers as well as RakNet's own `new`. `SetMalloc` and friends redirect only
  the `rakMalloc` family. `_USE_RAK_MEMORY_OVERRIDE` is frozen: kept and correct, but it is
  not the recommended hook, since it cannot reach containers.

## Toolchain facts this rests on

These were measured on MSVC 19.51 or read from the libstdc++, libc++ and MSVC STL sources.

- `-fno-exceptions` rejects the `throw`, `try` and `catch` keywords. Calling a throwing
  standard-library API still compiles. That is why enforcement needs a lint for banned APIs
  as well as a compile job.
- libstdc++, and MSVC's `operator new` and `_Xout_of_range`, throw from the prebuilt
  runtime. With no handler, the process ends: MSVC fail-fasts with `0xC0000409`, and GCC
  and Clang call `std::terminate`.
- MSVC with `/EHs-c- /D_HAS_EXCEPTIONS=0` builds `Source/` cleanly, including a bare
  `throw`. It diagnoses nothing, so it is only a best-effort check. Microsoft does not
  support `_HAS_EXCEPTIONS=0`. The GCC cell is the real gate.
- Before LLVM 18, libc++ built without exceptions could return null from a throwing `new`.
  That is undefined behaviour, not termination, and it is not supported.

## Consequences

The existing `notifyOutOfMemory` and `std::nothrow` sites stay. New code does not add
`std::nothrow` paths for fixed-size objects, and a missing one is not a defect.

`RakThread` moves off `std::thread` to `pthread_create` / `_beginthreadex`. That removes
the tree's only `catch` and makes a failed thread creation return an error in both modes.
Stack size and priority can then be set at creation, which `std::thread` cannot do. A
console port adds its own branch.

Code comments in `Source/` that cite ADR-0002 remain correct in substance. They are
repointed here as the files are touched.
