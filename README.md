# PoolAllocator

A header-only, fixed-size block allocator in C++20 — **O(1) allocate, O(1) deallocate, zero syscalls on the hot path.**

Measured **~10x faster than global `new`/`delete`** for a 16-byte order object, with `deallocate` running in **under half a nanosecond** on an Apple M1 Pro.

Built as part of a low-latency systems roadmap: the kind of allocator that sits underneath an order book or a market-data feed handler, where a single `malloc` taking a lock is a latency event you can't explain to a trading desk.

---

## Results

`Release` build (`-O3`), Apple M1 Pro, Apple clang 21.0.0, arm64, macOS 26.5.
100,000 allocations per trial × 51 trials, **median** reported.

| Operation           |    ns/op |  M ops/sec | vs. `new`/`delete` |
| :------------------ | -------: | ---------: | -----------------: |
| `::operator new`    |     7.78 |     128.55 |               1.0x |
| `::operator delete` |     9.02 |     110.91 |               1.0x |
| `pool.allocate()`   | **1.21** | **827.30** |          **6.44x** |
| `pool.deallocate()` | **0.46** |   **2170** |         **19.57x** |

**Combined allocate + free: 10.06x speedup.**

Run-to-run spread across repeated invocations was 6.34–6.44x (allocate), 19.34–19.75x (deallocate), 9.95–10.06x (combined) — the numbers are stable, not a lucky sample.

<details>
<summary>Raw benchmark output</summary>

```
PoolAllocator benchmark
  payload: Order{int,double} = 16 bytes
  allocations per trial: 100000
  trials: 51 (median reported)

                       ns/op      M ops/sec     median ms/trial
  new   allocate:       7.78       128.55            0.78
  new   deallocate:     9.02       110.91            0.90
  pool  allocate:       1.21       827.30            0.12
  pool  deallocate:     0.46      2170.00            0.05

  speedup (allocate):    6.44x
  speedup (deallocate):  19.57x
  speedup (alloc+free):  10.06x
```

</details>

At ~0.5 ns, `deallocate` is roughly **1–2 CPU cycles** — it is a single store plus a pointer swap, and the free-list head stays hot in L1.

---

## Why this is faster

`operator new` is a general-purpose allocator. It has to handle any size, defend against fragmentation, coalesce neighbours, maintain size classes, and — in a multithreaded process — synchronise. That generality costs branches, cache misses, and sometimes a lock.

A pool allocator throws all of that away by making one assumption: **every block is the same size.** Once every block is interchangeable, allocation degenerates to popping the head of a list.

```cpp
T* allocate() {
  if (m_free_list_head == nullptr) throw std::bad_alloc();
  Node* head{reinterpret_cast<Node*>(m_free_list_head)};
  m_free_list_head = head->next;      // pop
  return reinterpret_cast<T*>(head);
}

void deallocate(T* ptr) {
  Node* newHead{reinterpret_cast<Node*>(ptr)};
  newHead->next = reinterpret_cast<Node*>(m_free_list_head);
  m_free_list_head = newHead;         // push
}
```

No branches beyond the exhaustion check. No syscalls. No locks.

### The intrusive free list

The trick that makes this cost **zero bytes of metadata overhead**: free blocks store the "next free block" pointer *inside their own memory*. A block is either live user data or a free-list node — never both — so the two uses can share the same bytes.

```
One contiguous heap buffer, carved into BlockCount blocks of BLOCK_SIZE:

  m_pool
    │
    ▼
  ┌──────────┬──────────┬──────────┬──────────┬──────────┐
  │  blk 0   │  blk 1   │  blk 2   │  blk 3   │  blk 4   │
  │  next ───┼─► next ──┼─► next ──┼─► next ──┼─► nullptr│
  └──────────┴──────────┴──────────┴──────────┴──────────┘
    ▲
  m_free_list_head

After allocate() × 2, then deallocate(blk 0):

  ┌──────────┬──────────┬──────────┬──────────┬──────────┐
  │ FREE     │ IN USE   │  blk 2   │  blk 3   │  blk 4   │
  │ next ────┼──────────┼─► next ──┼─► next ──┼─► nullptr│
  └──────────┴──────────┴──────────┴──────────┴──────────┘
    ▲                        ▲
  m_free_list_head       (blk 0 points here)
```

