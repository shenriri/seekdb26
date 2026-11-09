#!/usr/bin/env python3
"""Data larger than the 8MiB decoded cache, with measured process-RSS peak.
Not a replacement for official isolated cgroup v2 memory.peak/io.stat.
"""
import os
import pathlib
import subprocess
import sys
import tempfile
import time

exe=sys.argv[1]
n=int(os.environ.get('MVDB_MEMORY_TEST_ROWS','50000'))
d=128
peak=0
with tempfile.TemporaryDirectory(prefix='mvdb_mem_') as directory:
    p=subprocess.Popen([exe,'--data-dir',directory],stdin=subprocess.PIPE,stdout=subprocess.PIPE,
                       stderr=subprocess.PIPE,encoding='utf-8',text=True,bufsize=1)
    def sql(query):
        global peak
        assert p.poll() is None,'database exited'
        p.stdin.write(query+'\n');p.stdin.flush()
        result=p.stdout.readline().rstrip('\n')
        if result.startswith('ROWS\t'):
            rows=[]
            for _ in range(int(result.split('\t')[1])):
                rows.append(p.stdout.readline().rstrip('\n').split('\t')[1:])
            assert p.stdout.readline().strip()=='END'
        else:
            assert result=='OK',(query[:60],result)
            rows=None
        try:
            for line in pathlib.Path(f'/proc/{p.pid}/status').read_text().splitlines():
                if line.startswith('VmRSS:'):
                    peak=max(peak,int(line.split()[1])*1024)
                    break
        except FileNotFoundError: pass
        return rows
    sql('CREATE TABLE mem (id BIGINT PRIMARY KEY, g BIGINT, v VECTOR(128))')
    vector='['+','.join(str(i%13) for i in range(d))+']'
    start=time.monotonic()
    for i in range(0,n,100):
        group=[]
        for k in range(i,min(i+100,n)):
            group.append(f'({k},{k%7},{vector})')
        sql('INSERT INTO mem VALUES '+','.join(group))
    write=time.monotonic()-start
    # Each result requires an actual scan that encounters more rows than fit in the decoded cache.
    assert sql(f'SELECT id FROM mem WHERE id>={n-1}')==[[str(n-1)]]
    assert sql(f'SELECT id FROM mem WHERE g=3 ORDER BY L2_DISTANCE(v,{vector}) LIMIT 10')==[[str(i)] for i in range(3,73,7)]
    assert sql('SELECT id FROM mem ORDER BY id LIMIT 3')==[['0'],['1'],['2']]
    p.stdin.write('.exit\n');p.stdin.flush()
    err=p.communicate(timeout=20)[1]
    assert p.returncode==0,err
    heap_size=(pathlib.Path(directory)/'heap.mvdb').stat().st_size
    index_size=(pathlib.Path(directory)/'index.mvdb').stat().st_size
    assert heap_size>24*1024*1024,heap_size
    assert peak<48*1024*1024,(peak,'RSS out of bounds')
    # Re-open without decoding all rows during recovery, and validate the highest primary key.
    p=subprocess.run([exe,'--data-dir',directory],input=f'SELECT id FROM mem WHERE id={n-1}\n.exit\n',
        text=True,encoding='utf-8',capture_output=True,timeout=20)
    assert p.returncode==0 and f'ROW\t{n-1}\n' in p.stdout,(p.returncode,p.stdout,p.stderr)
    print(f'PASS: {n} x {d}, heap={heap_size}, index={index_size}, sampled_RSS={peak}, '
          f'write_seconds={write:.3f}, restart primary-key lookup')
