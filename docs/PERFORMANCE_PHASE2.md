# Phase 2 performance study (local-only, 2026-11-09)

## Baseline and experiment

Both versions run in newly created temporary `/data` dirs on the same local
execution environment, with alternating execution order, 3 rounds each.
Workload: 10,000 x 128 float32 vectors, 100-row autocommit INSERT batches,
1,000 exact L2 TopK queries (K=10; 50% groupid equality filtering), same
seed. The harness checks 100 query results using an independent NumPy exact
reference. Results: `tests/benchmark_phase2_ab_3rounds.json`.

| Metric | Phase 1 median | Phase 2 median | Interpretation |
|---|---:|---:|---|
| Write T | 1.099 s | 1.367 s | Disk index+manifest imposes 24% overhead |
| Query T | 1.110 s | 1.212 s | Bounded cache+index traversal imposes ~9% overhead |
| Process I/O | 5.62 MiB | 13.63 MiB | Physical persistence adds ~8MiB |
| Sampled RSS | 9.79 MiB | 11.11 MiB | Bounded cache costs ~1.3MiB at 10K |
| Recall@10, filtered/unfiltered | 1.0/1.0 | 1.0/1.0 | Sampled exact matches NumPy reference |

**There is no claim that Phase 2 improves Phase 1 on T/B/M at 10K.** Its gain
is correctness under dataset growth without an O(N) committed-row map.
For 50,000 x 128, `tests/test_bounded_memory.py` observed 27,350,000 heap
bytes with sampled process RSS peak 13,807,616 bytes, and verified reopening.
This does not prove a 13MB cgroup memory footprint: cgroup includes page cache.

## Performance experiments with causal mechanisms

1. Naive persistent COW pages, decoded every candidate: 1K x 128/100 queries,
   ~1.54s. A 1MiB encoded row cache alone reduced this to ~1.07s.
2. Use B+Tree leaf keys/row locations and a bounded 8MiB decoded-row LRU:
   same workload ~0.035s. Filtering/Recall tests continued to pass.
3. On 10K x 128/1K queries, per-key immutable page publication wrote about
   173.9 MB according to process I/O. Coalescing all pages reachable at the
   end of a 100-row transaction brought this down to ~14.29 MB in a local run.
   Write time in those single local samples went ~3.17s -> ~1.59s.
4. Three alternating end-to-end runs (table above) validate the final tradeoff;
   the stage2 final median write/query are 1.367s / 1.212s.

## Official measurement gaps and next optimization targets

The scorer measures wall-clock **T** (write and query separately), physical
block **B = io.stat rbytes+wbytes**, and cgroup `memory.peak` **M** including
file page cache, container/runtime and index preparation. The current benchmark
uses process `/proc/<pid>/io` and sampled RSS, **not** scored B/M. In this
session `/sys/fs/cgroup` is not writable, so we cannot set up an isolated group.
Docker CLI is also unavailable, so Docker image build has not been tested.

To compete effectively next:

- Obtain official test protocol/limits and frozen baseline; build a cgroup v2
  harness with a dedicated cgroup and file-backed /data device.
- Make true slotted-row storage/compaction for mixed update/delete and check
  page reuse without violating WAL durability.
- Add ANN `CREATE VECTOR INDEX` and filtered TopK routing, holding average
  Recall@10 >=0.95 for *both* filtered and unfiltered groups.
- Optimize memory vs cache hit-rate with 4/8/12MiB decoded LRU experiments;
  cgroup M can penalize a cache that helped time T.
- Test memory limits and recovery time for bigger data and transactions,
  repeated checkpoints and WAL space growth.

Do not overfit official benchmark seeds or trade filter correctness for Recall.
