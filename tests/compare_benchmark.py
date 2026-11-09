#!/usr/bin/env python3
"""Three local alternating baselines (Phase1 vs Phase2) with cgroup caveat."""
import json
import pathlib
import statistics
import subprocess
import sys

p1,p2=sys.argv[1:3]
here=pathlib.Path(__file__).resolve().parent
runs=[]
for round_i in range(3):
    # Alternating executions reduce the impact of a monotone warmup trend.
    variants=[('phase1',p1),('phase2',p2)] if round_i%2==0 else [('phase2',p2),('phase1',p1)]
    for label,exe in variants:
        r=subprocess.run([sys.executable,str(here/'benchmark.py'),exe,
            '--n','10000','--d','128','--q','1000','--verify','100'],
            capture_output=True,text=True,check=True,timeout=120)
        obj=json.loads(r.stdout);obj['version']=label;obj['round']=round_i+1
        runs.append(obj)
        print(f"{label} round={round_i+1} write={obj['write_seconds']:.3f}s query={obj['query_seconds']:.3f}s "
              f"io={obj['observed_process_io_bytes_total']/1024**2:.2f}MiB rss={obj['sampled_process_rss_peak_bytes']/1024**2:.2f}MiB",flush=True)
summary={label:{
    metric:round(statistics.median(x[metric] for x in runs if x['version']==label),6)
    for metric in ['write_seconds','query_seconds','observed_process_io_bytes_total','sampled_process_rss_peak_bytes']}
    for label in ['phase1','phase2']}
report={'notes':'Three local alternating runs, isolated temporary data directories; NOT official cgroup v2 io.stat/memory.peak or frozen contest baseline.',
        'results':runs,'medians':summary}
(here/'benchmark_phase2_ab_3rounds.json').write_text(json.dumps(report,indent=2,ensure_ascii=False)+'\n')
print(json.dumps(summary,indent=2,ensure_ascii=False))
