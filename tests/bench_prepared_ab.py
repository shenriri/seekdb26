#!/usr/bin/env python3
import json,os,subprocess,sys,statistics,pathlib
base=pathlib.Path(__file__).resolve().parent
binary=str(pathlib.Path(sys.argv[1]).resolve())
results=[]
for turn in range(3):
 modes=['off','on'] if turn%2==0 else ['on','off']
 for mode in modes:
  env=os.environ.copy();env['MVDB_SIMD_PRUNE']='1';env['MVDB_FMA_PRUNE']='1';env['MVDB_PREPARED_QUERY']='1' if mode=='on' else '0'
  raw=subprocess.check_output([sys.executable,str(base/'benchmark.py'),binary,'--n','10000','--d','128','--q','1000','--verify','100'],env=env,text=True,timeout=90)
  x=json.loads(raw);x['prepared']=mode;x['round']=turn+1;results.append(x)
  print(f'{mode} round={turn+1} write={x["write_seconds"]:.3f} query={x["query_seconds"]:.3f} rss={x["sampled_process_rss_peak_bytes"]} recall={x["sampled_recall_at_10_no_filter"]:.3f}/{x["sampled_recall_at_10_filter"]:.3f}',flush=True)
summary={mode:{m:statistics.median(x[m] for x in results if x['prepared']==mode) for m in ['write_seconds','query_seconds','observed_process_io_bytes_total','sampled_process_rss_peak_bytes']} for mode in ('off','on')}
report={'kind':'local_nonofficial_query_hoist_3round','results':results,'median':summary,'query_speedup':summary['off']['query_seconds']/summary['on']['query_seconds']}
(base/'prepared_ab.json').write_text(json.dumps(report,indent=2,ensure_ascii=False)+'\n')
print(json.dumps({'medians':summary,'query_speedup':report['query_speedup']},indent=2))
