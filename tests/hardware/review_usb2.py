"""Reassess saved USB2 records under the documented one-second startup policy.

Raw evidence is never modified. Earlier runs may have used stricter criteria.
"""
import argparse
import hashlib
import json
from pathlib import Path
from usb2_characterize import assess

def review(root):
    rows = []
    for path in sorted(root.rglob('*.json')):
        record = json.loads(path.read_text(encoding='utf-8-sig'))
        if not isinstance(record, dict) or not all(k in record for k in ('run', 'result', 'mode', 'rate', 'seconds')):
            continue
        reasons, ratios, startup = assess(record['result'], record['run'], record['mode'], record['rate'], record['seconds'])
        rows.append({'source':path.relative_to(root).as_posix(),
                     'sha256':hashlib.sha256(path.read_bytes()).hexdigest(),
                     'mode':record['mode'], 'rate':record['rate'], 'seconds':record['seconds'],
                     'pass':not reasons, 'reasons':reasons, 'ratios':ratios, 'startup':startup,
                     'original_pass':record['pass']})
    return {'criteria':{'startup_window_seconds':1, 'minimum_throughput_ratio':0.98,
                        'startup_policy':'recovered_rx_overflows_within_window',
                        'other_faults_allowed':False}, 'records':rows}

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('directory', type=Path)
    ap.add_argument('--out', required=True, type=Path)
    opt = ap.parse_args()
    report = review(opt.directory)
    opt.out.write_text(json.dumps(report, indent=2, ensure_ascii=False)+'\n', encoding='utf-8')
    for row in report['records']:
        if row['seconds'] >= 60:
            print(f"{'PASS' if row['pass'] else 'FAIL'} {row['mode']:8} {row['rate']/1e6:6g} MSPS {row['seconds']:3}s "
                  f"startup={row['startup']['accepted']} {'; '.join(row['reasons'])}")

if __name__ == '__main__':
    main()
