#!/usr/bin/env python3
"""ANN index commit/restart/crash differential. Provisional developer protocol."""
import os, subprocess, sys, tempfile, random, pathlib
BIN=sys.argv[1] if len(sys.argv)>1 else './build/minivectordb'

class Client:
    def __init__(self, home, mode='exact', failpoint=None, probes=None):
        env=os.environ.copy()
        env.pop('MVDB_TEST_FAILPOINT',None)
        if failpoint: env['MVDB_TEST_FAILPOINT']=failpoint
        if probes:
            env['MVDB_ANN_PROBES']=str(probes)
            env['MVDB_ANN_FORCE_INDEX']='1'
        self.p=subprocess.Popen([BIN,'--data-dir',str(home),'--search-mode',mode],
            stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,
            encoding='utf-8',text=True,bufsize=1,env=env)
    def sql(self,q):
        assert self.p.poll() is None, f'db exited before {q[:30]}'
        self.p.stdin.write(q+'\n');self.p.stdin.flush()
        head=self.p.stdout.readline().strip()
        if head=='OK':return ('OK',[])
        if head.startswith('ERR\t'):return ('ERR',head[4:])
        assert head.startswith('ROWS\t'), (q,head)
        n=int(head.split('\t')[1]);rows=[]
        for _ in range(n):
            line=self.p.stdout.readline().strip().split('\t')
            assert line[0]=='ROW',(q,line)
            rows.append(line[1:])
        assert self.p.stdout.readline().strip()=='END'
        return ('ROWS',rows)
    def ok(self,q):
        t,r=self.sql(q);assert t=='OK',(q,t,r)
    def rows(self,q):
        t,r=self.sql(q);assert t=='ROWS',(q,t,r)
        return r
    def err(self,q,term):
        t,r=self.sql(q);assert t=='ERR' and term in r,(q,t,r)
    def close(self):
        self.p.stdin.write('.exit\n');self.p.stdin.flush()
        _,err=self.p.communicate(timeout=20)
        assert self.p.returncode==0,err
    def killed(self):
        self.p.communicate(timeout=20)
        assert self.p.returncode !=0

rng=random.Random(42001)
with tempfile.TemporaryDirectory(prefix='mvdb_ann_') as d:
    home=pathlib.Path(d)
    vectors={i:[round(rng.uniform(-3,3),4) for _ in range(8)] for i in range(400)}
    def vec(v):return '['+','.join(map(str,v))+']'
    def q_sql(x,condition=''):
        return f'SELECT id FROM t {condition} ORDER BY L2_DISTANCE(v,{vec(x)}) LIMIT 10'
    c=Client(home)
    c.ok('CREATE TABLE t (id BIGINT PRIMARY KEY, grp BIGINT, v VECTOR(8))')
    for start in range(0,400,40):
        payload=','.join(f'({i},{i%7},{vec(vectors[i])})' for i in range(start,start+40))
        c.ok('INSERT INTO t VALUES '+payload)
    c.err('CREATE VECTOR INDEX nope ON t(grp)','INDEX_REQUIRES_VECTOR_COLUMN')
    c.ok('CREATE VECTOR INDEX idx_t_v ON t(v)')
    c.err('CREATE VECTOR INDEX idx_again ON t(v)','VECTOR_INDEX_ALREADY_EXISTS')
    check_queries=[q_sql(vectors[i],'WHERE grp = 2' if i%2 else '') for i in [0,3,8,10,11,16,22,37,48,72,101,142,181,202,251,299,375]]
    original=[c.rows(q) for q in check_queries]
    c.close()
    assert (home/'ann_t_idx_t_v_v'/'meta.bin').exists()
    c=Client(home,'ann',probes=64)
    assert [c.rows(q) for q in check_queries]==original,'indexed full-probe results differ from exact'
    # Transaction overlay must hide stale committed postings and add uncommitted ones.
    c.ok('BEGIN')
    c.ok('UPDATE t SET v=[0,0,0,0,0,0,0,0] WHERE id=0')
    c.ok('DELETE FROM t WHERE id=1')
    c.ok('INSERT INTO t VALUES (999,2,[0,0,0,0,0,0,0,0])')
    top=c.rows(q_sql([0]*8));assert top[0]==['0'] and top[1]==['999'],top
    c.ok('ROLLBACK')
    assert [c.rows(q) for q in check_queries]==original
    c.ok('BEGIN')
    c.ok('UPDATE t SET v=[0,0,0,0,0,0,0,0] WHERE id=0')
    c.ok('DELETE FROM t WHERE id=1')
    c.ok('INSERT INTO t VALUES (999,2,[0,0,0,0,0,0,0,0])')
    c.ok('COMMIT')
    assert c.rows(q_sql([0]*8))[:2]==[['0'],['999']]
    c.close()
    c=Client(home,'ann',probes=64)
    assert c.rows(q_sql([0]*8))[:2]==[['0'],['999']]
    assert c.rows('SELECT id FROM t WHERE id=1')==[]
    assert c.rows(q_sql([0]*8,'WHERE grp=2'))[0]==['999']
    c.close()
    # Crash after main-store manifest publication; sidecar cursor is old.
    c=Client(home,'ann',failpoint='after_manifest_published',probes=64)
    try:
        c.sql('INSERT INTO t VALUES (1001,2,[0,0,0,0,0,0,0,0])')
    except (BrokenPipeError,ConnectionResetError,AssertionError):pass
    c.killed()
    c=Client(home,'ann',probes=64)
    assert c.rows('SELECT id FROM t WHERE id=1001')==[['1001']]
    assert c.rows(q_sql([0]*8))[:3]==[['0'],['999'],['1001']]
    c.close()
    # A corrupted derived index is rebuilt, not trusted, without altering committed rows.
    meta=home/'ann_t_idx_t_v_v'/'meta.bin'
    meta.write_bytes(b'corrupt')
    c=Client(home,'ann',probes=64)
    assert c.rows(q_sql([0]*8))[:3]==[['0'],['999'],['1001']]
    c.close()
    assert meta.stat().st_size>28
    print('PASS: IVF build, txn overlay, rollback, delta, restart, crash rebuild, sidecar corruption')
