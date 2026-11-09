#!/usr/bin/env python3
"""COW B+Tree fanout/split, row overflow, cross-table and restart regression."""
import os
import pathlib
import random
import subprocess
import sys
import tempfile

exe=sys.argv[1]
class Client:
    def __init__(self,dir):
        self.proc=subprocess.Popen([exe,'--data-dir',dir],stdin=subprocess.PIPE,stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,encoding='utf-8',text=True,bufsize=1)
    def sql(self,statement):
        self.proc.stdin.write(statement+'\n');self.proc.stdin.flush()
        line=self.proc.stdout.readline().rstrip('\n')
        if line=='OK':return ('OK',[])
        if line.startswith('ERR\t'):return ('ERR',line[4:])
        assert line.startswith('ROWS\t'),(statement,line)
        count=int(line[5:]);rows=[]
        for _ in range(count):
            fields=self.proc.stdout.readline().rstrip('\n').split('\t')
            assert fields[0]=='ROW',fields
            rows.append(fields[1:])
        assert self.proc.stdout.readline().strip()=='END'
        return ('ROWS',rows)
    def exit(self):
        self.proc.stdin.write('.exit\n');self.proc.stdin.flush()
        err=self.proc.communicate(timeout=20)[1]
        assert self.proc.returncode==0,err

with tempfile.TemporaryDirectory(prefix='mvdb_disk_') as directory:
    c=Client(directory)
    assert c.sql('CREATE TABLE t (id BIGINT PRIMARY KEY, g BIGINT, vec VECTOR(4))')[0]=='OK'
    assert c.sql('CREATE TABLE overflow (id BIGINT PRIMARY KEY, text VARCHAR(12000))')[0]=='OK'
    # Force multiple B+Tree leaf splits and replacements with deliberately shuffled primary keys.
    ids=list(range(3500));random.Random(7879).shuffle(ids)
    for start in range(0,len(ids),100):
        ins=','.join(f'({k},{k%7},[{k%5},{k%3},0,0])' for k in ids[start:start+100])
        assert c.sql('INSERT INTO t VALUES '+ins)[0]=='OK'
    assert c.sql('SELECT id FROM t ORDER BY id LIMIT 5')[1]==[[str(i)] for i in range(5)]
    for key in [0,1,239,320,321,639,999,1500,3199,3499]:
        assert c.sql(f'SELECT g FROM t WHERE id={key}')[1]==[[str(key%7)]],key
    assert c.sql('UPDATE t SET g=888 WHERE id>=3400')[0]=='OK'
    assert c.sql('DELETE FROM t WHERE id<100')[0]=='OK'
    assert c.sql('BEGIN')[0]=='OK'
    assert c.sql('INSERT INTO t VALUES (4000,8,[0,0,0,0])')[0]=='OK'
    assert c.sql('DELETE FROM t WHERE id=320')[0]=='OK'
    assert c.sql('ROLLBACK')[0]=='OK'
    assert c.sql('SELECT id FROM t WHERE id=320')[1]==[['320']]
    assert c.sql('SELECT id FROM t WHERE id=4000')[1]==[]
    assert c.sql('SELECT id FROM t WHERE g=888 ORDER BY id LIMIT 3')[1]==[['3400'],['3401'],['3402']]
    long_str='文'*9000
    assert c.sql(f"INSERT INTO overflow VALUES (1,'{long_str}')")[0]=='OK'
    assert c.sql('SELECT text FROM overflow WHERE id=1')[1]==[[long_str]]
    c.exit()
    page_file=pathlib.Path(directory)/'index.mvdb'
    assert page_file.stat().st_size%8192==0
    # The batch-coalesced COW index must not allocate a full 8K page for EVERY inserted row.
    assert page_file.stat().st_size<3500*8192//4,(page_file.stat().st_size,'COW amplification')
    c=Client(directory)
    assert c.sql('SELECT id FROM t ORDER BY id LIMIT 5')[1]==[[str(i)] for i in range(100,105)]
    assert c.sql('SELECT g FROM t WHERE id=3499')[1]==[['888']]
    assert c.sql('SELECT text FROM overflow WHERE id=1')[1]==[[long_str]]
    assert c.sql('INSERT INTO t VALUES (320,1,[0,0,0,0])')[0]=='ERR'
    assert c.sql('INSERT INTO t VALUES (50,1,[0,0,0,0])')[0]=='OK'
    assert c.sql('SELECT id FROM t WHERE id=50')[1]==[['50']]
    c.exit()
print('PASS: 3500 shuffled keys, B+Tree splits, COW rewrites, overflow row, rollback, restart, tombstone')
