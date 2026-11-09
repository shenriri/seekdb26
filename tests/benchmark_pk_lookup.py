#!/usr/bin/env python3
"""Compare a full-table PK equality filter against direct B+Tree point lookup.

Uses a shared read-only phase-3-created database, alternating process variants;
results are local wall-time observations, NOT official contest measurements.
"""
import json, os, pathlib, random, statistics, subprocess, sys, tempfile, time

baseline,current=sys.argv[1:3]
rng=random.Random(20261109)
N=10000
keys=[rng.randrange(-500,N+500) for _ in range(500)]
questions=[f'SELECT g FROM t WHERE id={pk}'+(f' AND g={pk%13}' if j%5==0 else '')
           for j,pk in enumerate(keys)]

def run(binary, home, setup=False):
    p=subprocess.Popen([binary,'--data-dir',str(home)],text=True,encoding='utf-8',
        stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,bufsize=1)
    def sql(text):
        p.stdin.write(text+'\n');p.stdin.flush()
        line=p.stdout.readline().rstrip('\n')
        if line=='OK':return 'OK'
        if not line.startswith('ROWS\t'):raise RuntimeError((text,line))
        rows=[]
        for _ in range(int(line[5:])):rows.append(p.stdout.readline().rstrip('\n'))
        assert p.stdout.readline().strip()=='END'
        return rows
    if setup:
        sql('CREATE TABLE t (id BIGINT PRIMARY KEY, g BIGINT, v VECTOR(4))')
        for start in range(0,N,100):
            vals=','.join(f'({i},{i%13},[{i%17},0,0,0])' for i in range(start,start+100))
            assert sql('INSERT INTO t VALUES '+vals)=='OK'
    outputs=[]
    for q in questions[:20]:sql(q)
    t0=time.perf_counter()
    for q in questions:outputs.append(sql(q))
    elapsed=time.perf_counter()-t0
    p.stdin.write('.exit\n');p.stdin.flush()
    _,err=p.communicate(timeout=40)
    assert p.returncode==0,err
    return elapsed,outputs

with tempfile.TemporaryDirectory(prefix='mvdb_pkbench_') as d:
    home=pathlib.Path(d)
    run(baseline,home,setup=True)
    samples={'phase3':[],'phase4':[]}
    for label,binary in [('phase3',baseline),('phase4',current),
                         ('phase4',current),('phase3',baseline),
                         ('phase3',baseline),('phase4',current)]:
        sec,out=run(binary,home)
        if 'canonical' not in samples:samples['canonical']=out
        assert out==samples['canonical'],f'wrong PK results for {label}'
        samples[label].append(round(sec,6))
    del samples['canonical']
    result={'kind':'local_nonofficial_point_lookup_benchmark','records':N,
      'point_queries':len(questions),'samples_seconds':samples,
      'median_seconds':{x:round(statistics.median(v),6) for x,v in samples.items()}}
    result['speedup_phase4_vs_phase3']=round(result['median_seconds']['phase3']/result['median_seconds']['phase4'],2)
    print(json.dumps(result,indent=2))
    if len(sys.argv)>3:pathlib.Path(sys.argv[3]).write_text(json.dumps(result,indent=2)+'\n')
