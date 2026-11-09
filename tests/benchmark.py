#!/usr/bin/env python3
"""Local performance + independent sampled recall, not official cgroup benchmark.
Usage: python3 tests/benchmark.py ./build/minivectordb [--n 10000 --d 128 --q 1000]
"""
import argparse
import json
import math
import os
import subprocess
import tempfile
import time
import random
import pathlib

import numpy as np
import psutil

p=argparse.ArgumentParser()
p.add_argument('binary')
p.add_argument('--n',type=int,default=10000)
p.add_argument('--d',type=int,default=128)
p.add_argument('--q',type=int,default=1000)
p.add_argument('--verify',type=int,default=40)
p.add_argument('--output',default=None)
p.add_argument('--search-mode',choices=['exact','ann'],default='exact')
p.add_argument('--create-index',action='store_true')
p.add_argument('--probes',type=int,default=None)
a=p.parse_args()
seed=20261109
rng=np.random.default_rng(seed)
values=rng.uniform(-1,1,(a.n,a.d)).astype(np.float32)
# This avoids trailing precision differences: same FP32 values are passed via SQL and recovered.
queries=rng.uniform(-1,1,(a.q,a.d)).astype(np.float32)
fmt=lambda row: '['+','.join(format(float(x),'.9g') for x in row)+']'
query_inputs=[fmt(q) for q in queries]

with tempfile.TemporaryDirectory(prefix='mvdb_bench_') as data_dir:
    env=os.environ.copy()
    if a.probes is not None:env['MVDB_ANN_PROBES']=str(a.probes)
    process=subprocess.Popen([a.binary,'--data-dir',data_dir,'--search-mode',a.search_mode],env=env,stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,encoding='utf-8',bufsize=1)
    proc=psutil.Process(process.pid)
    rss_peak=0
    def sample_memory():
        nonlocal_vars=None
        return proc.memory_info().rss
    def read_io():
        try:
            return proc.io_counters().write_bytes+proc.io_counters().read_bytes
        except Exception:
            return 0
    def sql(q):
        assert process.poll() is None, ('crashed',process.stderr.read())
        process.stdin.write(q+'\n');process.stdin.flush()
        line=process.stdout.readline().strip()
        if line=='OK':return []
        if not line.startswith('ROWS\t'):raise RuntimeError((q[:80],line))
        count=int(line.split('\t')[1])
        rows=[]
        for _ in range(count):
            fields=process.stdout.readline().strip().split('\t')
            assert fields[0]=='ROW',fields
            rows.append(fields[1:])
        assert process.stdout.readline().strip()=='END'
        return rows
    t0=time.monotonic()
    sql(f'CREATE TABLE items (id BIGINT PRIMARY KEY, groupid BIGINT, emb VECTOR({a.d}))')
    io_before=read_io()
    write_start=time.monotonic()
    for start in range(0,a.n,100):
        batch=[]
        for i in range(start,min(start+100,a.n)):
            batch.append(f'({i},{i%10},{fmt(values[i])})')
        sql('INSERT INTO items VALUES '+','.join(batch))
        if start % 1000 == 0:
            rss_peak=max(rss_peak,sample_memory())
    write_time=time.monotonic()-write_start
    write_io=read_io()-io_before
    rss_peak=max(rss_peak,sample_memory())
    index_build_seconds=0.0
    if a.create_index:
        index_start=time.monotonic()
        sql('CREATE VECTOR INDEX items_idx ON items(emb)')
        index_build_seconds=time.monotonic()-index_start
    # Same cold-start/prepare/measurement sequence should be applied to all variants.
    for j in range(min(30,a.q)):
        sql(f'SELECT id FROM items ORDER BY L2_DISTANCE(emb,\'{query_inputs[j]}\') LIMIT 10')
    query_start=time.monotonic()
    results=[]
    for j in range(a.q):
        filter_sql='WHERE groupid = 3 ' if j%2 else ''
        ret=sql(f'SELECT id FROM items {filter_sql}ORDER BY L2_DISTANCE(emb,\'{query_inputs[j]}\') LIMIT 10')
        results.append([int(row[0]) for row in ret])
        if j%100==0:
            rss_peak=max(rss_peak,sample_memory())
    query_time=time.monotonic()-query_start
    total_io=read_io()-io_before
    rss_peak=max(rss_peak,sample_memory())
    recalled=[[],[]]
    for j in range(min(a.q,a.verify)):
        candidates=np.arange(a.n,dtype=np.int64)
        if j%2:
            candidates=candidates[candidates%10==3]
        diff=values[candidates].astype(np.float64)-queries[j].astype(np.float64)
        norms=np.sqrt(np.einsum('ij,ij->i',diff,diff))
        truth=[int(candidates[k]) for k in np.lexsort((candidates,norms))[:10]]
        got=results[j]
        assert len(got)==len(truth) and len(got)==len(set(got)),(j,got,truth)
        if j%2:
            assert all(pk%10==3 for pk in got)
        recalled[j%2].append(len(set(got)&set(truth))/len(truth))
    process.stdin.write('.exit\n');process.stdin.flush()
    _,error=process.communicate(timeout=10)
    assert process.returncode==0,error
    result={
      'kind':'local_preliminary_not_official','search_mode':a.search_mode,'create_index':a.create_index,'probes':a.probes, 'n':a.n,'dimension':a.d,
      'batch_size':100,'queries':a.q,'k':10,'filtered_queries':a.q//2,
      'write_seconds':round(write_time,6),'index_build_seconds':round(index_build_seconds,6),'query_seconds':round(query_time,6),
      'query_qps':round(a.q/query_time,2),
      'write_committed_rows_per_sec':round(a.n/write_time,2),
      'observed_process_io_bytes_total':total_io,
      'observed_process_io_bytes_write_phase':write_io,
      'sampled_process_rss_peak_bytes':rss_peak,
      'wal_size_bytes':(pathlib.Path(data_dir)/'wal.mvdb').stat().st_size,
      'heap_size_bytes':(pathlib.Path(data_dir)/'heap.mvdb').stat().st_size if (pathlib.Path(data_dir)/'heap.mvdb').exists() else 0,
      'btree_file_size_bytes':(pathlib.Path(data_dir)/'index.mvdb').stat().st_size if (pathlib.Path(data_dir)/'index.mvdb').exists() else 0,
      'sampled_recall_at_10_no_filter':round(float(np.mean(recalled[0])),6) if recalled[0] else None,
      'sampled_recall_at_10_filter':round(float(np.mean(recalled[1])),6) if recalled[1] else None,
      'recall_queries_checked':len(recalled[0])+len(recalled[1]),
      'notes':'/proc process I/O and RSS are not cgroup v2 io.stat or memory.peak. '+('Disk B+Tree with bounded caches (ANN absent).' if (pathlib.Path(data_dir)/'index.mvdb').exists() else 'Phase 1 in-memory row map (ANN and disk index absent).'),
    }
    print(json.dumps(result,indent=2,ensure_ascii=False))
    if a.output:pathlib.Path(a.output).write_text(json.dumps(result,indent=2,ensure_ascii=False)+'\n',encoding='utf-8')
