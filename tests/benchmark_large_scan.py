#!/usr/bin/env python3
"""Batched load once; replay identical queries against legacy and mapped exact scanners.
Local proof of correctness/performance, not official performance scoring.
"""
import argparse, json, os, pathlib, random, subprocess, tempfile, time, struct

p=argparse.ArgumentParser()
p.add_argument('exe')
p.add_argument('--rows',type=int,default=25000)
p.add_argument('--d',type=int,default=128)
p.add_argument('--queries',type=int,default=20)
p.add_argument('--output',default='tests/large_scan_ab.json')
p.add_argument('--varchar', action='store_true')
p.add_argument('--filtered-only', action='store_true')
p.add_argument('--posting-ab', action='store_true')
p.add_argument('--mapped-only', action='store_true')
a=p.parse_args()
exe=str(pathlib.Path(a.exe).resolve())

class Client:
 def __init__(self,dir,enabled,postings=True):
  env=os.environ.copy();env['MVDB_MAPPED_SCAN']='1' if enabled else '0';env['MVDB_MMAP_FILTER_INDEX']='1' if postings else '0'
  self.p=subprocess.Popen([exe,'--data-dir',dir],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,encoding='utf-8',text=True,env=env,bufsize=1)
 def sql(self,s):
  self.p.stdin.write(s+'\n');self.p.stdin.flush()
  line=self.p.stdout.readline().strip()
  if line=='OK':return []
  if not line.startswith('ROWS\t'):raise RuntimeError((s[:100],line))
  n=int(line.split('\t')[1]);rows=[self.p.stdout.readline().rstrip('\n').split('\t')[1:] for _ in range(n)]
  assert self.p.stdout.readline().strip()=='END'
  return rows
 def finish(self):
  self.p.stdin.write('.exit\n');self.p.stdin.flush()
  error=self.p.communicate(timeout=10)[1]
  assert self.p.returncode==0,error

rng=random.Random(712)
patterns=[[round(rng.uniform(-1,1),5) for _ in range(a.d)] for j in range(200)]
fmt=lambda r:'['+','.join(map(str,r))+']'
vec=[fmt(r) for r in patterns]
queries=[];qvectors=[]
for j in range(a.queries):
  qvals=[round(rng.uniform(-1,1),5) for _ in range(a.d)]
  qvectors.append(qvals)
  v=fmt(qvals)
  if a.filtered_only: predicate="WHERE grp = 3 "
  elif a.varchar and j%3==0: predicate="WHERE tag = 'red' AND grp = 3 "
  elif a.varchar and j%3==1: predicate="WHERE tag >= 'blue' "
  else: predicate='WHERE grp = 3 ' if j%2 else ''
  queries.append(f'SELECT id FROM items {predicate}'+f"ORDER BY L2_DISTANCE(emb,'{v}') LIMIT 10")

