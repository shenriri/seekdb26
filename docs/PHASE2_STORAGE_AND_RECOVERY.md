# Phase 2: Disk storage + COW B+Tree + WAL recovery (implemented)

## Physical files and bounds

Every file lives under configured `/data` (the CLI supports `--data-dir` for tests).

| Path | Role | Durable publication |
|---|---|---|
| `wal.mvdb` | Complete committed transaction records; len + CRC | `fdatasync(wal_fd)` precedes success |
| `heap.mvdb` | Appended `[u32 length][u32 crc][encoded row]` | `fdatasync(heap_fd)` before manifest |
| `index.mvdb` | Immutable 8 KiB B+Tree pages | `fdatasync(index_fd)` before manifest |
| `manifest.mvdb` | Catalog, table root IDs, heap length, index-next-page, applied WAL offset, CRC | temp file + fsync + atomic rename + directory fsync |

There is no CPU-process-wide in-memory `std::map<pk, Row>` for committed records.
B+Tree internal pages contain separators and child page IDs. Leaf pages contain
ordered `(int64 pk, u64 heap_offset, u32 raw_length)` entries; length=0 is a
tombstone. Deserializing pages checks CRC and format. The tree does not need
leaf next-pointers because range scans recurse down internal pages.

**Not implemented:** slotted heap pages, page reuse, disk-space compaction, or
recycled WAL. These physical design limitations matter for very large updates.

## Commit ordering: WAL is the authoritative recovery source

1. Parse and validate SQL; build transaction overlay/WriteSet without modifying
   committed heap/index files. Each statement stages changes atomically.
2. Append ONE checksummed WAL record for the final transaction; `fdatasync` it.
3. Build the new B+Tree root(s) with copy-on-write pages. Within a transaction,
   allocate transient pages and remap only **reachable final pages** to compact
   physical page IDs. Append new row versions to the heap.
4. `fdatasync` both heap and B+Tree files (data must be durable before root
   metadata points to it).
5. Write new manifest to a temporary file; sync it; rename; sync containing
   directory. Only after durable manifest publish are the new roots visible
   across process restart.
6. Respond `OK`. The SQL layer never publishes uncommitted changes.

If the process dies at 2-4, the old manifest remains authoritative; startup
truncates speculative tails and replays all committed WAL records past the
manifest cursor. If it dies after step 5, the new manifest cursor means recovery
does NOT reapply that transaction. WAL records are never omitted before they
are known durable, and committed txns are re-applied as complete batches.

When loading a Phase 1 WAL-only directory (no manifest), the new store resets
its scratch physical heap/index files and recovers the whole WAL from offset 0.

## Crash failpoint matrix

| `MVDB_TEST_FAILPOINT` | Effect | Recovery expectation |
|---|---|---|
| `mid_wal_write` | Torn record tail | Discard incomplete last record |
| `after_write_before_sync` | WAL written but no explicit sync | It may replay or be omitted; never partial |
| `after_sync_before_apply` | WAL durable, before disk apply | REDO transaction on restart |
| `after_heap_before_index` | Heap appended before B+Tree publication | Truncate unpublished bytes, REDO |
| `after_index_before_manifest` | Index synced, manifest old | Truncate unreachable pages, REDO |
| `after_manifest_published` | New manifest published | Skip replay and preserve result |
| `recovery_after_replay` | SIGKILL during REDO after publish | Second startup resumes from manifest |

Crash model exercised is process SIGKILL, **not** power-cut/disk corruption.

## Complexity and memory bounds

- PK lookup: O(log_B N) index nodes + one row blob read.
- Table scan / exact TopK: O(N x dimension) vector distance, with bounded
  ordering heap O(K); B+Tree scans produce ordered PKs.
- Committed-row working set: up to 64 cached page frames (512 KiB), 1 MiB raw
  row bytes, 8 MiB decoded-row bodies, plus per-entry metadata and transient
  statement results. These bounds do NOT cap a huge explicit WriteSet, oversized
  SQL input or unbounded SELECT result.
- Physical disk amplification: B+Tree is immutable and COW; batches of up to
  100 SQL INSERT rows are coalesced to write only the latest reachable pages.
  Updates/deletes still accumulate old versions and tombstones; no vacuum yet.

## Interoperation and protocol

`src/db.cpp` retains Phase 1 parser/typed values and transaction SQL semantics.
`src/disk_store.cpp` owns B+Tree pages and manifest, `include/minivectordb/disk_store.h`
exposes `get`, `scan` (key+record-location only), `read_row`, and `publish`.

All current output is a development-only newline protocol. Do not assume its
error messages or query mode selector match official mysqltest style tests.
