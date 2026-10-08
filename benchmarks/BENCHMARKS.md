# Recorded queue and parsing benchmark — October 8, 2026

The primary run transfers 500,000 identical prebuilt frames per trial with a capacity-256 queue. Seven pairs alternate SPSC/mutex order after one warm-up of each implementation. Both paths decode the same CRC-protected frames and update the same feed state. Publication counts and digests match in all trials.

| Implementation | Median frames/s | Median sampled p50 | Median sampled p99 |
|---|---:|---:|---:|
| spsc | 1,552,720 | 147.21 μs | 363.16 μs |
| mutex | 1,550,060 | 152.00 μs | 341.03 μs |

Median **paired** throughput ratio (SPSC / mutex): **0.989×**. Individual ratios: 0.811×, 0.938×, 0.980×, 1.062×, 0.989×, 1.003×, 1.080×. This is a workload/host observation, not a universal speedup.

Latency is sampled every 128th message, beginning before enqueue attempts and ending after consumer decode/state handling. It includes time spent waiting for a full queue and the queued backlog. It is not individual ring-operation latency. Percentiles use sorted observed samples with lower-index selection, without interpolation. The table reports medians of trial percentiles, not a merged percentile across trials.

The first, shorter pilot (`paired-20261008.csv`) ran while other project builds/tests were active and is retained for transparency. It is not used for the main performance claim. The longer run deliberately avoided simultaneous project builds, but the shared host still provides no affinity, CPU isolation, or noise guarantee. Compare raw trials and rerun on the target machine.

`environment.json`, `summary.json` and `paired-long-20261008.csv` contain the exact workload metadata and outcomes. Binary frames are prebuilt outside the timed region, and worker threads wait at a start barrier. Timing ends with the final consumer update. Frames/s includes queue transport, parsing, CRC and state work, while excluding frame generation and disk/network I/O. Busy-yield backpressure can consume CPU, and parser cost can dominate the queue difference.

The separate allocation probes cover queue push/pop after construction. The benchmark's vectors, threads and recorded samples allocate. A zero-allocation claim for the whole process would be incorrect.

## Reproduce

```bash
make all test
./build/benchmark 500000 7 256 > benchmarks/local.csv
```

Do not use these as matching-engine, exchange feed, NIC or order-throughput numbers.
