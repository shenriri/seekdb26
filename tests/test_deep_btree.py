#!/usr/bin/env python3
"""Large enough to split a B+Tree internal/root page, with checkpoint restart."""
import os
import pathlib
import random
import subprocess
import sys
import tempfile
import time

exe=sys.argv[1]
n=int(os.environ.get('MVDB_DEEP_BTREE_ROWS','110000'))
with tempfile.TemporaryDirectory(prefix='mvdb_btree3_') as data:
    p=subprocess.Popen([exe,'--data-dir',data],stdin=subprocess.PIPE,stdout=subprocess.PIPE,
                       stderr=subprocess.PIPE,text=True,encoding='utf-8',bufsize=1)
    def sql(q):
        assert p.poll() is None,'unexpected exit'
        p.stdin.write(q+'\n');p.stdin.flush()
        out=p.stdout.readline().rstrip('\n')
        if out=='OK':return []
        assert out.startswith('ROWS\t'),(q[:100],out)
        rows=[]
        for i in range(int(out[5:])):rows.append(p.stdout.readline().rstrip('\n').split('\t')[1:])
        assert p.stdout.readline().strip()=='END'
        return rows
    assert sql('CREATE TABLE v (id BIGINT PRIMARY KEY, emb VECTOR(1))')==[]
    start=time.monotonic()
    for base in range(0,n,100):
        batch=','.join(f'({i},[{i%31}])' for i in range(base,min(base+100,n)))
        sql('INSERT INTO v VALUES '+batch)
    for k in [0,319,320,321,512,1023,5000,n//2,n-2,n-1]:
        assert sql(f'SELECT id FROM v WHERE id={k}')==[[str(k)]],k
    assert sql(f'SELECT id FROM v ORDER BY id LIMIT 3')==[['0'],['1'],['2']]
    sql(f'DELETE FROM v WHERE id={n-1}')
    sql(f'UPDATE v SET emb=[100] WHERE id={n//2}')
    p.stdin.write('.exit\n');p.stdin.flush()
    err=p.communicate(timeout=15)[1]
    assert p.returncode==0,err
    p=subprocess.run([exe,'--data-dir',data],
      input=f'SELECT id FROM v WHERE id={n//2}\nSELECT id FROM v WHERE id={n-1}\n.exit\n',
      text=True,encoding='utf-8',capture_output=True,timeout=20)
    assert p.returncode==0,p.stderr
    assert f'ROW\t{n//2}' in p.stdout
    assert 'ROWS\t0\nEND' in p.stdout
    # Rebuild a deep tree by dense bottom-up bulk load; validate root routing
    # across previous internal splits and the deleted last key.
    env=os.environ.copy()
    env['MVDB_COMPACT_ON_START']='1'
    p=subprocess.run([exe,'--data-dir',data],
      input=f'SELECT id FROM v WHERE id=0\nSELECT id FROM v WHERE id=320\nSELECT id FROM v WHERE id={n//2}\nSELECT id FROM v WHERE id={n-1}\n.exit\n',
      text=True,encoding='utf-8',capture_output=True,env=env,timeout=60)
    assert p.returncode==0,p.stderr
    assert p.stdout.count('ROWS\t1')==3,p.stdout
    assert p.stdout.count('ROWS\t0')==1,p.stdout
    new_indexes=list(pathlib.Path(data).glob('index.*.mvdb'))
    assert len(new_indexes)==1,new_indexes
    assert new_indexes[0].stat().st_size>0
    file_size=new_indexes[0].stat().st_size
    print(f'PASS: {n} B+Tree inserts with internal splits, update/delete, restart, index {file_size} bytes, '
          f'write+check {time.monotonic()-start:.2f}s')
