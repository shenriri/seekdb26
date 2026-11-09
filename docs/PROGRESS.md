# Phase 2 update — 2026-11-09

Implemented and tested disk-backed copy-on-write B+Tree, committed WAL + atomic manifest, bounded page/raw/decoded row cache, and WAL-only phase1 migration. Six Release CTest suites pass, four initial suites pass under ASan/UBSan; 50k x 128 data (27.35MB heap) observed sampled process RSS 13.8MB, and 110k PK B+Tree root split + restart passes. Three alternating benchmark rounds recorded in `tests/benchmark_phase2_ab_3rounds.json`. Official protocol, ANN, slot-based heap and WAL recycling remain. Detailed design in `PHASE2_STORAGE_AND_RECOVERY.md` and benchmark interpretation in `PERFORMANCE_PHASE2.md`.

---

# MiniVectorDB: Implementation log / acceptance gates

## Baseline and provenance

- Official remote branch previously verified: `oceanbase/seekdb/2026-competition-round2`
  commit `c842a368c3394748d0499386c18f6b465242e141`.
- Team target repo: `shenriri/seekdb26`. It was readable, but creating the branch via
  connected GitHub integration returned `403 Resource not accessible by integration`.
- The visible official baseline tree contains empty root `Dockerfile` and
  `conversation.json`; its former `src/` code was removed. The official round2 SQL
  test driver/error mapping is not available in the inspected material.
- Therefore all program files here are an **independent prototype**, not yet uploaded
  to the team repo. They must not be described as part of a remote commit.

## Stage status

| Phase | Actual status | Proof / blocker |
|---|---|---|
| P0 baseline / official protocol | Partial | Baseline known; official input/output schema not available; provisional newline adapter only |
| P1 SQL & types | Implemented subset | Integration/differential tests; no JOIN/NULL/aggregate by requirement |
| P2 bounded disk storage | Not started | Current committed rows in memory; no B+Tree or page cache |
| P3 transaction/recovery | Prototype complete | WriteSet, single-record WAL, fdatasync, crash tests; no checkpoint or spill |
| P4 exact vector | Prototype complete | L2 float32 + double accumulation, filter first, TopK & PK ties |
| P5 ANN/index maintenance | Not started | `CREATE VECTOR INDEX` explicitly errors |
| P6 full contest performance | Preliminary only | Local T/RSS/io; real cgroup T/B/M and frozen baselines still missing |
| P7 contest submission | Not started | Docker/protocol/agent data incomplete; no pushed commit |

## Executed tests

- `cmake --build` (C++17 Release): successful.
- `ctest` three test suites: integration, fault injection and randomized differential,
  passing on the provided environment.
- ASan+UBSan Debug build and tests: all three passing.
- Deterministic differential: 220 mixed operations vs an independent Python model,
  then reopen and verify persistent content.
- Crash injection: SIGKILL during partial WAL record, after WAL sync/before memory
  apply, and during recovery; all corresponding invariants passed.

## Measured performance (not official scoring)

Host environment: current sandbox, Release C++, deterministic 10,000 x 128 float32,
100 batches of 100 INSERT, 1000 K=10 vector TopK queries (half equality-filtered),
30-query warmup. Independent NumPy sample truth. Note: official frozen runner, cgroup
and baseline have not been run.

- Single run **after late materialization**: write 1.127331 s, query 1.156181 s,
  query 864.92 QPS, process sampled RSS 10,264,576 B, process I/O 5,894,144 B;
  sample Recall@10 = 1.000 unfiltered, 1.000 filtered (100 queries checked).
- Alternating A/B/A/B/A/B query runs, same seed and workload:
  - Before late result materialization: 1.236894 / 1.294285 / 1.324779 s;
    median 1.294285 s.
  - After late materialization: 1.078589 / 1.053667 / 1.111960 s;
    median 1.078589 s.
  - Observed median query speedup = ~1.20x. Write times showed no comparable
    improvement, which is consistent with the change being query-only.
- This is a local experiment, **not** the contest E index, not the official 3-round
  cgroup results, and does not establish a passing hidden test score.

## Highest-risk technical issues (address before declaring contest-ready)

1. **Bounded storage**: current `std::map<int64_t,Row>` stores all committed data in
   RAM. Formal tests with database size > memory will fail. Replace with 8 KiB slotted
   disk pages, fixed-capacity BufferPool and disk B+Tree.
2. **Safe WAL checkpointing**: current WAL grows with every commit and startup must
   redo every record. Persist heap/index pages with pageLSN and checkpoint manifest;
   make WAL recycling crash-safe and replay idempotent.
3. **ANN correctness under updates**: implement self-authored index with version/tombstone
   filtering, transactional overlay and on-disk posting; exact mode must stay exact.
4. **Official protocol mismatch**: current `OK`/`ROWS` protocol is not contest protocol.
   Replace adapter based on the official driver and test every error code.
5. **TopK filtering**: correctness passed in current random tests; with ANN and sparse
   WHERE predicates, never apply TopK before the filter.
6. **Measurement fidelity**: official cgroup v2 `io.stat` and `memory.peak` include
   file cache and index build; process RSS alone is not enough. Collect all three
   metrics over the same lifecycle and compare three-run medians.

## Next implementation acceptance gates

**P2-A (heap + buffered disk scan):** 8 KiB slotted pages; rowid metadata; segment
iteration reads only a bounded cache; 1M x 128 vectors under an independently enforced
memory cap. Reopen verifies all data; introduce overflow storage for large strings.

**P2-B (PK B+Tree):** disk leaf/internal pages; unique insertion; range; iterator;
persisted root; split/merge or append+rebuild policy; crash/restart integration with WAL.

**P3-B (checkpoint):** dirty-page flushing ordered after WAL, atomic manifest update,
short-write/ENOSPC handling, SIGKILL at each checkpoint boundary, restart twice.

**P5 (ANN):** build IVF-Flat postings, test mixed post-index inserts/updates/deletes,
filtered + unfiltered Recall@10 >=0.95, no duplicate/stale IDs, then tune nlist/nprobe.