Because the list is LIFO, a freed block is the *next* block handed out. That block was just touched, so it is still in L1 — the cache behaviour of a recycling workload is far better than the address-scattering you get from a general allocator.

### Sizing a block correctly

A block has to satisfy two independent constraints, because the same bytes serve two purposes:

```cpp
static constexpr std::size_t ALIGNMENT{std::max(alignof(T), alignof(Node))};
static constexpr std::size_t MIN_BLOCK{std::max(sizeof(T), sizeof(Node))};
static constexpr std::size_t BLOCK_SIZE{(MIN_BLOCK + ALIGNMENT - 1) / ALIGNMENT * ALIGNMENT};
```

`MIN_BLOCK` widens the block for types smaller than a pointer (e.g. `char`), so there is always room for the link. `ALIGNMENT` takes the stricter of `T`'s and `Node`'s requirements, since a block must be legally usable as either.

The rounding on `BLOCK_SIZE` is the subtle part. Aligning the *buffer* only fixes block 0 — block *i* sits at `i * BLOCK_SIZE`, so if the stride isn't a multiple of `ALIGNMENT`, every later block drifts. A 12-byte, `alignof(1)` struct gives `BLOCK_SIZE == 12`, putting blocks 1, 3, 5… four bytes off a pointer boundary and making the `Node*` write in `deallocate` a misaligned store. Rounding the stride up to `ALIGNMENT` keeps every block aligned, and a `static_assert` pins the invariant.

The buffer itself comes from `::operator new(POOL_SIZE, std::align_val_t{ALIGNMENT})` rather than `new std::byte[]`, which only guarantees `__STDCPP_DEFAULT_NEW_ALIGNMENT__` (16 bytes on this platform) and would quietly under-align a type like `alignas(64) struct`. The aligned form is paired with the matching `::operator delete(ptr, size, align_val_t)` in the destructor — mixing the two families is undefined behaviour that AddressSanitizer reports as `alloc-dealloc-mismatch`.

### Design decisions worth defending in an interview

- **Pool is one contiguous buffer.** A single aligned allocation at construction; the hot path never touches the system allocator again. Sequential blocks also mean hardware prefetchers do useful work on a linear sweep.
- **No per-block header.** Common allocator designs prepend a size/tag word to every block. That is 8–16 bytes of waste per object and a cache line touched on every free. The intrusive list makes it free.
- **`allocate()` returns raw storage, not a constructed `T`.** The allocator's job is memory, not object lifetime. Placement-new on top of it if you want construction — that keeps the allocator usable for types with no default constructor.
- **Exhaustion throws `std::bad_alloc`** rather than silently falling back to the heap. A fallback would hide a capacity misconfiguration behind a latency spike that only shows up in production. Failing loudly is the correct behaviour for a system where you are supposed to have capacity-planned.
- **Compile-time `BlockCount`.** Capacity is a template parameter, so the size is known statically and there is no runtime bounds state to carry. `POOL_SIZE` is a `static constexpr`, which means an implausible `BlockCount` overflows inside a constant expression and is rejected at compile time rather than silently wrapping to a small allocation at runtime.
- **Copy and move are `= delete`d.** Two pools sharing one `m_pool` would double-free it, and there is no sane copy semantic for live allocations handed out to callers. Deleting all four is the honest answer; the pool is a resource owner, not a value.

---

## Complexity & guarantees

