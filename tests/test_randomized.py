#!/usr/bin/env python3
"""Differential small-model tests for SQL mutation and L2 TopK semantics."""
import math
import pathlib
import random
import subprocess
import sys
import tempfile

exe=sys.argv[1]
seed=1033
rand=random.Random(seed)

class Client:
    def __init__(self,dir):
        self.proc=subprocess.Popen([exe,'--data-dir',dir],stdin=subprocess.PIPE,stdout=subprocess.PIPE,
                                   text=True,encoding='utf-8',bufsize=1)
    def sql(self,statement):
        self.proc.stdin.write(statement+'\n');self.proc.stdin.flush()
        line=self.proc.stdout.readline().rstrip('\n')
        if line=='OK':return ('OK',[])
        if line.startswith('ERR\t'):return ('ERR',line[4:])
        assert line.startswith('ROWS\t'),line
        count=int(line[5:]);out=[]
        for _ in range(count):out.append(self.proc.stdout.readline().rstrip('\n').split('\t')[1:])
        assert self.proc.stdout.readline().strip()=='END'
        return ('ROWS',out)
    def close(self):self.proc.stdin.write('.exit\n');self.proc.stdin.flush();self.proc.wait(timeout=10)

with tempfile.TemporaryDirectory(prefix='mvdb_diff_') as directory:
    c=Client(directory)
    assert c.sql('CREATE TABLE t (id BIGINT PRIMARY KEY, tag VARCHAR(4), v VECTOR(3))')[0]=='OK'
    model={}
    for step in range(220):
        op=rand.choice(['insert','update','delete','topk','transaction'])
        pk=rand.randrange(60)
        if op=='insert':
            v=[rand.randrange(-8,9) for _ in range(3)];tag='a' if pk%2==0 else 'b'
            sql=f"INSERT INTO t VALUES ({pk},'{tag}',[{','.join(map(str,v))}])"
            typ,_=c.sql(sql)
            if pk in model:assert typ=='ERR'
            else:assert typ=='OK';model[pk]=(tag,v)
        elif op=='update':
            v=[rand.randrange(-8,9) for _ in range(3)]
            typ,_=c.sql(f"UPDATE t SET v=[{','.join(map(str,v))}] WHERE id={pk}")
            assert typ=='OK'
            if pk in model:model[pk]=(model[pk][0],v)
        elif op=='delete':
            assert c.sql(f'DELETE FROM t WHERE id={pk}')[0]=='OK'
            model.pop(pk,None)
        elif op=='transaction':
            # Failing second INSERT must not cancel the preceding successful statement.
            pka=rand.randrange(100,160)
            while pka in model:pka=rand.randrange(100,160)
            assert c.sql('BEGIN')[0]=='OK'
            assert c.sql(f"INSERT INTO t VALUES ({pka},'x',[0,0,0])")[0]=='OK'
            assert c.sql(f"INSERT INTO t VALUES ({pka},'y',[1,1,1])")[0]=='ERR'
            assert c.sql(f'SELECT id FROM t WHERE id={pka}')[1]==[[str(pka)]]
            if rand.choice([True,False]):
                assert c.sql('COMMIT')[0]=='OK';model[pka]=('x',[0,0,0])
            else:assert c.sql('ROLLBACK')[0]=='OK'
        else:
            q=[rand.randrange(-3,4) for _ in range(3)]
            where="WHERE tag='a' " if step%2 else ''
            limit=rand.randint(1,15)
            rows=c.sql(f"SELECT id FROM t {where}ORDER BY L2_DISTANCE(v,[{','.join(map(str,q))}]) LIMIT {limit}")[1]
            baseline=[]
            for row_id,(tag,vec) in model.items():
                if where and tag!='a':continue
                dist=math.sqrt(sum((x-y)*(x-y) for x,y in zip(vec,q)))
                baseline.append((dist,row_id))
            baseline.sort()
            assert rows==[[str(i)] for _,i in baseline[:limit]],(step,rows,baseline[:limit])
        if step%25==0:
            actual=c.sql('SELECT id FROM t ORDER BY id')[1]
            assert actual==[[str(pk)] for pk in sorted(model)],(step,actual,model)
    c.close()
    # Exercise one more full replay, not just the original process view.
    c=Client(directory)
    actual=c.sql('SELECT id FROM t ORDER BY id')[1]
    assert actual==[[str(pk)] for pk in sorted(model)]
    c.close()
print('PASS: 220 deterministic differential operations incl. mutations, failed statements, transactions, TopK and restart')
