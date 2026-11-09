#!/usr/bin/env python3
"""Generation-based WAL checkpoint/recycling and interruption safety."""
import os, subprocess, sys, tempfile, pathlib
BIN=sys.argv[1]

def start(home,failpoint=None):
    env=os.environ.copy();env['MVDB_CHECKPOINT_BYTES']='1024'
    env['MVDB_ANN_FORCE_INDEX']='1';env['MVDB_ANN_PROBES']='64'
    env.pop('MVDB_TEST_FAILPOINT',None)
    if failpoint:env['MVDB_TEST_FAILPOINT']=failpoint
    return subprocess.Popen([BIN,'--data-dir',str(home),'--search-mode','ann'],
        stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,
        encoding='utf-8',text=True,bufsize=1,env=env)

def sql(p,s,allow_crash=False):
    assert p.poll() is None
    try:p.stdin.write(s+'\n');p.stdin.flush();h=p.stdout.readline().strip()
    except (BrokenPipeError,ConnectionResetError):
        if allow_crash:return 'KILLED'
        raise
    if not h and allow_crash:return 'KILLED'
    if h.startswith('ROWS\t'):
        n=int(h.split('\t')[1]);vals=[]
        for _ in range(n):vals.append(p.stdout.readline().strip().split('\t')[1:])
        assert p.stdout.readline().strip()=='END'
        return vals
    return h

def close(p):
    p.stdin.write('.exit\n');p.stdin.flush()
    p.communicate(timeout=20);assert p.returncode==0

def interrupted(home,failpoint,pk):
    p=start(home,failpoint)
    batch=','.join(f'({i},3,[0,0,0,0])' for i in range(pk,pk+40))
    sql(p,'INSERT INTO t VALUES '+batch,True)
    p.communicate(timeout=20)
    assert p.returncode != 0,(failpoint,p.returncode)
    p=start(home)
    assert sql(p,f'SELECT id FROM t WHERE id={pk}')==[[str(pk)]],failpoint
    close(p)

with tempfile.TemporaryDirectory(prefix='mvdb_wal_rotation_') as tmp:
    home=pathlib.Path(tmp)
    p=start(home)
    assert sql(p,'CREATE TABLE t (id BIGINT PRIMARY KEY, grp BIGINT, emb VECTOR(4))')=='OK'
    assert sql(p,'CREATE VECTOR INDEX idx ON t(emb)')=='OK'
    for i in range(15):
        assert sql(p,f'INSERT INTO t VALUES ({i},3,[{i},0,0,0])')=='OK'
    assert sql(p,'SELECT id FROM t WHERE id=14')==[['14']]
    close(p)
    files=list(home.glob('wal.*.mvdb'))
    assert files and max(f.stat().st_size for f in files)<1024, [(f.name,f.stat().st_size) for f in files]
    assert not (home/'wal.mvdb').exists(), 'old log not reaped'
    p=start(home)
    assert sql(p,'SELECT id FROM t ORDER BY id LIMIT 16')==[[str(i)] for i in range(15)]
    close(p)
    interrupted(home,'before_wal_checkpoint_manifest',101)
    interrupted(home,'after_wal_checkpoint_manifest',201)
    p=start(home)
    assert sql(p,'SELECT id FROM t WHERE id=101')==[['101']]
    assert sql(p,'SELECT id FROM t WHERE id=201')==[['201']]
    assert sql(p,'SELECT id FROM t ORDER BY L2_DISTANCE(emb,[0,0,0,0]) LIMIT 2')==[['0'],['101']]
    close(p)
    print('PASS: WAL rotation, old-gen reclamation, ANN meta transition, both crash boundaries')
