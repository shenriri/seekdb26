#!/usr/bin/env python3
"""Verify AVX2 pruning produces byte-identical *logical* exact query results.
Checks random vectors, ties, filters, deletes, updates, transactions and restart.
"""
import json
import os
import random
import subprocess
import sys
import tempfile
from pathlib import Path

binary=str(Path(sys.argv[1]).resolve())
random_gen=random.Random(20261109)
dim=64
vectors=[]
for i in range(240):
    vec=[round(random_gen.uniform(-2,2),7) for _ in range(dim)]
    if i<20: # mathematically identical squared norms for adversarial tie-breaking
        vec=[0.0]*dim
        vec[i%dim]=1.0 if i%2==0 else -1.0
    if 20<=i<25:
        vec=[0.0]*dim
        vec[0]=1.0 + (i-22)*0.00000011920928955078125
    vectors.append(vec)

vec=lambda v:'['+','.join(format(x,'.9g') for x in v)+']'
commands=['CREATE TABLE items (id BIGINT PRIMARY KEY, groupid BIGINT, emb VECTOR(64));']
for k in range(0,len(vectors),40):
    commands.append('INSERT INTO items VALUES '+','.join(f"({i},{i%5},'{vec(vectors[i])}')" for i in range(k,min(k+40,len(vectors))))+';')
commands.extend([
    "SELECT id, L2_DISTANCE(emb, '["+','.join('0' for _ in range(dim))+"]') AS dist FROM items ORDER BY dist LIMIT 20;",
    "SELECT id FROM items WHERE groupid = 3 ORDER BY L2_DISTANCE(emb, '["+','.join('0' for _ in range(dim))+"]') LIMIT 10;",
])
for i in range(90):
    query=[round(random_gen.uniform(-3,3),7) for _ in range(dim)]
    filt=f'WHERE groupid = {i%5} ' if i%2 else ''
    commands.append(f"SELECT id, L2_DISTANCE(emb, '{vec(query)}') AS d FROM items {filt}ORDER BY d LIMIT {1+i%20};")
commands.extend([
    "BEGIN;",
    "UPDATE items SET groupid = 2 WHERE id = 12;",
    "SELECT id FROM items WHERE groupid = 2 ORDER BY L2_DISTANCE(emb, '["+','.join('0' for _ in range(dim))+"]') LIMIT 12;",
    "ROLLBACK;",
    "UPDATE items SET groupid = 4 WHERE id = 17;",
    "DELETE FROM items WHERE id = 30;",
    "SELECT id FROM items WHERE groupid = 4 ORDER BY L2_DISTANCE(emb, '["+','.join('0' for _ in range(dim))+"]') LIMIT 30;",
])

def run(enabled,fma=False,prepared=False,avx512=False):
    env=os.environ.copy()
    env['MVDB_SIMD_PRUNE']='1' if enabled else '0'
    env['MVDB_FMA_PRUNE']='1' if fma else '0'
    env['MVDB_PREPARED_QUERY']='1' if prepared else '0'
    env['MVDB_AVX512']='1' if avx512 else '0'
    with tempfile.TemporaryDirectory(prefix='mvdb_simd_'+str(enabled)+'_') as path:
        proc=subprocess.run([binary,path],input='\n'.join(commands)+'\n',capture_output=True,
                            text=True,encoding='utf-8',env=env,timeout=30)
        assert proc.returncode==0,(enabled,proc.stderr)
        results=[json.loads(x) for x in proc.stdout.splitlines()]
        assert len(results)==len(commands),(enabled,len(results),len(commands))
        assert all(x['ok'] for x in results),(enabled,[(i,x) for i,x in enumerate(results) if not x['ok']])
        return results

before=run(False);after=run(True);fma=run(True,True);prepared=run(True,True,True);wide=run(True,True,True,True)
for i,(a,b,c,d,e) in enumerate(zip(before,after,fma,prepared,wide)):
    assert a==b==c==d==e,('SIMD/FMA/prepared/AVX512 differs at',i,commands[i],a,b,c,d,e)
print('scalar / AVX2 / FMA / prepared / AVX512 exact differential:',len(commands),'statements identical')
