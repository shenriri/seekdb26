"""Typed JSONL, per-request modes, shutdown and recovery on the production entry point."""
import json
import queue
import subprocess
import sys
import tempfile
import threading

binary = sys.argv[1]


class Client:
    def __init__(self, data):
        self.p = subprocess.Popen([binary, '--data-dir', data], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.q = queue.Queue()
        def reader():
            for line in iter(self.p.stdout.readline, b''):
                self.q.put(line)
            self.q.put(None)
        threading.Thread(target=reader, daemon=True).start()
        assert self.read() == {'type': 'ready'}

    def read(self):
        raw = self.q.get(timeout=10)
        assert raw is not None and raw.endswith(b'\n'), raw
        return json.loads(raw)

    def request(self, sql=None, ident=1, mode='exact', op='execute'):
        request = {'id': ident, 'op': op}
        if sql is not None:
            request.update(sql=sql, search_mode=mode)
        self.p.stdin.write((json.dumps(request, ensure_ascii=False) + '\n').encode())
        self.p.stdin.flush()
        answer = self.read()
        assert type(answer['id']) is type(ident) and answer['id'] == ident, answer
        assert type(answer['ok']) is bool, answer
        return answer

    def close(self):
        assert self.request(op='shutdown', ident='bye') == {'id': 'bye', 'ok': True}
        self.p.stdin.close()
        assert self.p.wait(timeout=10) == 0
        assert self.q.get(timeout=10) is None
        assert not self.p.stderr.read()


with tempfile.TemporaryDirectory(prefix='mvdb_json_') as data:
    c = Client(data)
    assert c.request('CREATE TABLE t (id BIGINT PRIMARY KEY, s VARCHAR(128), v VECTOR(2));')['ok']
    text = '数字123;\n换行\t"quoted"\\路径🙂'
    sql = "INSERT INTO t VALUES (9007199254740993,'" + text + "','[3,4]');"
    r = c.request(sql, ident=9223372036854775807)
    assert type(r['affected_rows']) is int and r['affected_rows'] == 1
    r = c.request('SELECT id,s,v,L2_DISTANCE(v,\'[0,0]\') AS d FROM t;', ident='query')
    assert r['columns'] == ['id', 's', 'v', 'd']
    assert r['rows'] == [[9007199254740993, text, [3, 4], 5]]
    assert type(r['rows'][0][0]) is int and type(r['rows'][0][1]) is str
    assert c.request('SELECT s FROM t WHERE id=0;')['columns'] == ['s']
    assert c.request('SELECT s FROM t WHERE id=0;')['rows'] == []
    assert c.request('CREATE VECTOR INDEX vi ON t(v);')['ok']
    assert c.request('BEGIN;')['ok']
    assert c.request("UPDATE t SET v='[0,0]' WHERE id=9007199254740993;")['ok']
    for mode in ('ann', 'exact', 'ann'):
        assert c.request("SELECT id,L2_DISTANCE(v,'[0,0]') AS d FROM t ORDER BY d LIMIT 10;",
                         mode=mode)['rows'] == [[9007199254740993, 0]]
    assert c.request('ROLLBACK;')['ok']
    assert c.request("SELECT L2_DISTANCE(v,'[0,0]') AS d FROM t;")['rows'] == [[5]]
    r = c.request("INSERT INTO t VALUES (9007199254740993,'duplicate','[0,0]');")
    assert not r['ok'] and isinstance(r['error']['code'], str) and r['error']['code']
    for vector in ('[NaN,0]', '[Infinity,0]', '[1e100,0]', '[0]'):
        r = c.request("INSERT INTO t VALUES (2,'bad','" + vector + "');")
        assert r['ok'] is False, (vector, r)
    c.p.stdin.write(b'not-json\n')
    c.p.stdin.flush()
    assert c.read()['ok'] is False
    assert c.request('SELECT id FROM t;')['rows'] == [[9007199254740993]]
    assert c.request('BEGIN;')['ok']
    assert c.request('DELETE FROM t;')['ok']
    c.close()
    c = Client(data)
    assert c.request('SELECT id,s,v FROM t;')['rows'] == [[9007199254740993, text, [3, 4]]]
    c.p.kill()
    c.p.wait(timeout=10)
    c = Client(data)
    assert c.request('SELECT id FROM t;')['rows'] == [[9007199254740993]]
    c.close()
print('PASS: typed IDs/results, escaping, modes within transaction, errors, graceful and crash restart')
