# Phase 3 — architecture, correctness and performance evaluation

## Measured tests (2026-11-09, local prototype)

Release CTest: all **8 of 8** passed: SQL integration, fault injection, randomized differential, disk B+Tree stress, 50K bounded-memory stress, 110K deep B+Tree stress, new WAL rotation/crash, new IVF index transaction/recovery/corruption tests.

Debug with ASan/UBSan: six suites passed (large bounded-memory and deep B+Tree suites exercised in Release only).

### Alternating 3-round benchmark, 10K records x 128 float32, 1000 TopK queries (500 with tag filter)

| Metric (median) | exact / no index | ann with IVF index + cost guard |
|---|---:|---:|
| batch write, seconds | 1.367320 | 1.310222 |
| IVF index preparation, seconds | 0 | 0.662905 |
| 1000 TopK query seconds | 1.194406 | 1.191224 |
| process read/write bytes (as reported by `/proc/<pid>/io`) | 14,290,944 | 19,845,120 |
| sampled peak process RSS bytes | 11,726,848 | 11,788,288 |
| sampled Recall@10 (unfiltered / filtered) | 1.0 / 1.0 | 1.0 / 1.0 |

Full numbers: `tests/phase3_benchmark_summary.json` and `tests/phase3_bench/{exact,ann}_{1,2,3}.json`.

**Interpretation:** ANN mode at this workload deliberately uses the exact plan because the estimated IVF candidate cost is worse. Query time is effectively unchanged; IVF creation **increases measured process I/O by ~5.55 MB**. This is a correctness/feature milestone, not an ANN speed win. The observed changes in write time are normal three-round variance; index construction happens after bulk write.

### Forced true-IVF path, 10K x 128, 100 queries (for controlled algorithm analysis)

| IVF probes | Query time, s | QPS | Recall@10 no filter | Recall@10 filter | Meets 0.95? |
|---|---:|---:|---:|---:|---|
| 8 / 64 | 2.352586 | 42.51 | 0.434 | 0.398 | **No** |
| 64 / 64 | 10.869034 | 9.20 | 1.000 | 1.000 | Yes, but unacceptable latency |

Full results: `tests/phase3_bench/forced_ivf_8probes.json`, `...allprobes.json`. This exposed the hard high-dimensional trade-off: centroid partitions are poorly selective with uniformly random 128-dimensional vectors; validating versioned postings through the primary index is expensive. Neither configuration is a useful official performance choice as-is.

### What to optimize next

- A simple but strong exact baseline: SIMD/batched L2, vector-only contiguous column storage, avoid per-row heap decoding for TopK; measure first.
- Design a *truly* competitive filtered ANN approach, e.g. posting-level scalar filter information or a secondary scalar index with adaptive exact fallback; compare at ≥0.95 recall and both filter groups.
- Replace per-posting B+Tree validity probes with a compact indexed version map or per-partition live-list compaction; ensure bounded memory and transactional correctness.
- Add vacuum/compaction of disk row versions and COW pages; existing append-only storage never reclaims them.
- Test under real cgroup v2 limits and capture `io.stat` and `memory.peak`; local `/proc` metrics are not comparable to the official baseline.

## Crash invariants

1. WAL transaction payload is synced before data/index publication.
2. Heap and primary B+Tree pages are synced before atomic primary manifest rename.
3. ANN sidecars are *derived*. The metadata's committed WAL cursor is advanced only after their postings are synced; if the process is killed between steps, startup rebuilds from the committed primary state.
4. For WAL retirement, a new empty `wal.N.mvdb` is synced and directory entry persisted *before* the primary manifest atomically refers to that generation. The old WAL may then be removed. An interruption before the manifest uses the old WAL; after uses the new WAL. The ANN cursor is updated after cutover or the ANN sidecar is rebuilt on restart.
5. The old WAL-only and Phase 2 v1 manifest formats remain readable; new manifests use v2 with a WAL-generation field. WAL tx payload version 1 and version 2 are both readable.

## Risks not resolved

- On creating or using IVF, recovery may need to scan the full table and reconstruct derived partitions. This increases startup work, counted in formal recovery resources.
- Disk B+Tree update/delete uses tombstones; old COW pages and obsolete heap versions accumulate. WAL recycling helps log footprint but does not solve heap/index growth.
- Fixed defaults in the cost guard are preliminary heuristics calibrated only with one local workload; performance tests must be repeated when the official generator is published.
- Official benchmark requires cgroup v2 physical I/O and memory.peak; our environment cannot create a separately controlled writable cgroup. Docker CLI and official SQL protocol also still unverified.
- Never describe local proof as hidden-test qualification or claim ranking points before official acceptance.
