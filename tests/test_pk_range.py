#!/usr/bin/env python3
"""Independent differential test of accelerated bounded B+Tree PK scans.
Test both ascending and descending traversal, inclusive boundaries and txn overlays.
"""
import os, pathlib, random, shutil, subprocess, sys, tempfile
exe=str(pathlib.Path(sys.argv[1]).resolve())
class C:
 def __init__(self,dir,fast):
  env=os.environ.copy();env['MVDB_PK_FASTPATH']='1' if fast else '0'
  self.p=subprocess.Popen([exe,'--data-dir',dir],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,encoding='utf-8',text=True,env=env,bufsize=1)
 def sql(self,s):
  self.p.stdin.write(s+'\n');self.p.stdin.flush()
  a=self.p.stdout.readline().strip()
  if a=='OK':return ('ok',[])
  if a.startswith('ERR\t'):return ('error',a)
  assert a.startswith('ROWS\t'),(s,a,self.p.poll())
  vals=[self.p.stdout.readline().strip() for _ in range(int(a.split('\t')[1]))]
  assert self.p.stdout.readline().strip()=='END'
  return ('rows',vals)
 def stop(self):
  self.p.stdin.write('.exit\n');self.p.stdin.flush()
  error=self.p.communicate(timeout=10)[1]
  assert self.p.returncode==0,error

rng=random.Random(7112)
keys=list(range(-1200,1200))+[2147483657,9223372036854775807,-9223372036854775808]
rng.shuffle(keys)
with tempfile.TemporaryDirectory(prefix='mvdb_pk_bounds_') as root:
 original=os.path.join(root,'initial');os.mkdir(original)
 c=C(original,True)
 assert c.sql('CREATE TABLE t (id BIGINT PRIMARY KEY, g BIGINT, s VARCHAR(8))')[0]=='ok'
 for i in range(0,len(keys),80):
  vals=','.join(f"({pk},{pk%13},'{'甲' if pk%2 else '乙'}')" for pk in keys[i:i+80])
  assert c.sql('INSERT INTO t VALUES '+vals)[0]=='ok'
 c.stop()
 recordings=[]
 for fast in (False,True):
  folder=os.path.join(root,str(fast));shutil.copytree(original,folder)
  c=C(folder,fast)
  commands=[]
  for lo,hi in [(-2,2),(-999,-333),(0,1200),(-2147483648,2147483647),
                (-9223372036854775808,-9223372036854775808),
                (9223372036854775807,9223372036854775807),
                (2147483657,9223372036854775807)]:
   for lim in (0,1,3,12,5000):
    for order in ('ASC','DESC'):
     commands.append(f'SELECT id FROM t WHERE id >= {lo} AND id <= {hi} ORDER BY id {order} LIMIT {lim}')
     commands.append(f"SELECT id, s FROM t WHERE id > {lo} AND id < {hi} AND g >= 2 ORDER BY id {order} LIMIT {lim}")
  commands += [
   'SELECT id FROM t WHERE id < -9223372036854775808 ORDER BY id LIMIT 9',
   'SELECT id FROM t WHERE id > 9223372036854775807 ORDER BY id DESC LIMIT 9',
   'SELECT id FROM t ORDER BY id DESC LIMIT 10',
   'SELECT id FROM t WHERE id >= 1000 AND id < 1100',
   "SELECT id FROM t WHERE s = '甲' AND id >= -100 ORDER BY id DESC LIMIT 20",
   'BEGIN',
   'UPDATE t SET g = 77 WHERE id = 1199',
   'DELETE FROM t WHERE id = -1199',
   'SELECT id FROM t WHERE g = 77 ORDER BY id DESC LIMIT 8',
   'ROLLBACK',
   'SELECT id FROM t WHERE id < -800 ORDER BY id LIMIT 5',
   'UPDATE t SET s = \'丙\' WHERE id = 1000',
   'DELETE FROM t WHERE id = 1100',
   'SELECT id FROM t WHERE s = \'丙\' ORDER BY id LIMIT 5',
   'SELECT id FROM t ORDER BY id DESC LIMIT 15',
  ]
  out=[c.sql(stmt) for stmt in commands]
  c.stop()
  c=C(folder,fast)
  out += [c.sql(s) for s in [
   'SELECT id FROM t WHERE id >= 1000 ORDER BY id LIMIT 30',
   'SELECT id FROM t ORDER BY id DESC LIMIT 50',
   "SELECT id FROM t WHERE s = '丙' ORDER BY id DESC LIMIT 5"]]
  c.stop();recordings.append(out)
 assert recordings[0]==recordings[1],next(((a,b) for a,b in zip(*recordings) if a!=b),None)
 print('PASS: B+Tree inclusive PK ranges, ASC/DESC LIMIT, splits, unicode, min/max signed IDs, writes, txn overlays, restart')
