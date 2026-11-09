#!/usr/bin/env python3
"""Differential cache/transaction test. Uses only the provisional dev wire protocol.

Tests numeric postings, low-cardinality and high-cardinality predicates,
cache rebuild after commit, transaction overlays, table switching and restart.
"""
import os
import pathlib
import subprocess
import sys
import tempfile

BIN = sys.argv[1] if len(sys.argv) > 1 else './build/minivectordb'


class Client:
    def __init__(self, dirname, enabled):
        env = os.environ.copy()
        env['MVDB_VECTOR_CACHE'] = str(int(enabled))
        env['MVDB_VECTOR_FILTER_INDEX'] = '1'
        self.p = subprocess.Popen([BIN, '--data-dir', str(dirname), '--search-mode', 'ann'],
                                  stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.PIPE, text=True, env=env, bufsize=1)

    def sql(self, s):
        assert '\n' not in s
        self.p.stdin.write(s+'\n')
        self.p.stdin.flush()
        first = self.p.stdout.readline().rstrip('\n')
        assert first, ('no response', s, self.p.poll())
        if first == 'OK':
            return ('OK', None)
        if first.startswith('ERR\t'):
            return ('ERR', first[4:])
        assert first.startswith('ROWS\t'), first
        n = int(first[5:])
        rows = []
        for _ in range(n):
            line = self.p.stdout.readline().rstrip('\n')
            assert line.startswith('ROW'), line
            rows.append(line.split('\t')[1:])
        assert self.p.stdout.readline().strip() == 'END'
        return ('ROWS', rows)

    def close(self):
        self.p.stdin.write('.exit\n')
        self.p.stdin.flush()
        _, err = self.p.communicate(timeout=10)
        assert self.p.returncode == 0, err


def workload(enabled):
    with tempfile.TemporaryDirectory(prefix='mvdb_cache_diff_') as directory:
        c = Client(directory, enabled)
        record = []
        def run(q):
            ret = c.sql(q)
            record.append((q, ret))
            return ret
        run('CREATE TABLE t (id BIGINT PRIMARY KEY, g BIGINT, tag VARCHAR(16), emb VECTOR(3))')
        for begin in range(0,240,40):
            vals = [f"({i},{i%10},'{('even' if i%2==0 else 'odd')}',[{i%5},{(i//5)%3},0])"
                    for i in range(begin, begin+40)]
            assert run('INSERT INTO t VALUES '+','.join(vals))[0] == 'OK'
        # Repeat queries to hit the scan cache and lazy numeric group postings.
        queries = [
            'SELECT id FROM t ORDER BY L2_DISTANCE(emb,[0,0,0]) LIMIT 10',
            'SELECT id FROM t WHERE g=3 ORDER BY L2_DISTANCE(emb,[0,0,0]) LIMIT 10',
            "SELECT id FROM t WHERE tag='odd' AND g=3 ORDER BY L2_DISTANCE(emb,[1,0,0]) LIMIT 10",
            'SELECT id FROM t WHERE g=999 ORDER BY L2_DISTANCE(emb,[1,0,0]) LIMIT 10',
            'SELECT id FROM t WHERE id=13 ORDER BY L2_DISTANCE(emb,[1,0,0]) LIMIT 10',
            'SELECT id FROM t ORDER BY L2_DISTANCE(emb,[1,0,0]) DESC LIMIT 6',
        ]
        for _ in range(4):
            for q in queries:
                assert run(q)[0] == 'ROWS'
        assert run('UPDATE t SET g=77 WHERE id=3')[0] == 'OK'
        assert run('UPDATE t SET emb=[0,0,0] WHERE id=13')[0] == 'OK'
        assert run('DELETE FROM t WHERE id=8')[0] == 'OK'
        assert run("INSERT INTO t VALUES (999,77,'odd',[0,0,0])")[0] == 'OK'
        for q in queries: run(q)
        assert run('SELECT id FROM t WHERE g=77 ORDER BY L2_DISTANCE(emb,[0,0,0]) LIMIT 10')[1] == [['999'], ['3']]
        assert run('BEGIN')[0] == 'OK'
        assert run('UPDATE t SET g=88 WHERE id=999')[0] == 'OK'
        assert run('SELECT id FROM t WHERE g=88 ORDER BY L2_DISTANCE(emb,[0,0,0]) LIMIT 10')[1] == [['999']]
        assert run('ROLLBACK')[0] == 'OK'
        assert run('SELECT id FROM t WHERE g=77 ORDER BY L2_DISTANCE(emb,[0,0,0]) LIMIT 10')[1] == [['999'], ['3']]
        assert run('CREATE VECTOR INDEX ti ON t(emb)')[0] == 'OK'
        for q in queries: run(q)
        run('CREATE TABLE second (id BIGINT PRIMARY KEY, grp BIGINT, emb VECTOR(3))')
        for begin in range(0,180,45):
            vals = [f'({i},{i},[{i%4},1,0])' for i in range(begin,begin+45)]
            assert run('INSERT INTO second VALUES '+','.join(vals))[0] == 'OK'
        # 180 unique groups triggers low-cardinality map's safe fallback.
        for _ in range(3):
            run('SELECT id FROM second WHERE grp=152 ORDER BY L2_DISTANCE(emb,[0,1,0]) LIMIT 10')
            run('SELECT id FROM t WHERE g=3 ORDER BY L2_DISTANCE(emb,[0,0,0]) LIMIT 10')
        c.close()
        c = Client(directory, enabled)
        for q in queries: record.append(('reopen:'+q,c.sql(q)))
        record.append(('reopen:changed',c.sql('SELECT id FROM t WHERE g=77 ORDER BY L2_DISTANCE(emb,[0,0,0]) LIMIT 10')))
        c.close()
        return record


on = workload(True)
off = workload(False)
assert on == off, next(((x,y) for x,y in zip(on,off) if x!=y), 'length mismatch')
print('PASS: vector scan cache and filtered postings match no-cache exact output across writes, rollback, ANN fallback, two tables and restart')
