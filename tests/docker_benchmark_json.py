#!/usr/bin/env python3
"""Local Docker cgroup-v2 benchmark of the PRODUCTION MiniVectorDB image.

This is a reproducible experimental runner, NOT the competition's private runner.
The driver runs on the host; only the DB process runs in the container.
Reads memory.peak and io.stat from the DB container cgroup via docker exec.
Requires local Docker and NumPy on the host.
"""
import argparse
import json
import os
from pathlib import Path
import statistics
import subprocess
import sys
import tempfile
import time
import uuid

try:
    import numpy as np
except ImportError:
    raise SystemExit('Missing NumPy on host: python3 -m pip install numpy')


def call(*args, check=True, timeout=30):
    return subprocess.run(list(args), text=True, encoding='utf-8', errors='replace',
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          check=check, timeout=timeout)


def metrics(name):
    # If the host uses cgroup v1 or its Docker runtime does not expose the
    # cgroup-v2 files, report missing measurements rather than inventing them.
    cmd = ('cat /sys/fs/cgroup/memory.peak 2>/dev/null || echo unavailable; '
           'echo __IOSTAT__; '
           'cat /sys/fs/cgroup/io.stat 2>/dev/null || echo unavailable')
    output = call('docker', 'exec', name, '/bin/sh', '-c', cmd, timeout=20).stdout
    parts = output.split('__IOSTAT__', 1)
    memory_txt = parts[0].strip()
    peak = int(memory_txt) if memory_txt.isdigit() else None
    total_r = total_w = 0
    valid = False
    for line in parts[1].strip().splitlines() if len(parts) > 1 else []:
        tokens = line.split()
        values = {}
        for item in tokens[1:]:
            if '=' in item:
                k, v = item.split('=', 1)
                values[k] = v
        if 'rbytes' in values and 'wbytes' in values:
            total_r += int(values['rbytes'])
            total_w += int(values['wbytes'])
            valid = True
    return {'memory_peak_bytes': peak,
            'io_read_bytes': total_r if valid else None,
            'io_write_bytes': total_w if valid else None}


def io_delta(start, end):
    a, b = start['io_read_bytes'], end['io_read_bytes']
    c, d = start['io_write_bytes'], end['io_write_bytes']
    return {'read_bytes': max(0, b - a) if a is not None and b is not None else None,
            'write_bytes': max(0, d - c) if c is not None and d is not None else None}


