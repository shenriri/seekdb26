"""Run current public assertions with real graceful restarts and in-place mode changes."""
import json
import math
import re
import subprocess
import sys
import tempfile
from pathlib import Path


def row_matches(want, got):
    if len(want) != len(got):
        return False
    for left, right in zip(want, got):
        if isinstance(left, float):
            try:
                value = float(right)
            except (ValueError, TypeError):
                return False
            if not math.isfinite(value) or not math.isclose(left, value, rel_tol=1e-5, abs_tol=1e-5):
                return False
        elif str(left) != right:
            return False
    return True


def verify(want, got):
    if 'error' in want:
        if got.get('ok') is not False or not got.get('error'):
            return 'Expected database failure with a nonempty error'
        patterns = {'duplicate_key': r'duplicate', 'unsupported_sql': r'unsupported|not supported',
                    'dimension_mismatch': r'dimension|^VECTOR_DIM_MISMATCH$'}
        if not re.search(patterns[want['error']], str(got['error']), flags=re.IGNORECASE):
            return f'Error semantic mismatch: {got["error"]!r}'
        return None
    if got.get('ok') is not True:
        return f'Database error: {got.get("error")}'
    for field in ('affected_rows', 'columns'):
        if field in want and got.get(field) != want[field]:
            return f'{field}: {got.get(field)!r}, expected {want[field]!r}'
    if 'rows' in want:
        rows = got.get('rows')
        if not isinstance(rows, list) or len(rows) != len(want['rows']):
            return 'Wrong row count'
        if want.get('unordered'):
            remaining = list(rows)
            for expected_row in want['rows']:
                pos = next((i for i, row in enumerate(remaining) if row_matches(expected_row, row)), -1)
                if pos < 0:
                    return f'Missing unordered row: {expected_row!r}'
                remaining.pop(pos)
        elif any(not row_matches(x, y) for x, y in zip(want['rows'], rows)):
            return f'Rows: {rows!r}, expected {want["rows"]!r}'
    return None


def main():
    binary, case_root, output = sys.argv[1:]
    case_root = Path(case_root)
    manifest = json.loads((case_root / 'manifest.json').read_text())
    report = {'classification': 'current_official_public_SQL_via_developer_API_adapter',
              'source_commit': '9f6df2ba5acf871376e22b64a3d0724e4b575044',
              'version': manifest['version'], 'official_private_protocol_tested': False,
              'error_mapping': 'semantic regex against engine error text; no frozen official error-code map',
              'adapter_overlay': 'adds only Database.set_ann_mode; core algorithms unchanged', 'cases': []}
    for entry in manifest['cases']:
        actions = [line.strip() for line in (case_root / entry['test']).read_text().splitlines() if line.strip()]
        expectations = json.loads((case_root / entry['result']).read_text())
        assert sum(not line.startswith('--') for line in actions) == len(expectations) == entry['statements']
        case = {'id': entry['id'], 'statements': len(expectations), 'passed': 0, 'failures': [], 'segments': []}
        chunks = [[]]
        for action in actions:
            if action == '--restart':
                chunks.append([])
            elif action.startswith('--') and action not in ('--mode ann', '--mode exact'):
                raise ValueError(f'Unknown driver directive: {action}')
            else:
                chunks[-1].append(action)
        index, mode = 0, 'exact'
        with tempfile.TemporaryDirectory(prefix='mvdb_v4_' + entry['id'] + '_') as data_dir:
            for segment_index, chunk in enumerate(chunks):
                run = subprocess.run([binary, data_dir, mode], input='\n'.join(chunk) + '\n',
                                     text=True, encoding='utf-8', capture_output=True, timeout=45)
                segment = {'number': segment_index, 'actions': chunk, 'returncode': run.returncode,
                           'stdout': run.stdout, 'stderr': run.stderr, 'assertions': []}
                case['segments'].append(segment)
                try:
                    actual = [json.loads(line) for line in run.stdout.splitlines()]
                    if run.returncode != 0 or len(actual) != len(chunk):
                        raise ValueError('Adapter exit or response count mismatch')
                    for action, got in zip(chunk, actual):
                        if action.startswith('--mode '):
                            assert got == {'driver': 'mode'}
                            mode = action.split()[1]
                            continue
                        want = expectations[index]
                        error = verify(want, got)
                        item = {'statement': index + 1, 'sql': action, 'expected': want, 'actual': got,
                                'passed': error is None}
                        segment['assertions'].append(item)
                        if error:
                            case['failures'].append({**item, 'reason': error})
                        else:
                            case['passed'] += 1
                        index += 1
                except Exception as exc:
                    case['failures'].append({'segment': segment_index, 'reason': str(exc)})
                    break
        report['cases'].append(case)
        print(f"{case['id']}: {case['passed']}/{case['statements']}" +
              (f" FAIL {case['failures']}" if case['failures'] else ''), flush=True)
    report['total_cases'] = len(report['cases'])
    report['passed_cases'] = sum(not case['failures'] for case in report['cases'])
    report['total_sql'] = sum(case['statements'] for case in report['cases'])
    report['passed_sql'] = sum(case['passed'] for case in report['cases'])
    Path(output).write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
    print(f"PUBLIC V4: {report['passed_cases']}/{report['total_cases']} cases, "
          f"{report['passed_sql']}/{report['total_sql']} SQL", flush=True)
    return 0 if report['passed_cases'] == report['total_cases'] else 1


if __name__ == '__main__':
    sys.exit(main())
