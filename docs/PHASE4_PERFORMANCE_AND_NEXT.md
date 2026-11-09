# Phase 4: local performance evidence, decisions, next work

The tests below are from a local Linux C++17 release build on 2026-11-09. These are **not** official OceanBase measurements. Per-process RSS/I/O used by the development benchmark do not equal cgroup v2 `memory.peak` / `io.stat`; the latter also includes page cache and writeback attribution.

## Physical compaction (high-update workload)

The new `compaction_and_crash` test inserts 1,800 shuffled vector rows, repeatedly updates subsets, deletes 300 rows, and builds an IVF index. Measured before/after successful compact:

| File category | Before | After | Reduction |
|---|---:|---:|---:|
| Row heap | 255,498 B | 76,611 B | 70.0% |
| COW B+Tree | 1,212,416 B | 65,536 B | 94.6% |
| IVF postings | 200,320 B | 78,480 B | 60.8% |

These reductions arise from that particular update/delete workload; do not extrapolate to append-only data. Files are not overwritten in place. A temporary new generation is needed during compaction.

## PK equality fast path (three alternated local rounds)

10,000 rows, 500 `SELECT g FROM t WHERE id=...` queries, including a subset with a second predicate. Same read-only persisted phase-3 database per variant; observed returned rows were identical.

| Variant | Individual seconds | Median |
|---|---|---:|
| Phase 3 full scan | 0.164208 / 0.154099 / 0.160019 | 0.160019 s |
| Phase 4 direct B+Tree lookup | 0.018517 / 0.018128 / 0.018599 | 0.018517 s |

**Observed speedup: 8.64x in this PK-equality microbenchmark.** This is a performance optimization for point predicates, not an ANN improvement.

## Default 10K x 128 TopK baseline (three alternated rounds)

Each run inserts 10K rows in 100-row committed batches and performs 1,000 exact TopK (half filtered), 80 independent NumPy truth comparisons, separate cold database. Median of 3 runs, same benchmark script for each binary:

| Variant | Write median | Query median | Sampled Recall@10 |
|---|---:|---:|---:|
| Phase 3 | 1.464504 s | 1.313547 s | 1.0 / 1.0 |
| Phase 4 (without later microexperiment) | 1.550168 s | 1.367001 s | 1.0 / 1.0 |

Phase 4 is **about 4% slower in query and 6% slower in write** in these local medians; data-generation compaction is *disabled* during the runs and makes no claim of TopK acceleration. Observed process I/O was ~14.29 MB in both; differences were small. Full per-round results live in `tests/phase4_benchmark_summary.json` and `tests/phase4_bench/`.

## Rejected TopK microoptimization

We tested squared L2 accumulation with an early cutoff once the top-K heap filled. In 3 alternating default benchmark rounds it was **slower** (median 1.485787 s vs. previous 1.426812 s), despite matching sampled exact results; branch overhead and layout effects are possible causes but were not profiled. **The change was reverted**. Raw A/B results remain at `tests/phase4_bounded_l2_summary.json` and `tests/phase4_bounded_ab/` as negative evidence; the submitted source uses the original L2 function.

## Next priorities (must precede formal submission)

1. Obtain the actual `2026-competition-round2` runner/test package. Verify SQL statement framing, output/error formatting, readiness/shutdown, ANN selector and resource limits. Current development protocol is not a valid submission claim.
2. Implement true slotted heap pages, reusable free-space maps, and online-safe reclamation only after verifying performance and crash semantics. Phase-4 compaction is *offline*, not an online storage engine.
3. Reduce ANN random PK lookup overhead: a generation-aware, memory-bounded live-vector access path and posting compaction policy; evaluate **both** filtered/unfiltered Recall@10>=0.95 and latency. Current cost guard often falls back to exact.
4. Bound explicit transaction WriteSet and unbounded SELECT outputs, consider spill-to-disk with a tested atomic commit path.
5. Build and test the supplied Dockerfile in a real Linux Docker engine. Independently collect cgroup v2 io.stat and memory.peak under fixed quotas, including compaction/rebuild and file-page cache.
6. Re-run all functional tests under official driver; only then consume a formal submission attempt. Keep authentic `agent/conversation.json` with user prompts and measured experiments, no invented student reasoning.
