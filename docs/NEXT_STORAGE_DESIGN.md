# P2 design implementation handoff — disk-backed storage and recovery

This stage is NOT implemented by the Phase 1 prototype. It is the next step, not a
claim about existing code. The interfaces deliberately separate SQL from physical
storage so that the current parser/transaction/exact TopK can be retained.

## Storage targets

- `PageId=(table_id,file_kind,page_no)`, 8 KiB little-endian pages.
- Header: magic, page type/version, pageLSN, slot count, free-offset, CRC.
- Slot array: `(offset,length,flags)` entries growing from page end; payload from
  header end; long VARCHAR/VECTOR values in overflow page chains.
- `RowId=(heap_page,slot_no,generation)` so deleted/reused slots cannot trick indexes.
- `PrimaryIndex` maps `(table_id, int64_pk)` -> `(RowId, version_lsn)` using on-disk
  B+Tree (point lookups, ordered scans) with a pinned-leaf iterator.
- A fixed-size BufferPool with page pin/unpin + dirty page tracking. Memory budget
  is a hard number of frames, not "cache everything and let Linux handle it".

## WAL + disk publication invariants

1. Never flush a data/index page whose `pageLSN > durable_wal_lsn`.
2. Autocommit and COMMIT both sync WAL commit record before acknowledging success.
3. Recovery REDOs committed logical changes in LSN order, with pageLSN/version
   checks so a second recovery does not duplicate PKs or index postings.
4. Only after all relevant dirty pages are synced, atomically write + rename
   the manifest to advance checkpoint LSN; fsync its parent directory.
5. Truncate/recycle WAL only after checkpoint is durably published.
6. Catalog CREATE TABLE is transaction-logged and appears or disappears as a unit.
7. Index files are rebuildable from committed heap rows when version mismatch is
   detected; readiness waits for required indexes to become consistent.

## Migration work in manageable commits

1. Define `IRowStore::lookup/scan/apply_batch` and preserve existing visible row
   semantics; keep a reference in-memory implementation for differential tests.
2. Add slotted-page I/O, page CRC and page cache. Validate scan over data > cache.
3. Add disk PK index and hook row version/tombstone updates; validate against
   `std::map` reference under random mutations.
4. Link staged transaction commit to WAL LSN, ensure data page flush never precedes
   commit durability. Add crash/recovery fault injection at all boundaries.
5. Implement checkpoint and cap WAL growth; measure startup time and write I/O.
6. Add bounded external ORDER BY/UPDATE staging via `/data/tmp` for large results.

## Benchmark gates after P2

- Run 10k x 128 / 1000 K=10 under a **dedicated** cgroup v2; record wall-clock T,
  `io.stat` `rbytes+wbytes` B and `memory.peak` M across startup/load/index/warmup/query.
- Reopen after each run and check all rows. Run `kill -9` at durable commit boundary,
  at page writeback and at checkpoint rename.
- Run data larger than memory cap and assert no OOM, timeouts or unbounded eviction.
- Optimize only if correctness+recovery+resource gates remain green.