def run_once(args, iteration):
    seed = 20261109
    rng = np.random.default_rng(seed)
    values = rng.uniform(-1, 1, (args.n, args.d)).astype(np.float32)
    queries = rng.uniform(-1, 1, (args.q, args.d)).astype(np.float32)
    fmt = lambda row: '['+','.join(format(float(v), '.9g') for v in row)+']'
    query_inputs = [fmt(q) for q in queries]
    name = 'mvdb-json-' + uuid.uuid4().hex[:12]
    volume_name = name + '-data'
    call('docker', 'volume', 'create', volume_name)
    with tempfile.TemporaryDirectory(prefix='mvdb_docker_eval_') as data_dir:
        cmd = ['docker', 'run', '--rm', '-i', '--read-only', '--name', name,
               '--network', 'none', '--cpus', str(args.cpus),
               '--memory', args.memory, '--memory-swap', args.memory,
               '--pids-limit', str(args.pids),
               '--mount', f'type=volume,source={volume_name},target=/data']
        if args.search_mode == 'ann':
            cmd += ['--env', f'MVDB_ANN_PROBES={args.probes}']
        cmd += [args.image, '--search-mode', args.search_mode]
        p = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                             stderr=subprocess.PIPE, text=True, encoding='utf-8', bufsize=1)
        assert json.loads(p.stdout.readline()) == {'type': 'ready'}
        p.stdin.reconfigure(newline='\n')
        sequence = 0
        def sql(statement):
            nonlocal sequence
            sequence += 1
            request = {'id': sequence, 'op': 'shutdown' if statement == '.exit' else 'execute'}
            if statement != '.exit':
                request.update(sql=statement, search_mode=args.search_mode)
            p.stdin.write(json.dumps(request, ensure_ascii=False, allow_nan=False)+'\n')
            p.stdin.flush()
            response = json.loads(p.stdout.readline())
            if type(response.get('id')) is not int or response['id'] != sequence or response.get('ok') is not True:
                raise RuntimeError(f'JSONL request failed: {response!r}')
            return response.get('rows')
        try:
            sql(f'CREATE TABLE items (id BIGINT PRIMARY KEY, groupid BIGINT, emb VECTOR({args.d}))')
            snap0 = metrics(name)
            start = time.monotonic()
            for i in range(0, args.n, 100):
                batch = [f'({j},{j%10},{fmt(values[j])})'
                         for j in range(i, min(i+100, args.n))]
                sql('INSERT INTO items VALUES ' + ','.join(batch))
            write_seconds = time.monotonic() - start
            snap1 = metrics(name)
            prep_start = time.monotonic()
            if args.create_index:
                sql('CREATE VECTOR INDEX items_idx ON items(emb)')
            for j in range(min(30, args.q)):
                sql(f"SELECT id FROM items ORDER BY L2_DISTANCE(emb, '{query_inputs[j]}') LIMIT 10")
            prep_seconds = time.monotonic() - prep_start
            snap2 = metrics(name)
            results = []
            start = time.monotonic()
            for j in range(args.q):
                predicate = 'WHERE groupid = 3 ' if j%2 else ''
                records = sql(f"SELECT id FROM items {predicate}ORDER BY L2_DISTANCE(emb, '{query_inputs[j]}') LIMIT 10")
                results.append([int(row[0]) for row in records])
            query_seconds = time.monotonic() - start
            call('docker', 'exec', name, '/bin/sync')
            snap3 = metrics(name)
            # Independent, host-side FP32-grounded exact truth. Not timed into DB query.
            recalled = [[], []]
            for j in range(min(args.verify, args.q)):
                ids = np.arange(args.n, dtype=np.int64)
                if j%2:
                    ids = ids[ids % 10 == 3]
                diff = values[ids].astype(np.float64) - queries[j].astype(np.float64)
                distances = np.sqrt(np.einsum('ij,ij->i', diff, diff))
                truth = [int(ids[k]) for k in np.lexsort((ids, distances))[:10]]
                actual = results[j]
                if len(actual) != len(truth) or len(actual) != len(set(actual)):
                    raise AssertionError(f'Duplicate or wrong count: query={j} got={actual!r}')
                if any(x < 0 or x >= args.n for x in actual):
                    raise AssertionError('invalid returned primary key')
                if j%2 and any(x % 10 != 3 for x in actual):
                    raise AssertionError(f'Wrong filter: query={j} got={actual!r}')
                recalled[j%2].append(len(set(actual) & set(truth))/len(truth))
            sql('.exit')
            _, stderr = p.communicate(timeout=30)
            if p.returncode:
                raise RuntimeError(f'DB/container exit code={p.returncode}; stderr={stderr[:1500]}')
            # Validate the full written data after shutdown in a separate, untimed container/cgroup.
            request = {'id': 'persist', 'op': 'execute', 'sql': 'SELECT id,groupid,emb FROM items ORDER BY id;', 'search_mode': 'exact'}
            requests = json.dumps(request)+'\n'+json.dumps({'id':'stop','op':'shutdown'})+'\n'
            check_run = subprocess.run(['docker','run','--rm','-i','--read-only','--network','none',
                '--cpus',str(args.cpus),'--memory',args.memory,'--memory-swap',args.memory,
                '--pids-limit',str(args.pids),'--mount',f'type=volume,source={volume_name},target=/data',args.image],
                input=requests.encode(), capture_output=True, timeout=90)
            assert check_run.returncode == 0, check_run.stderr
            lines = [json.loads(line) for line in check_run.stdout.splitlines()]
            assert len(lines) == 3 and lines[0] == {'type':'ready'} and lines[1]['ok'] is True and lines[2] == {'id':'stop','ok':True}
            stored = lines[1]['rows']
            assert len(stored) == args.n
            for index,row in enumerate(stored):
                assert row[:2] == [index,index%10]
                assert np.array_equal(np.asarray(row[2],dtype=np.float32),values[index])
            result = {
                'round': iteration, 'rows': args.n, 'dimension': args.d,
                'data_volume': volume_name, 'seed': seed,
                'cgroup_snapshots': [snap0, snap1, snap2, snap3],
                'queries': args.q, 'filtered_queries': args.q//2, 'k': 10,
                'search_mode': args.search_mode, 'created_vector_index': args.create_index,
                'write_seconds': round(write_seconds, 6),
                'query_prepare_seconds': round(prep_seconds, 6),
                'query_seconds': round(query_seconds, 6),
                'query_qps': round(args.q/query_seconds, 2),
                'io_write_phase': io_delta(snap0, snap1),
                'io_index_and_warmup': io_delta(snap1, snap2),
                'io_query_phase': io_delta(snap2, snap3),
                'io_total_after_create': io_delta(snap0, snap3),
                'cgroup_v2_memory_peak_bytes': snap3['memory_peak_bytes'],
                'sampled_recall_at_10_unfiltered': float(np.mean(recalled[0])) if recalled[0] else None,
                'sampled_recall_at_10_filtered': float(np.mean(recalled[1])) if recalled[1] else None,
                'sampled_recall_queries': len(recalled[0])+len(recalled[1]),
                'errors': None,
            }
            return result
        finally:
            if p.poll() is None:
                p.kill()
                try: p.communicate(timeout=5)
                except Exception: pass
            # --rm removes it; cleanup if docker run exits abnormally.
            subprocess.run(['docker','rm','-f',name], stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL, timeout=10, check=False)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--image', default='mvdb:runtime')
    ap.add_argument('--cpus', type=float, default=2.0)
    ap.add_argument('--memory', default='2g')
    ap.add_argument('--pids', type=int, default=128)
    ap.add_argument('--n', type=int, default=10000)
    ap.add_argument('--d', type=int, default=128)
    ap.add_argument('--q', type=int, default=1000)
    ap.add_argument('--verify', type=int, default=100)
    ap.add_argument('--rounds', type=int, default=3)
    ap.add_argument('--search-mode', choices=('exact','ann'), default='exact')
    ap.add_argument('--create-index', action='store_true')
    ap.add_argument('--probes', type=int, default=8)
    ap.add_argument('--output', default='reports/docker_benchmark.json')
    args = ap.parse_args()
    if min(args.n, args.d, args.q, args.rounds) <= 0 or args.verify < 0:
        ap.error('n,d,q,rounds must be positive and verify must be nonnegative')
    try:
        call('docker','info',timeout=20)
    except (FileNotFoundError, subprocess.CalledProcessError, subprocess.TimeoutExpired):
        raise SystemExit('Docker daemon is unavailable; cannot claim container benchmark results')
    data = {'classification': 'local_JSONL_production_cgroup_v2_NOT_official_scoring',
            'image': args.image,
            'local_limits_not_organizer_verified': {'cpus': args.cpus, 'memory': args.memory, 'pids': args.pids},
            'notes': [
                'Uses official JSONL framing; organizer protocol_check is run separately. All written rows are checked after restart outside timing.',
                'Container cgroup-v2 memory.peak includes page cache and process memory (unlike host process RSS).',
                'Cgroup io.stat may depend on storage driver, filesystem, block-device accounting and writeback.',
                'Index preparation time excluded from query_seconds; its IO and memory included in total.',
                'NumPy truth checking runs on host after timing and does not contribute to DB container resources.',
                'Results are not an official score: official baseline and hidden workloads are unavailable.'
            ], 'rounds': []}
    try:
        for i in range(args.rounds):
            row = run_once(args, i+1)
            data['rounds'].append(row)
            print(f"Round {i+1}: write={row['write_seconds']:.3f}s "
                  f"query={row['query_seconds']:.3f}s "
                  f"mem.peak={row['cgroup_v2_memory_peak_bytes']} B ", flush=True)
        data['median'] = {key: statistics.median([round_data[key] for round_data in data['rounds']])
                          for key in ['write_seconds','query_seconds','query_qps']}
        mems=[d['cgroup_v2_memory_peak_bytes'] for d in data['rounds']]
        data['median']['cgroup_memory_peak_bytes'] = statistics.median(mems) if all(x is not None for x in mems) else None
        b=[]
        for row in data['rounds']:
            io=row['io_total_after_create']
            b.append(io['read_bytes'] + io['write_bytes'] if io['read_bytes'] is not None and io['write_bytes'] is not None else None)
        data['median']['cgroup_io_total_bytes'] = statistics.median(b) if all(x is not None for x in b) else None
    except Exception as exc:
        data['incomplete_error']=str(exc)
        print(f'INCOMPLETE: {exc}',file=sys.stderr)
        raise
    finally:
        if args.output:
            path=Path(args.output)
            path.parent.mkdir(parents=True,exist_ok=True)
            path.write_text(json.dumps(data,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    print('Saved:',args.output)

if __name__ == '__main__':
    main()
