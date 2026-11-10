"""Production JSONL stress: a >64MiB database under a hard 64MiB container limit."""
import json
import os
from pathlib import Path
import subprocess
import sys
import uuid

docker, image, output = sys.argv[1:]
rows = 200000
name = 'mvdb-external-' + uuid.uuid4().hex[:12]
volume = name + '-data'
subprocess.run([docker, 'volume', 'create', volume], check=True, capture_output=True)
command = [docker, 'run', '--name', name, '-i', '--read-only', '--network', 'none',
           '--cpus', '2', '--memory', '64m', '--memory-swap', '64m', '--pids-limit', '128',
           '--mount', f'type=volume,src={volume},dst=/data', image]
p = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
active_name = name
sequence = 0


def read():
    raw = p.stdout.readline()
    if not raw:
        raise RuntimeError(f'container ended: {p.poll()} {p.stderr.read().decode()[:2000]}')
    return json.loads(raw)


def sql(text, op='execute'):
    global sequence
    sequence += 1
    request = {'id': sequence, 'op': op}
    if op == 'execute':
        request.update(sql=text, search_mode='exact')
    p.stdin.write((json.dumps(request)+'\n').encode())
    p.stdin.flush()
    reply = read()
    assert reply['id'] == sequence and reply['ok'] is True, reply
    return reply


try:
    assert read() == {'type': 'ready'}
    sql('CREATE TABLE t (id BIGINT PRIMARY KEY, g BIGINT, v VECTOR(128));')
    vector = '['+','.join(str(i % 13) for i in range(128))+']'
    for start in range(0, rows, 100):
        values = ','.join(f'({i},{i%7},\'{vector}\')' for i in range(start, start+100))
        assert sql('INSERT INTO t VALUES '+values+';')['affected_rows'] == 100
        if (start+100) % 50000 == 0:
            print(f'inserted {start+100}/{rows}', flush=True)
    assert sql(f'SELECT id FROM t WHERE id>={rows-3} ORDER BY id;')['rows'] == [[rows-3], [rows-2], [rows-1]]
    assert sql(f"SELECT id FROM t ORDER BY L2_DISTANCE(v,'{vector}') LIMIT 10;")['rows'] == [[i] for i in range(10)]
    assert sql(f"SELECT id FROM t WHERE g=3 ORDER BY L2_DISTANCE(v,'{vector}') LIMIT 10;")['rows'] == [[3+7*i] for i in range(10)]
    snapshot = subprocess.check_output([docker, 'exec', name, '/bin/sh', '-c',
        'wc -c </data/heap.mvdb; cat /sys/fs/cgroup/memory.max; cat /sys/fs/cgroup/memory.peak; cat /sys/fs/cgroup/memory.events']).decode()
    lines = snapshot.splitlines()
    heap_bytes, memory_max, memory_peak = map(int, lines[:3])
    assert heap_bytes > memory_max == 64*1024*1024
    sql('', op='shutdown')
    assert p.communicate(timeout=20)[1] == b'' and p.returncode == 0
    restart_command = list(command)
    active_name = name + '-restart'
    restart_command[restart_command.index('--name')+1] = active_name
    p = subprocess.Popen(restart_command,
                         stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    assert read() == {'type': 'ready'}
    for ident in (0, rows//2, rows-1):
        assert sql(f'SELECT id,g,v FROM t WHERE id={ident};')['rows'] == [[ident, ident%7, [i%13 for i in range(128)]]]
    sql('', op='shutdown')
    assert p.communicate(timeout=20)[1] == b'' and p.returncode == 0
    result = {'passed': True, 'rows': rows, 'dimension': 128, 'heap_bytes': heap_bytes,
              'memory_max_bytes': memory_max, 'cgroup_memory_peak_bytes': memory_peak,
              'cgroup_snapshot': snapshot, 'restart_checks': True, 'volume': volume,
              'note': 'Local reduced-memory stress; not all hidden acceptance cases.'}
    Path(output).write_text(json.dumps(result, indent=2), encoding='utf-8')
    print(json.dumps(result, indent=2), flush=True)
finally:
    if p.poll() is None:
        subprocess.run([docker, 'kill', active_name], capture_output=True, timeout=15)
        p.kill()
