# MiniVectorDB — official JSONL implementation

C++17, single process/client, self-authored SQL executor, disk COW B+Tree, bounded
caches, WAL/recovery, transactions and IVF-Flat. Database files stay under /data.
This version incorporates the published protocol at official commit f4d4e26653af823a7601668802f80e374ad6643c.

## Build and run

```bash
docker build -t mvdb:runtime .
docker run --rm -i --network none --cpus 2 --memory 2g --memory-swap 2g \
  --pids-limit 128 --mount type=volume,src=mvdb-data,dst=/data mvdb:runtime
```

Default input/output is the JSONL protocol in [STDIO_PROTOCOL.md](STDIO_PROTOCOL.md).
After recovery the program proactively emits `{"type":"ready"}` and flushes.
It accepts execute requests with id/sql/search_mode, and shutdown requests;
id types, typed rows, columns, affected_rows and structured errors are returned.
Logs go to stderr. No network listener or TTY is required.

```json
{"id":1,"op":"execute","sql":"CREATE TABLE docs (id BIGINT PRIMARY KEY, content VARCHAR(16));","search_mode":"exact"}
{"id":2,"op":"execute","sql":"INSERT INTO docs VALUES (1,'你好');","search_mode":"exact"}
{"id":3,"op":"execute","sql":"SELECT id,content FROM docs;","search_mode":"exact"}
{"id":4,"op":"shutdown"}
```

Send one physical JSON line, wait for one response. Reuse the same volume after
shutdown to check persistence. Per-request exact/ann switches retain transaction
state. Default exact is also the measured performance mode.

## Official published checks (Linux / WSL with Docker CLI)

```bash
python3 protocol_check/run.py --image mvdb:runtime --handshake-only --output protocol-results/handshake-1
python3 protocol_check/run.py --image mvdb:runtime --output protocol-results/public-1
```

Each output directory must be new. The official checker is retained unchanged;
it uses POSIX pipe selectors, so Windows users should run it under Linux/WSL.
Read protocol_check/README.md for configuration and interpretation.

## Results recorded 2026-11-10

- Actual production image: published handshake PASS, 20/20 public groups / 95/95 SQL PASS.
- CTest: 16/16, including JSON types, modes within a transaction, recovery and failure paths.
- 200K x 128D / 109,400,000-byte Heap under hard 64MiB cgroup: passed, no OOM, restart checked.
- Three interleaved JSONL A/B rounds: median write 2.859s -> 2.469s,
  query 0.922s -> 0.828s, both Recall@10 groups 1.0 in every round.
  Every query and all rows after restart checked. Single-round variance is visible.
- This is public compatibility and local performance evidence, NOT all 29 formal cases or an official score.

See [design, revised approach and measurements](docs/JSONL_DESIGN_AND_VALIDATION.md)
and validation/20261110/. Base digests, direct package versions and vendored JSON
library checksum are in DEPENDENCIES.lock.json. MIT serialization-library license
is retained at include/nlohmann/LICENSE.MIT; no external database/vector engine is used.

## Additional regression and performance checks

```bash
docker build -f Dockerfile.eval -t mvdb:verify .
docker run --rm --network none --cpus 2 --memory 2g --memory-swap 2g --pids-limit 128 mvdb:verify
python3 tests/docker_benchmark_json.py --image mvdb:runtime --rounds 3 --verify 1000 --output benchmark.json
```

The older text interface remains available only via explicit --protocol legacy
or the test binary minivectordb_legacy. Historical text benchmarks must target
that binary; production evaluation uses default JSONL. Old API adapters and
20261109 reports are historical development checks.

Target branch: 2026-competition-round2; learning evidence: agent/conversation.json.
Known limits: in-memory pending write set and 64MiB serialized transaction cap;
unbounded result materialization and all hidden admission cases still need review.
No formal platform evaluation was initiated by this upload.
