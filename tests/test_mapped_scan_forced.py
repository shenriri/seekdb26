#!/usr/bin/env python3
"""Small bounded fixture forces mmap path for ASan/UBSan coverage.
The opt-in MVDB_TEST_FORCE_MAPPED flag is forbidden in scoring measurements.
"""
import os, pathlib, random, subprocess, sys, tempfile
exe=str(pathlib.Path(sys.argv[1]).resolve())
rng=random.Random(3011)
dim=64

def v(vs):return '['+','.join(format(float(x),'.6g') for x in vs)+']'
vs=[v([rng.uniform(-3,3) for _ in range(dim)]) for _ in range(155)]
queries=[]
for j in range(36):
 q=v([rng.uniform(-3,3) for _ in range(dim)])
 clause=('',"WHERE cat = '甲' ",'WHERE g=3 ',"WHERE cat >= '乙' AND g <= 2 ")[j%4]
 queries.append(f"SELECT id, L2_DISTANCE(emb,'{q}') AS distance FROM t {clause}ORDER BY distance LIMIT 10")
class Client:
 def __init__(self,path,mapped):
  env=os.environ.copy();env['MVDB_TEST_FORCE_MAPPED']='1' if mapped else '0';env['MVDB_MAPPED_SCAN']='1' if mapped else '0'
  self.p=subprocess.Popen([exe,'--data-dir',path],env=env,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,encoding='utf-8',bufsize=1)
 def sql(self,q):
  self.p.stdin.write(q+'\n');self.p.stdin.flush()
  head=self.p.stdout.readline().strip()
  if head=='OK':return ('OK',)
  if head.startswith('ERR\t'):return ('ERR',head)
  assert head.startswith('ROWS\t'),(q,head)
  rows=[self.p.stdout.readline().rstrip('\n') for _ in range(int(head.split('\t')[1]))]
  assert self.p.stdout.readline().strip()=='END'
  return ('ROWS',tuple(rows))
 def close(self):
  self.p.stdin.write('.exit\n');self.p.stdin.flush()
  error=self.p.communicate(timeout=15)[1]
  assert self.p.returncode==0,error

def run(mapped):
 with tempfile.TemporaryDirectory(prefix='mvdb_force_map_') as p:
  c=Client(p,mapped);res=[]
  res.append(c.sql('CREATE TABLE t (id BIGINT PRIMARY KEY, cat VARCHAR(8), g BIGINT, emb VECTOR(64))'))
  for first in range(0,150,25):
   rows=','.join(f"({i},'{'甲' if i%2 else '乙'}',{i%7},'{vs[i]}')" for i in range(first,first+25))
   res.append(c.sql('INSERT INTO t VALUES '+rows))
  res.extend(c.sql(q) for q in queries)
  res += [c.sql('BEGIN'),c.sql('UPDATE t SET g=3 WHERE id=13')]
  res.extend(c.sql(q) for q in queries[:4])
  res.append(c.sql('ROLLBACK'))
  res += [c.sql("UPDATE t SET cat='甲' WHERE id=14"),c.sql('DELETE FROM t WHERE id=17'),
          c.sql(f"INSERT INTO t VALUES (151,'乙',3,'{vs[150]}')")]
  res.extend(c.sql(q) for q in queries)
  c.close()
  c=Client(p,mapped)
  res.extend(c.sql(q) for q in queries[:14]);c.close()
  return res
base=run(False);mapped=run(True)
assert base==mapped,next(((i,a,b) for i,(a,b) in enumerate(zip(base,mapped)) if a!=b),('length mismatch',len(base),len(mapped)))
print('PASS forced mmap vs ordinary exact: 155-row UTF8 filters, 64D L2, rollback, commit invalidation, delete, insert, restart')