|                          |                                                          |
| :----------------------- | :------------------------------------------------------- |
| `allocate()`             | O(1), lock-free, no syscall                               |
| `deallocate()`           | O(1), lock-free, no syscall                               |
| Construction             | O(BlockCount) — one heap allocation + free-list threading |
| Destruction              | O(1) — single aligned `::operator delete`                 |
| Metadata overhead        | **0 bytes per block**                                     |
| Fragmentation            | **None** — fixed-size blocks cannot fragment              |
| Alignment                | `max(alignof(T), alignof(Node))`, incl. over-aligned `T`  |
| Exception on exhaustion  | `std::bad_alloc`                                          |
| Copy / move              | Deleted — the pool is a non-transferable resource owner   |
| Thread safety            | **None** — single-threaded / thread-per-pool by design    |

Fragmentation deserves a sentence: because every block is identical and interchangeable, there is no such thing as a "hole too small to reuse." A pool that has run for a month behaves exactly like one that just started. For a long-lived trading process, that predictability is often worth more than the raw speed.

---

## Build & run

Requires CMake ≥ 3.25 and a C++20 compiler. [doctest](https://github.com/doctest/doctest) is pulled in automatically via `FetchContent` — no manual dependency setup.

```bash
# Release — this is the configuration the numbers above come from
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release
cmake --build build/release
./build/release/PoolAllocator
```

```bash
# Debug — -O0 -g with AddressSanitizer wired in
cmake -S . -B build/debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build/debug
./build/debug/PoolAllocator
```

The Debug build runs the same benchmark under AddressSanitizer and exits clean — no invalid accesses, and no `alloc-dealloc-mismatch` on the aligned buffer. (It reports ~5x rather than ~10x; that is the sanitizer's instrumentation tax on both sides of the comparison, not a real result. Note that ASan's leak detector is unavailable on macOS/arm64, so this run proves memory *safety*, not leak-freedom.)

The project compiles under `-Wall -Wextra -Werror -Wconversion -pedantic -Wsign-conversion -Wfloat-conversion` with zero warnings. Those flags are carried on an `INTERFACE` target rather than set directory-wide, so `-Werror` applies to this project's code and cannot be tripped by a fetched dependency's headers.

---

## Tests

Either configuration above builds the test binary alongside the benchmark, so the suite just runs:

```bash
ctest --test-dir build/debug   --output-on-failure   # under AddressSanitizer
ctest --test-dir build/release --output-on-failure   # optimized
```

`doctest_discover_tests` registers each `TEST_CASE` with CTest individually, so failures name the specific case and `ctest -R` can select one:

```
100% tests passed, 0 tests failed out of 21
```

21 cases from 490 assertions, passing in both Release and Debug/ASan configurations. What they cover:

| Area                | Test                                                                            |
| :------------------ | :------------------------------------------------------------------------------ |
| **Alignment**       | Every block of a full pool is aligned for both `T` and the free-list node — checked across `char`, `int`, `float`, `double`, `Order`, `alignas(64)`, and a 12-byte `alignof(1)` struct |
|                     | An over-aligned type genuinely lands on its 64-byte boundary                    |
| **Block layout**    | Blocks are pairwise distinct and never overlap                                  |
|                     | Live blocks do not alias — writes to 64 blocks stay independent                 |
| **Capacity**        | A pool yields exactly `BlockCount` blocks, then throws `std::bad_alloc`         |
|                     | Returning one block buys exactly one more allocation                            |
|                     | A single-block pool behaves at the boundary                                     |
| **Free list**       | LIFO order — the most recently freed block is the next one handed out           |
|                     | A full drain/release cycle loses and duplicates nothing                         |
| **Ownership**       | Copy and move are rejected at compile time                                      |

The two type-parameterised cases run over seven payload types each via `TEST_CASE_TEMPLATE`, which is where 14 of the 21 cases come from. The type list is deliberately adversarial rather than convenient: `alignas(64)` catches an under-aligned buffer, and the 12-byte `alignof(1)` struct catches an unrounded stride — the two bugs that a pool tested only against `int` and a nicely-sized struct will happily ship with.

### Do the tests actually have teeth?

A suite that passes on broken code is worth nothing, so the cases were checked against deliberately reintroduced regressions:

| Mutation                                                | Result                                        |
| :------------------------------------------------------ | :-------------------------------------------- |
| Drop the stride round-up (`BLOCK_SIZE = MIN_BLOCK`)      | 1 case fails, 8 assertions — the `Odd` alignment case |
| Revert to `new std::byte[]` / `delete[]`                 | 2 cases fail, 24 assertions — both over-alignment cases |
| Off-by-one in the free-list threading loop               | Crashes (SIGBUS) and throws `bad_alloc`       |

Each mutation is caught by the case written for it, not incidentally by an unrelated one.

### Files

| File               | Purpose                                                              |
| :----------------- | :------------------------------------------------------------------- |
| `PoolAllocator.h`  | The allocator. Header-only, ~125 lines.                              |
| `benchmark.cpp`    | Benchmark harness — the `PoolAllocator` target's entry point.        |
| `Tests.h`          | The doctest suite — 21 cases, run via `ctest`.                       |
| `tests.cpp`        | Test runner entry point; supplies doctest's `main`.                  |
| `Clock.h`          | `Util::Timer`, a `steady_clock` RAII stopwatch.                      |
| `main.cpp`         | Scratch driver for manual poking. Not part of any CMake target.      |

---

## Benchmark methodology

Microbenchmarks are easy to get wrong in ways that flatter you. Steps taken to avoid that:

1. **51 trials, median reported.** The mean is hostage to a single scheduler preemption or page fault; an odd trial count gives a clean median that rejects outliers on both tails.
2. **Memory is actually touched.** Every allocation writes `ptrs[i]->id`, and every free reads it back into an accumulator. An allocator that hands back addresses you never dereference is not being measured honestly — page faults and cache misses are part of the cost.
3. **Results are made observable.** The accumulator `sink` feeds a branch at the end of `main`, so the optimizer cannot delete the loops it just spent 51 trials running.
4. **Pool construction is excluded.** The pool is constructed once outside the trial loop. Threading the free list is a one-time O(n) startup cost, not a per-operation cost, and amortising it into the per-op number would understate the steady-state result the benchmark is about.
5. **Like-for-like comparison.** The baseline is `::operator new(sizeof(Order))` / `::operator delete` — raw storage against raw storage. Comparing against `new Order{}` would have folded constructor cost into the baseline only.
6. **Steady-state, LIFO-favourable pattern.** The workload allocates N then frees N — see the limitations below for why this is the pool's best case.

---

## Known limitations

These are deliberate scope boundaries for a Phase 1 exercise, not oversights — but they are real, and I'd rather state them than have someone find them:

- **Not thread-safe.** Concurrent `allocate()` calls will race on `m_free_list_head`. The intended usage is one pool per thread, which is also the design that actually wins in HFT — a shared atomic free list reintroduces the cache-line contention the pool exists to avoid. A lock-free variant needs a tagged CAS head to handle ABA.
- **Double-free is undefined behaviour.** Freeing the same pointer twice creates a self-referential cycle in the free list, after which two live allocations alias the same block. Detecting this cheaply requires a debug-only occupancy bitmap — worth adding behind `#ifdef DEBUG_MODE_ENABLED`, since it costs nothing in Release.
- **The benchmark measures the pool's best case.** Allocate-N-then-free-N keeps every access sequential and every recycled block cache-hot. A long-running interleaved alloc/free workload would scatter the free list across the pool and narrow the gap. The O(1) bound and the zero-fragmentation guarantee hold regardless; the *cache* advantage would shrink.
- **No `std::allocator` interface.** Adding `value_type`, `rebind`, and `allocate(n)` would let this drop into STL containers directly — though the fixed-block premise means only `n == 1` can ever be served, which is exactly what node-based containers like `std::list`, `std::map`, and `std::unordered_map` request.

## Next steps

- Debug-only double-free detection via an occupancy bitmap
- `std::allocator`-compatible adapter, benchmarked against `std::list<Order>` with the default allocator
- Lock-free multi-producer variant with a tagged head to defeat ABA, benchmarked against per-thread pools to show where the crossover is
- Randomised / interleaved allocation pattern in the benchmark to characterise the non-ideal case
