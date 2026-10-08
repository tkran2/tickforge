# TickForge

**A bounded C++20 feed transport with strict binary parsing, generation/sequence recovery, and an honest SPSC-versus-mutex comparison.** It complements a matching engine: this project is about safely getting data between threads and deciding when a quote can be trusted.

TKF1 is a **local replay format**, not a claimed exchange protocol. There is no NIC, socket receive path, order submission or matching logic in its benchmark.

## Build and verify

```bash
make all test
make sanitize
make benchmark
```

Requires a C++20 compiler, Make and pthreads. Python 3 is optional for conversion and the independent replay oracle. On systems where LeakSanitizer is unavailable under the host's process tracing, use `make sanitize ASAN_OPTIONS=detect_leaks=0`; that still enables AddressSanitizer and UndefinedBehaviorSanitizer but does not check leaks. `make tsan` supplies a separate ThreadSanitizer check where supported.

The recorded checks include **1.6 million concurrent transfers**, order checks at capacities 8 and 256, unsigned cursor wraparound, full/empty handling, malformed binary inputs, trust-gate recovery, and allocation probes showing no dynamic allocation inside either queue's push/pop loop after construction. This is not a claim that the whole application has no allocations.

## Design

Each 64-byte frame contains:

| Offset | Bytes | Meaning |
|---|---:|---|
| 0 | 4 | `TKF1` magic/version |
| 4 | 1 | Snapshot=1, update=2, heartbeat=3 |
| 5 | 3 | Reserved zero |
| 8 | 8 | Generation |
| 16 | 8 | Sequence |
| 24 | 8 | Bid price in fixed-point ticks |
| 32 | 8 | Ask price |
| 40 | 8 | Bid size |
| 48 | 8 | Ask size |
| 56 | 4 | Standard CRC-32 over the first 56 bytes |
| 60 | 4 | Reserved zero |

Multi-byte fields are big-endian. The parser checks exact length, magic, kind, reserved fields, CRC and positive non-crossed BBO fields. It reads bytes explicitly rather than casting an unaligned pointer. CRC catches accidental corruption; it is not authentication.

**Recovery rule:** a new generation must start with snapshot sequence zero. Heartbeats count in sequence continuity without publishing a quote. Old generations and duplicates do not mutate the quote. A gap or malformed frame closes the trust gate; continued updates cannot reopen it. A fresh higher-generation snapshot is required. No quote is accessible through the public API while fenced.

**Queue ownership:** the SPSC ring has exactly one producer and one consumer. The producer exclusively advances the head; the consumer advances the tail. A release store publishes a written slot, and an acquire load makes that slot safe to read. The opposite release/acquire pair protects reuse of a consumed slot. See the [C++ memory-order specification](https://eel.is/c++draft/atomics.order). Relaxed loads are used only for each thread's own cursor. The ring never overwrites a full slot or silently drops a frame. It does not support multiple producers or consumers.

The alternative uses the same preallocated fixed-capacity ring behind a mutex. Both benchmark paths use identical prebuilt frames, parser work, state updates, bounded capacity and yield-based backpressure. This avoids comparing one algorithm with a different container or allocation strategy.

## Real-data replay

The included binary is derived from MicropriceLab's 9,514 trusted October 8 Coinbase BTC-USD publications. Its transport sequences are **new local IDs**, not Coinbase sequence numbers. Both the C++ replay and an independent Python `struct`/`zlib` implementation verify the same publication count and deterministic digest.

```bash
./build/tickforge replay benchmarks/real-20261008.tkf
python3 tools/check_replay.py benchmarks/real-20261008.tkf
# To convert another MicropriceLab observation file:
python3 tools/pack_quotes.py ../micropricelab/data/observations/20261008.csv /tmp/quotes.tkf
```

## Measurements

Read [BENCHMARKS.md](benchmarks/BENCHMARKS.md) and the raw CSV. Trials alternate implementation order after warm-up. Throughput includes queue transfer, binary validation and book-state handling; sampled latency runs from the producer's enqueue attempt through the consumer's state update, including backpressure. It is not exchange-to-order latency or isolated queue-operation latency.

The shared virtual host has no affinity or CPU isolation. Results can vary substantially, and a trial can favor the mutex. The queue's benefit is workload-specific; parsing can dominate both paths. Rerun on your Mac or Windows/Linux machine before citing personal benchmark results. The benchmark CLI accepts `messages`, `trials`, and `capacity`:

```bash
./build/benchmark 500000 7 256 > benchmarks/my-machine.csv
```

## Decisions to defend

- Why sequence gaps fence quotes instead of publishing potentially incorrect state.
- Which release/acquire edge protects slot ownership, and why `volatile` would not substitute for it.
- Why the baseline uses the same ring and work, and what the sampled latency includes.
- Why 64-byte cursor separation is a layout choice to validate on the target CPU, rather than a universal performance guarantee.
- What would change with multiple producers, a socket reader, variable-length frames, a sleeping consumer, or a fully reconstructed depth book.

The useful next extension is a measured bottleneck change—CRC implementation, batching, or a wait strategy—followed by the same oracle, stress checks and workloads. Preserve a result that gets slower; identifying a tradeoff is part of the project.
