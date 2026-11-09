#!/usr/bin/env python3
import json,os,pathlib,statistics,subprocess,sys
base=pathlib.Path(__file__).resolve().parent
old=str(pathlib.Path(sys.argv[1]).resolve());new=str(pathlib.Path(sys.argv[2]).resolve())
rows=[]
for r in range(3):
 for name,exe in ([('previous',old),('optimized',new)] if r%2==0 else [('optimized',new),('previous',old)]):
  env=os.environ.copy()
  if name=='previous': env['MVDB_AVX512']='0';env['MVDB_FMA_PRUNE']='0';env['MVDB_PREPARED_QUERY']='0'
  a=json.loads(subprocess.check_output([sys.executable,str(base/'benchmark.py'),exe,
      '--n','10000','--d','128','--q','1000','--verify','100'],env=env,text=True,timeout=120))
  a['version']=name;a['round']=r+1;rows.append(a)
  print(name,r+1,'write',a['write_seconds'],'query',a['query_seconds'],'IO',a['observed_process_io_bytes_total'],'RSS',a['sampled_process_rss_peak_bytes'],flush=True)
metrics=['write_seconds','query_seconds','observed_process_io_bytes_total','sampled_process_rss_peak_bytes']
summary={version:{metric:statistics.median(x[metric] for x in rows if x['version']==version) for metric in metrics} for version in ('previous','optimized')}
report={'kind':'local_default_scale_preliminary_not_official','hardware':'current execution container',
 'n':10000,'d':128,'q':1000,'k':10,'both_filter_types_recall_pass':all(x['sampled_recall_at_10_filter']==1.0 and x['sampled_recall_at_10_no_filter']==1.0 for x in rows),
 'runs':rows,'medians':summary,'query_speedup':summary['previous']['query_seconds']/summary['optimized']['query_seconds']}
(base/'final_combined_ab.json').write_text(json.dumps(report,indent=2,ensure_ascii=False)+'\n')
print(json.dumps({'medians':summary,'query_speedup':report['query_speedup']},indent=2))
