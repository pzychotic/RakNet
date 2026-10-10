# A memory pool stays only where a benchmark shows it pays

Status: accepted.

Upstream gave RakNet a hand-rolled page allocator, `DataStructures::MemoryPool`, and used it
for every object made per message or per datagram: `InternalPacket`, its ref-counted
payload, datagram history nodes, `Packet`, and the remote-system index. Nobody has measured
since whether it beats the platform allocator, and it costs a second allocation path with
its own failure handling, its own `_DISABLE_MEMORY_POOL` build, and its own `file, line`
plumbing.

**Decision.** A pool is kept only where a benchmark shows it pays. Every other one becomes
plain `new`/`delete` (or `std::unique_ptr`), whose failure is fatal under ADR-0004. Each
pool is judged on its own numbers, not as a set: one hot pool earning its keep does not keep
the others.

- **The benchmark** is a Catch2 `BENCHMARK` under the hidden `[.bench]` tag, so ctest and CI
  never run it. Two Peers in one process over loopback. One sends 200 000 reliable-ordered
  messages and the time until the last is received is measured. There are three cases:
  32-byte messages, 1 KB messages, and messages large enough to be split. Release build,
  median of 5 runs.
- **The threshold.** A pool is replaced if the replacement is no more than 5% slower on every
  case. Otherwise its owner gets a `std::pmr::unsynchronized_pool_resource` in place of the
  hand-rolled pool.
- **Cold pools are not measured.** A pool touched once per connection, like the
  remote-system index, is replaced without a benchmark.

## Considered and rejected

- **Keep `MemoryPool` and only tidy it.** It is the last reason `Packet` slots have a
  recoverable null path, and nothing shows the speed it was kept for.
- **Replace every pool with `new` unmeasured.** `InternalPacket` is made once per message.
  A regression there is a regression in RakNet's throughput, and it would go unnoticed.
- **One verdict for all pools.** It lets the hottest pool decide for the coldest.

## Consequences

Where a pool is replaced, failing to allocate its object is fatal, like any other
allocation under ADR-0004. For `Packet` that means only the payload's `rakMalloc_Ex` still
reports failure through `notifyOutOfMemory`; the slot no longer can.

The benchmark stays in the tree. Any later change to how per-message objects are allocated
is measured the same way.

Verdicts:

- Remote-system index: replace, cold.
- `Packet`, `InternalPacket`, ref-counted payload, datagram history: replace, measured.

Each pool was replaced on its own in a build of its own, and a fifth build replaced all
four. Windows 11, MSVC Release, an idle machine, the six builds run in turn four times.
Each figure is the median of the four runs' medians, in megacycles of process CPU work,
against the build that still had every pool:

| Replaced          | 32 B          | 1 KB          | 4 KB, split    |
|-------------------|---------------|---------------|----------------|
| none (baseline)   | 1483          | 6176          | 19307          |
| `Packet`          | 1417 (−4.4%)  | 6206 (+0.5%)  | 19226 (−0.4%)  |
| `InternalPacket`  | 1513 (+2.1%)  | 6240 (+1.0%)  | 19294 (−0.1%)  |
| ref-counted data  | 1470 (−0.8%)  | 6244 (+1.1%)  | 19375 (+0.4%)  |
| datagram history  | 1489 (+0.4%)  | 6311 (+2.2%)  | 19567 (+1.3%)  |
| all four          | 1543 (+4.1%)  | 6355 (+2.9%)  | 19634 (+1.7%)  |

Between runs of one build the 32 B case spread up to 20%, the 1 KB case 2.5% and the 4 KB
case 2.3%. So the 32 B column cannot resolve 5% on its own; no pool's median there was more
than 4.4% slower, and the 1 KB and 4 KB columns, which can, put every pool under 2.3%.
`Packet` is made with `OP_NEW`, the same way the plugins that hand `Packet`s to
`DeallocatePacket` make theirs. Wall time moved less than 5% for every pool on every case: the medians were
785-800 ms, 898-927 ms and 2992-3227 ms.
