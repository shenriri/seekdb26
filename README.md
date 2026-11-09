# MiniVectorDB — Phase6 submission candidate

C++17 single-process, single-client SQL/vector database. Self-authored SQL execution,
disk copy-on-write B+Tree, bounded caches, WAL/recovery, transactions and IVF-Flat.
Persistent database files are stored under `/data`. No network listener is used.

## Build and run

```bash
docker build -t mvdb:runtime .
docker run --rm -i --network none --cpus 2 --memory 2g --memory-swap 2g \
  --mount type=volume,src=mvdb-data,dst=/data mvdb:runtime
```

The resource limits above are local examples, not organizer-frozen limits.
The Dockerfile is the runtime recipe successfully exercised on Windows Docker
Desktop Linux amd64; it uses a signed Ubuntu mirror and includes C++ runtime libraries.
Image digests and full dependency versions still require the organizer's freeze.

Current developer protocol: one SQL statement per UTF-8 line, flushed responses
`OK`, `ERR\t<engine error>`, or `ROWS\tN`, N `ROW` lines, then `END`.
Fields escape backslash/tab/CR/LF. `.exit`, `\q`, or stdin EOF close normally and
discard uncommitted transactions. Select exact/ann at startup with
`--search-mode exact|ann`; exact is the default. This protocol has NOT been
certified against an organizer stdin/stdout driver. Production output currently
does not expose columns or affected_rows; those are verified through a development API adapter.

```sql
CREATE TABLE docs (id BIGINT PRIMARY KEY, text VARCHAR(128), emb VECTOR(2));
INSERT INTO docs VALUES (1,'hello','[3,4]');
SELECT id,text FROM docs ORDER BY id;
SELECT id,L2_DISTANCE(emb,'[0,0]') AS distance FROM docs ORDER BY distance LIMIT 10;
BEGIN;
UPDATE docs SET text='committed' WHERE id=1;
COMMIT;
BEGIN;
DELETE FROM docs WHERE id=1;
ROLLBACK;
```

Send `.exit`, then rerun the container with the same mvdb-data volume to verify persistence.
SQL/type rules and implementation history are in `docs/`.

## Actual local validation, 2026-11-09

- Current official public-v4: 20/20 groups, 95/95 SQL via development API adapter.
- Docker Release CTest: 15/15; production smoke/restart and 21 transport checks passed.
- Three exact rounds: 10K x 128 vectors, 1000 queries each, half filtered; all
  queries independently checked, Recall@10=1 for both groups in every round.
- Median: write 2.437s, query 0.859s, 1164.14 QPS; container peak median 36,241,408 bytes.
- These are local results, NOT formal admission, an official score, or an ANN performance result.

Raw relevant results: `validation/20261109/`. Older observations under tests/docs
are historical and may describe earlier development environments.

## Reproduce correctness checks

```bash
docker build -f Dockerfile.eval -t mvdb:verify .
docker run --rm --network none --cpus 2 --memory 2g --memory-swap 2g mvdb:verify
docker build -f Dockerfile.public-v4 -t mvdb:public-v4 .
docker run --rm --network none --cpus 2 --memory 2g --memory-swap 2g mvdb:public-v4
```

`mysqltest/` is the upstream v4 suite; `official_mysqltest/` retains the older
five-case suite used by the original CTest. The v4 adapter overlay adds only an
in-process ANN-mode setter, without changing core algorithms or production code.

## Submission and outstanding checks

Target branch: `2026-competition-round2`; learning material: `agent/conversation.json`.
Core source, runtime Dockerfile and learning material belong to the same commit.
The empty upstream root conversation.json placeholder is removed to retain one material file.

Formal wire-format validation awaits the organizer protocol/driver. Large
transactions remain in memory and serialized commits are capped at 64MiB;
datasets exceeding the cgroup memory budget need further validation. Actual
low-probe IVF recall is not certified; exact is the measured performance mode.
No platform formal assessment has been requested by this repository upload.
