# RakNet does not use exceptions

Status: accepted

`Source/` contains zero occurrences of `throw`, `try`, or `catch`. This has always been
true and was never written down, so it survived only by everyone happening to follow it —
and it silently invalidated a design decision already taken (see ADR-0001) before anyone
noticed the rule existed.

**Decision.** Library code reports failure by return value or out-parameter. Nothing in
`Source/` throws, catches, or requires exceptions to be enabled. Tests are exempt: Catch2
needs them.

## Consequences

The binding consequence is not about RakNet's own code, which is easy enough to write
this way. It is that **any standard-library API whose only failure channel is an exception
is off-limits in `Source/`**, however convenient. `std::random_device` is the worked
example: both its constructor and `operator()` may throw, there is no no-throw mode, and
so ADR-0001 calls the platform CSPRNG directly instead. Expect this to recur — `std::stoi`,
`std::filesystem`'s throwing overloads, and anything allocating without `std::nothrow` all
fall under it.

The cost is real: hand-written error returns instead of RAII-clean propagation, at every
call site. It is accepted because the alternative excludes the builds RakNet exists to
serve — game consoles and embedded targets routinely compile with `-fno-exceptions`, where
even *constructing* a throwing type fails to compile, and an exception escaping a
`RAK_DLL_EXPORT` boundary is not portable in any case.

This is a convention, not an enforced constraint: the build sets no `-fno-exceptions` or
`/EHsc-`. Enforcing it in the build would be a separate decision, and would need the test
target exempted.