with tempfile.TemporaryDirectory(prefix='mvdb_mmap_ab_') as d:
 client=Client(d,True)
 client.sql(f'CREATE TABLE items (id BIGINT PRIMARY KEY, grp BIGINT, '+('tag VARCHAR(8), ' if a.varchar else '')+f'emb VECTOR({a.d}))')
 start=time.perf_counter()
 for i in range(0,a.rows,100):
  rows=','.join('('+str(k)+','+str(k%10)+(','+"'red'" if k%4 in (0,1) else ','+"'blue'")*(1 if a.varchar else 0)+",'"+vec[k%200]+"')" for k in range(i,min(a.rows,i+100)))
  client.sql('INSERT INTO items VALUES '+rows)
 write_time=time.perf_counter()-start
 client.finish()
 data=[]
 # Independently reopen the same committed database for each variant.
 for j,enabled in enumerate(([True,True] if a.mapped_only else ([True,True,True,True] if a.posting_ab else [False,True,False,True]))):
  posting=(j%2==1) if (a.posting_ab or a.mapped_only) else True
  client=Client(d,enabled,postings=posting)
  if a.posting_ab or a.mapped_only: client.sql(queries[0]) # isolate steady-state posting benefit
  begin=time.perf_counter()
  answers=[client.sql(s) for s in queries]
  elapsed=time.perf_counter()-begin
  mem=pathlib.Path(f'/proc/{client.p.pid}/status').read_text()
  rss=int(next(x.split()[1] for x in mem.splitlines() if x.startswith('VmRSS:')))*1024
  client.finish()
  data.append({'mapped':enabled,'postings':posting,'queries':len(answers),'elapsed':elapsed,'rss_sample':rss,'answers':answers})
 assert all(v['answers']==data[0]['answers'] for v in data[1:]),'MAPPED_EXACT_RESULT_MISMATCH'
 # Independent full exact TopK oracle for this deterministic repeated-vector fixture.
 # Every base pattern repeats at PK+200; keep its first ten repeats to
 # reproduce tie ordering, even for a data set far beyond the row cache.
 checked=0
 if a.rows>=200 and a.d<=256 and a.queries<=100:
  f32=lambda x:struct.unpack('<f',struct.pack('<f',float(x)))[0]
  vectors32=[[f32(x) for x in v] for v in patterns]
  for j,qv in enumerate(qvectors):
   q32=[f32(x) for x in qv]
   ranking=[]
   for base in range(200):
    if a.filtered_only: eligible=(base%10==3)
    elif a.varchar and j%3==0: eligible=(base%4 in (0,1) and base%10==3)
    elif a.varchar and j%3==1: eligible=True
    else: eligible=(base%10==3) if j%2 else True
    if not eligible:continue
    dist=0.0
    for x,y in zip(vectors32[base],q32):
     dv=x-y;dist+=dv*dv
    for pk in range(base,min(a.rows,base+200*10),200):ranking.append((dist,pk))
   expected=[x[1] for x in sorted(ranking)[:10]]
   observed=[int(x[0]) for x in data[0]['answers'][j]]
   assert expected==observed,('INDEPENDENT_TOPK_ORACLE',j,expected,observed)
   checked+=1
 # Modification and remapping after commit, with cross-check against old path.
 if not a.mapped_only:
  after=[]
  for enabled in [False,True]:
   client=Client(d,enabled)
   # Read the exact same post-commit contents without changing the stored DB.
   ans=[client.sql(s) for s in queries[:min(4,len(queries))]]
   client.finish();after.append(ans)
  assert after[0]==after[1]
  # Build the mapped cursor, then mutate; stale pointers and postings must be invalidated.
  updater=Client(d,True)
  updater.sql(queries[0])
  new_vector=fmt([0.0]*a.d)
  updater.sql(f"UPDATE items SET emb = '{new_vector}', grp = 3 WHERE id = 13")
  updater.sql('DELETE FROM items WHERE id = 24')
  updater.sql(f"INSERT INTO items VALUES ({a.rows},3,"+ ("'red'," if a.varchar else '')+f"'{new_vector}')")
  changed=[updater.sql(s) for s in queries]
  updater.finish()
  unchanged=Client(d,False)
  changed_reference=[unchanged.sql(s) for s in queries]
  unchanged.finish()
  assert changed==changed_reference,'STALE_MAPPED_CACHE_AFTER_COMMIT'
  reopened=Client(d,True)
  reopened_results=[reopened.sql(s) for s in queries]
  reopened.finish()
  assert changed==reopened_results,'MAPPED_CACHE_REOPEN_MISMATCH'
 output={'kind':'local_large_scan_ab_nonofficial','n':a.rows,'d':a.d,'varchar':a.varchar,'filtered_only':a.filtered_only,'posting_ab':a.posting_ab,'mapped_only':a.mapped_only,'queries':a.queries,'write_seconds':write_time,
  'samples':[{k:v for k,v in x.items() if k!='answers'} for x in data],
  'ratio_from_medians':(sum(x['elapsed'] for x in data if (not x['postings'] if (a.posting_ab or a.mapped_only) else not x['mapped'])))/(
   sum(x['elapsed'] for x in data if (x['postings'] if (a.posting_ab or a.mapped_only) else x['mapped']))),
  'output_matching':True,'independent_exact_queries_checked':checked,'heap_bytes':next(pathlib.Path(d).glob('heap*.mvdb')).stat().st_size}
 print(json.dumps(output,indent=2));pathlib.Path(a.output).write_text(json.dumps(output,indent=2)+'\n')
