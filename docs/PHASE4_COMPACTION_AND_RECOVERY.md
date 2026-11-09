# Phase 4: crash-safe generation compaction and B+Tree point lookup

## Scope and current correctness boundary

This is still a **single-process developer prototype**. The official `2026-competition-round2` test driver, error-code table, ANN-mode wire protocol, resource caps, and frozen Docker environment have not been verified. The SQL stdin/stdout adapter is provisional. No changes were pushed to GitHub.

This phase adds **opt-in, startup-only, stop-the-world physical compaction** and primary-key-equality direct lookup. It does **not** implement slot-directory heap pages or continuous online garbage collection.

## Why generation compaction is necessary

Phase 3 uses an append-only `heap.mvdb` and a copy-on-write `index.mvdb` B+Tree. INSERT/UPDATE/DELETE create historical row bytes, tombstones and unreachable index pages. WAL rotation does not reclaim either data file. Reusing pages in place would conflict with the committed manifest during a SIGKILL before the next successful commit. Phase 4 instead creates a new physical generation, preserving the old files until the manifest switch is durable.

## Manifest format

Existing v1/v2 manifests are accepted. Newly written manifests are v3:

```
magic:u32 | version=3:u32 | applied_wal_offset:u64
heap_end:u64 | next_page_id:u64 | wal_generation:u64 | data_generation:u64
catalog_size:u32 | catalog_bytes | table_count:u32
[table_name:string, root_page_id:u64]... | crc32:u32
```

The `data_generation` controls both file names (`heap.mvdb`, `index.mvdb` for generation 0; `heap.<generation>.mvdb` and `index.<generation>.mvdb` thereafter). The WAL generation remains an *independent* counter.

## Procedure and SIGKILL safety

After `Database` opens the published store, replays its WAL and **before ANN readiness**, `MVDB_COMPACT_ON_START=1` triggers:

1. Traverse only reachable leaves of each B+Tree, skipping tombstones.
2. Copy each committed live row into a freshly created `heap.N.mvdb`. Write row length, CRC32 and encoded bytes.
3. Bulk-load new B+Tree leaf pages in PK order (up to 320 keys per leaf); build internal levels from each child's first key, reducing old sparse COW pages to dense pages. One leaf is accumulated in memory at a time; each higher level stores bounded-size child references (O(number of leaves), **not** O(row count)).
4. Synchronize both files, then the data directory. A crash here leaves the original manifest and old generation authoritative (`compact_after_data_sync`).
5. Write/FDATASYNC the temporary manifest referencing the new generation, rename over `manifest.mvdb` and FSYNC the parent directory. A crash here leaves the new generation authoritative (`compact_after_manifest`).
6. Close the old data descriptors, switch active descriptors and clear raw row and page caches. Delete old-generation files and FSYNC their directory (`compact_after_cleanup`). A future startup also removes orphan generations from interrupted maintenance.
7. Invalidate and rebuild IVF-Flat sidecars **before serving SQL**. Index metadata format v2 records both committed WAL cursor **and** `data_generation`; old offsets can never be trusted across compaction. Normal exact mode is unaffected by ANN rebuild.

The normal commit path still follows WAL sync -> heap/index sync -> atomic manifest publication, with WAL generation rotation unchanged. `compact()` does not modify or truncate WAL, and recovery replays only records after the manifest cursor.

**Temporary disk requirement:** at least the live compacted dataset plus the old generation until the manifest is published. Disk quotas may prevent maintenance. If no capacity is available, compaction is not suitable for that workload. Startup compaction is explicitly opt-in, not used automatically in the benchmark.

**Fault model:** the contest limits the crash model to process termination (SIGKILL), not power loss or storage corruption. The protocol uses FDATASYNC and FSYNC nonetheless. A full independent power-failure durability certification has not been performed.

## Direct PK lookup

The phase-3 `each_visible` implementation scanned all B+Tree leaf rows for `SELECT ... WHERE id = n` and for PK-targeted UPDATE/DELETE. New `each_matching` checks prepared predicates: when a BIGINT primary-key equality exists it calls `visible_row` directly, which first checks the active transaction overlay and otherwise makes a B+Tree point lookup. Remaining predicates are evaluated against this visible row. All other predicates continue through the prior scan behavior.

## Verification

- `tests/test_compaction.py`: 1,800 shuffled keys, 2 tables, repeated UPDATE, DELETE, ROLLBACK, exact/ANN checks, SIGKILL before and after manifest switch, next-commit correctness and repeated compaction.
- `tests/test_deep_btree.py`: 110,000 keys exercise a multi-level COW B+Tree, followed by compacted bulk-load and primary-key reads across the new root.
- Full Release: 9/9 test suites passed.
- ASan+UBSan: 7/7 bounded-size functional suites passed; the two very large suites exceeded the available sanitizer test time budget and are *not* claimed as sanitizer successes.

## Important limitations

- No page-level slot directory, free-page list, incremental compaction, live transaction spill, or huge-result streaming.
- **Not** a generally performant ANN implementation: phase-3 low-probe IVF recall failures remain; the default ANN cost guard can use exact scanning.
- The `MVDB_COMPACT_ON_START=1` setting performs compaction on each startup; do not leave enabled unintentionally.
- A reader built from phase-3 code cannot open the new v3 manifest (forward-only file-format evolution).
- Starting or completing compaction is part of startup/recovery latency, not a free setup step for official benchmarking. Physical data may temporarily occupy roughly two generations.
