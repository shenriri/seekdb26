#!/usr/bin/env python3
"""SIGKILL-safe generational compaction, B+Tree bulk-load, ANN offset versioning."""
import os
import pathlib
import random
import subprocess
import sys
import tempfile

BIN=sys.argv[1]
class Client:
    def __init__(self,directory,ann=False,compact=False,failpoint=None):
        env=os.environ.copy()
        for k in ('MVDB_COMPACT_ON_START','MVDB_TEST_FAILPOINT','MVDB_ANN_FORCE_INDEX','MVDB_ANN_PROBES'):
            env.pop(k,None)
        if compact:env['MVDB_COMPACT_ON_START']='1'
        if failpoint:env['MVDB_TEST_FAILPOINT']=failpoint
        if ann:
            env['MVDB_ANN_FORCE_INDEX']='1'
            env['MVDB_ANN_PROBES']='64'
        self.proc=subprocess.Popen([BIN,'--data-dir',str(directory),'--search-mode','ann' if ann else 'exact'],
            env=env,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,
            text=True,encoding='utf-8',bufsize=1)
    def sql(self,q):
        self.proc.stdin.write(q+'\n');self.proc.stdin.flush()
        head=self.proc.stdout.readline().rstrip('\n')
        if head=='OK':return 'OK',[]
        if head.startswith('ERR\t'):return 'ERR',head[4:]
        assert head.startswith('ROWS\t'),(q,head,self.proc.poll())
        rows=[]
        for _ in range(int(head[5:])):
            line=self.proc.stdout.readline().rstrip('\n').split('\t')
            assert line[0]=='ROW',line
            rows.append(line[1:])
        assert self.proc.stdout.readline().strip()=='END'
        return 'ROWS',rows
    def ok(self,q):
        result=self.sql(q);assert result[0]=='OK',(q,result)
    def rows(self,q):return self.sql(q)[1]
    def close(self):
        self.proc.stdin.write('.exit\n');self.proc.stdin.flush()
        _,err=self.proc.communicate(timeout=40)
        assert self.proc.returncode==0,err
    def killed(self):
        _,err=self.proc.communicate(timeout=40)
        assert self.proc.returncode<0,(self.proc.returncode,err)

def sigkill_compaction(home,failpoint):
    c=Client(home,compact=True,failpoint=failpoint)
    c.killed()

def probe(c):
    return [c.rows(q) for q in [
        'SELECT id FROM t ORDER BY id LIMIT 20',
        'SELECT id,g FROM t WHERE id=511',
        'SELECT id FROM t WHERE id=200',
        'SELECT id FROM t WHERE id=200 AND g=1234',
        'SELECT id FROM t WHERE g=888 ORDER BY id LIMIT 10',
        'SELECT id FROM t ORDER BY L2_DISTANCE(v,[3,2,1,0]) LIMIT 10',
        'SELECT id FROM t WHERE g=2 ORDER BY L2_DISTANCE(v,[3,2,1,0]) LIMIT 10',
        'SELECT id FROM other ORDER BY id LIMIT 10',
    ]]

with tempfile.TemporaryDirectory(prefix='mvdb_compact_') as data:
    home=pathlib.Path(data)
    c=Client(home)
    c.ok('CREATE TABLE t (id BIGINT PRIMARY KEY,g BIGINT,v VECTOR(4))')
    c.ok('CREATE TABLE other (id BIGINT PRIMARY KEY,g BIGINT)')
    perm=list(range(1800));random.Random(42).shuffle(perm)
    for start in range(0,len(perm),100):
        rows=','.join(f'({x},{x%8},[{x%13},{x%17},{x%19},0])' for x in perm[start:start+100])
        c.ok('INSERT INTO t VALUES '+rows)
    c.ok('INSERT INTO other VALUES (1,1),(2,2),(3,3)')
    c.ok('CREATE VECTOR INDEX ti ON t(v)')
    for i in range(8):
        c.ok('UPDATE t SET g=888 WHERE id >= 1000 AND id < 1400')
        c.ok('UPDATE t SET g=1234 WHERE id = 511')
    c.ok('DELETE FROM t WHERE id < 300')
    c.ok('DELETE FROM other WHERE id=2')
    c.ok('BEGIN')
    c.ok('UPDATE t SET v=[0,0,0,0] WHERE id=500')
    c.ok('ROLLBACK')
    expected=probe(c)
    c.close()
    old_heap=(home/'heap.mvdb').stat().st_size
    old_index=(home/'index.mvdb').stat().st_size
    old_postings=sum(p.stat().st_size for p in (home/'ann_t_ti_v').glob('list_*.post'))

    sigkill_compaction(home,'compact_after_data_sync')
    c=Client(home,ann=True)
    assert probe(c)==expected,'crash before publish changed state'
    c.close()
    assert (home/'heap.mvdb').exists() and (home/'index.mvdb').exists()

    sigkill_compaction(home,'compact_after_manifest')
    c=Client(home,ann=True)
    assert probe(c)==expected,'crash after publish changed state or stale ANN offsets'
    c.ok('INSERT INTO t VALUES (2001,2,[0,0,0,0])')
    c.ok('UPDATE t SET v=[0,0,0,0] WHERE id=511')
    c.close()
    # This maintenance call advances data generation and reclaims the last
    # published generation; querying exact and forced full-probe ANN must agree.
    c=Client(home,ann=True,compact=True)
    first=c.rows('SELECT id FROM t ORDER BY L2_DISTANCE(v,[0,0,0,0]) LIMIT 10')
    assert first[:2]==[['511'],['2001']],first
    assert c.rows('SELECT id FROM t WHERE id=511 AND g=1234')==[['511']]
    c.close()
    c=Client(home)
    assert c.rows('SELECT id FROM t WHERE id=2001')==[['2001']]
    c.close()

    # The two compacted generations each contain only live rows.
    generations=sorted(home.glob('heap.*.mvdb'))
    assert generations, 'new data generation missing'
    current_heap=max(generations,key=lambda x:int(x.name.split('.')[1]))
    current_index=home/current_heap.name.replace('heap.','index.')
    assert current_heap.stat().st_size<old_heap*0.7,(old_heap,current_heap.stat().st_size)
    assert current_index.stat().st_size<old_index*0.7,(old_index,current_index.stat().st_size)
    compact_heap_bytes=current_heap.stat().st_size
    compact_index_bytes=current_index.stat().st_size
    new_postings=sum(p.stat().st_size for p in (home/'ann_t_ti_v').glob('list_*.post'))
    assert new_postings<old_postings*0.8,(old_postings,new_postings)

    # Re-compaction after a crash in cleanup must remain idempotent.
    sigkill_compaction(home,'compact_after_cleanup')
    c=Client(home,ann=True)
    assert c.rows('SELECT id FROM t WHERE id=2001')==[['2001']]
    assert c.rows('SELECT id FROM t ORDER BY L2_DISTANCE(v,[0,0,0,0]) LIMIT 2')==[['511'],['2001']]
    c.close()
    print('PASS: two-table dense bulk load, post-delete reclaim, B+Tree lookup, ANN rebuild, '
          'pre/post manifest SIGKILL, follow-up commits, repeated compaction')
    print('BEFORE_AFTER_BYTES:',old_heap,compact_heap_bytes,old_index,compact_index_bytes,
          old_postings,new_postings)
