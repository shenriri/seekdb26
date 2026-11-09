#!/usr/bin/env python3
"""Measure bounded PK scan on one durable database, alternating 3 rounds.
These are local elapsed times, NOT official competition metrics.
"""
import json,os,pathlib,random,statistics,subprocess,sys,tempfile,time
binary=str(pathlib.Path(sys.argv[1]).resolve())
class Client:
 def __init__(self,dir,fast):
  env=os.environ.copy();env['MVDB_PK_FASTPATH']='1' if fast else '0'
  self.proc=subprocess.Popen([binary,'--data-dir',dir],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,encoding='utf-8',text=True,env=env,bufsize=1)
 def sql(self,s):
  self.proc.stdin.write(s+'\n');self.proc.stdin.flush()
  header=self.proc.stdout.readline().strip()
  if header=='OK':return []
  assert header.startswith('ROWS\t'),(header,s)
  rows=[self.proc.stdout.readline().rstrip('\n') for _ in range(int(header.split('\t')[1]))]
  assert self.proc.stdout.readline().strip()=='END'
  return rows
 def close(self):
  self.proc.stdin.write('.exit\n');self.proc.stdin.flush()
  err=self.proc.communicate(timeout=10)[1]
  assert self.proc.returncode==0,err
with tempfile.TemporaryDirectory(prefix='mvdb_pk_perf_') as d:
 c=Client(d,True);c.sql('CREATE TABLE t (id BIGINT PRIMARY KEY, cat BIGINT, tag VARCHAR(8))')
 for start in range(0,20000,100):
  c.sql('INSERT INTO t VALUES '+','.join(f"({i},{i%10},'red')" for i in range(start,start+100)))
 c.close()
 rng=random.Random(830)
 queries=[]
 for k in range(300):
  x=rng.randint(0,19999)
  if k%4==0:sql='SELECT id FROM t ORDER BY id DESC LIMIT 10'
  elif k%4==1:sql='SELECT id FROM t ORDER BY id ASC LIMIT 10'
  elif k%4==2:sql=f'SELECT id FROM t WHERE id >= {x} ORDER BY id LIMIT 10'
  else:sql=f'SELECT id FROM t WHERE id <= {x} ORDER BY id DESC LIMIT 10'
  queries.append(sql)
 results=[];baseline=None
 for t in range(3):
  for fast in ([False,True] if t%2==0 else [True,False]):
   c=Client(d,fast);start=time.perf_counter()
   answers=[c.sql(q) for q in queries]
   seconds=time.perf_counter()-start;c.close()
   if baseline is None:baseline=answers
   assert baseline==answers,'PK_FASTPATH_RESULTS_DIFFER'
   results.append({'round':t+1,'fast':fast,'seconds':seconds})
 summary={str(b):statistics.median(r['seconds'] for r in results if r['fast']==b) for b in [False,True]}
 report={'kind':'local_pk_ordered_topk_perf_nonofficial','n':20000,'queries':len(queries),'results':results,'medians_seconds':summary,'speedup':summary['False']/summary['True'],'identical_results':True}
 out=pathlib.Path(__file__).parent/'pk_range_ab.json';out.write_text(json.dumps(report,indent=2)+'\n')
 print(json.dumps(report,indent=2))
