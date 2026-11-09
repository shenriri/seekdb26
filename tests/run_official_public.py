#!/usr/bin/env python3
"""Run the official MiniVectorDB public SQL cases against the C++ API adapter.
Source: oceanbase/seekdb mysqltest at a3dba559c1e7ebdc48bc990ffb1a4c75d1c1075c.
The .test files have been verified byte-for-byte via Git blob SHA.
The .result files preserve official assertion semantics (whitespace normalized).
This is NOT the organizer's private run_case.py or full acceptance tests.
"""
import collections
import json
import math
from pathlib import Path
import subprocess
import sys
import tempfile


def check_row(expected, got):
    if len(expected) != len(got):
        return False
    for x, y in zip(expected, got):
        if isinstance(x, float):
            try:
                z = float(y)
            except ValueError:
                return False
            if not math.isclose(x, z, rel_tol=1e-5, abs_tol=1e-5):
                return False
        elif isinstance(x, int):
            if str(x) != y:
                return False
        else:
            if not isinstance(x, str) or x != y:
                return False
    return True


def verify(expected, actual):
    if not actual.get('ok'):
        return f"database error: {actual.get('error')}"
    if 'affected_rows' in expected and expected['affected_rows'] != actual.get('affected_rows'):
        return f"affected_rows={actual.get('affected_rows')}, expected {expected['affected_rows']}"
    if 'columns' in expected and expected['columns'] != actual.get('columns'):
        return f"columns={actual.get('columns')}, expected {expected['columns']}"
    if 'rows' in expected:
        got = actual.get('rows')
        want = expected['rows']
        if not isinstance(got, list) or len(got) != len(want):
            return f"rows count={len(got) if isinstance(got, list) else got}, expected {len(want)}"
        if expected.get('unordered'):
            # Perform a multiset match rather than a set match (duplicates count).
            remaining = list(got)
            for row in want:
                pos = next((i for i, y in enumerate(remaining) if check_row(row,y)), -1)
                if pos < 0:
                    return f"unordered row {row!r} not found in {remaining!r}"
                remaining.pop(pos)
        elif any(not check_row(a,b) for a,b in zip(want,got)):
            return f"row mismatch: actual={got!r}, expected={want!r}"
    return None


def main():
    if len(sys.argv) < 3:
        raise SystemExit('usage: run_official_public.py ADAPTER CASE_DIR [OUTPUT_JSON]')
    binary=Path(sys.argv[1]).resolve()
    root=Path(sys.argv[2])
    manifest=json.loads((root/'manifest.json').read_text(encoding='utf-8'))
    report={'source_commit':'a3dba559c1e7ebdc48bc990ffb1a4c75d1c1075c',
            'source_scope':manifest['scope'], 'version':manifest['version'], 'cases':[]}
    for entry in manifest['cases']:
        name=entry['id'];stem=root/'cases'/name
        sqls=[line.strip() for line in (Path(str(stem)+'.test')).read_text(encoding='utf-8').splitlines() if line.strip()]
        expected=json.loads(Path(str(stem)+'.result').read_text(encoding='utf-8'))
        assert len(sqls)==len(expected)==entry['statements'],name
        with tempfile.TemporaryDirectory(prefix='mvdb_public_'+name+'_') as dbdir:
            try:
                run=subprocess.run([str(binary),dbdir],input='\n'.join(sqls)+'\n',text=True,encoding='utf-8',
                                   capture_output=True,timeout=30,check=False)
                outputs=[json.loads(line) for line in run.stdout.splitlines() if line.strip()]
                if run.returncode!=0:
                    raise RuntimeError(f'process rc={run.returncode} stderr={run.stderr[:400]}')
                if len(outputs)!=len(sqls):
                    raise RuntimeError(f'got {len(outputs)} outputs, expected {len(sqls)}; stderr={run.stderr[:300]}')
                failed=[]
                for i,(sql,want,got) in enumerate(zip(sqls,expected,outputs),1):
                    error=verify(want,got)
                    if error:failed.append({'statement':i,'sql':sql,'error':error})
                report['cases'].append({'name':name,'statements':len(sqls),'passed':len(sqls)-len(failed),'failures':failed})
                print(f'{name}: {len(sqls)-len(failed)}/{len(sqls)} passed')
                for fail in failed:print('  FAIL',fail)
            except Exception as e:
                report['cases'].append({'name':name,'statements':len(sqls),'passed':0,'failures':[{'error':str(e)}]})
                print(name,'FAILED',e)
    report['total_sql']=sum(c['statements'] for c in report['cases'])
    report['passed_sql']=sum(c['passed'] for c in report['cases'])
    report['passed_cases']=sum(not c['failures'] for c in report['cases'])
    report['total_cases']=len(report['cases'])
    print('OFFICIAL PUBLIC BASIC (not hidden acceptance):',report['passed_cases'],'/',report['total_cases'],
          'cases,',report['passed_sql'],'/',report['total_sql'],'SQL')
    if len(sys.argv)>3:Path(sys.argv[3]).write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    return 0 if report['passed_cases']==report['total_cases'] else 1
if __name__=='__main__':sys.exit(main())
