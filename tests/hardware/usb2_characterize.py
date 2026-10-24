"""USB2 hardware sweep. Runs one device session at a time and retains raw evidence.

Example: python usb2_characterize.py --och path/to/och.exe --out results \
    --case rx2:6000000:10 --case duplex2:3000000:300
TX cases use the CLI's default 1 MHz tone (amplitude 0.25) and 60 dB attenuation.
"""
import argparse
import datetime
import hashlib
import json
import math
from pathlib import Path
import subprocess
import time

MODES = {
    'rx1': (['--rx', '--channels', '1'], 1, 0, 3),
    'rx2': (['--rx', '--channels', '1,2'], 2, 0, 3),
    'tx1': (['--tx', '--channels', '1'], 0, 1, 3),
    'tx2': (['--tx', '--channels', '1,2'], 0, 2, 3),
    'duplex1': (['--rx', '--tx', '--channels', '1'], 1, 1, 3),
    'duplex2': (['--rx', '--tx', '--channels', '1,2'], 2, 2, 3),
    'duplex8': (['--rx', '--tx', '--channels', '1,2', '--iq-bits', '8'], 2, 2, 2),
}
FAULTS = ['host_dropped_samples', 'transfer_errors', 'device_overflows',
          'device_underflows', 'event_drops', 'resyncs', 'tx_starvation_events', 'tx_padding_samples']

def run(exe, args, timeout):
    t = time.monotonic()
    p = subprocess.run([str(exe)] + args, capture_output=True, timeout=timeout)
    return {'args': args, 'exit_code': p.returncode,
            'wall_seconds': time.monotonic()-t,
            'stdout': p.stdout.decode('utf-8', errors='replace'),
            'stderr': p.stderr.decode('utf-8', errors='replace')}

def result_json(r):
    return json.loads(r['stdout'].strip())

def assess(payload, run_result, mode, rate, seconds, min_ratio=0.98):
    """Assess saved evidence too, without changing its original counters/verdict."""
    _, nr, nt, width = MODES[mode]
    reasons, ratios = [], {}
    startup = {'window_seconds':1.0, 'policy':'recovered_rx_overflows_within_window',
               'overflows':0, 'recoveries':0, 'resyncs':0, 'accepted':0}
    if run_result['exit_code'] != 0:
        reasons.append('nonzero exit / timeout')
    if not isinstance(payload, dict):
        return ['no result JSON object'] + reasons, ratios, startup
    for line in run_result.get('stderr', '').splitlines():
        try:
            event = json.loads(line)
        except ValueError:
            continue
        observed = event.get('observed_seconds')
        if event.get('type') != 'event' or not isinstance(observed, (float, int)) or not 0 <= observed <= 1:
            continue
        message = event.get('message', '')
        if event.get('event_type') == 1 and 'type=rx_overflow ' in message:
            if 'action=waiting_rx_resync ' in message: startup['overflows'] += 1
            if 'action=rx_restarted ' in message: startup['recoveries'] += 1
        if event.get('event_type') == 2 and event.get('epoch', 0) > 1:
            startup['resyncs'] += 1
    rx = payload.get('rx', {})
    startup['accepted'] = min(startup['overflows'], startup['recoveries'], startup['resyncs'],
                              rx.get('device_overflows', 0), rx.get('resyncs', 0))
    if payload.get('interrupted'): reasons.append('interrupted')
    if payload.get('stopped_early'): reasons.append('early stop: repeated RX overflows after startup')
    if payload.get('config', {}).get('sample_rate') != rate:
        reasons.append('configured sample rate mismatch')
    for direction, channels in [('rx',nr),('tx',nt)]:
        if not channels: continue
        stats = payload.get(direction, {})
        required = FAULTS + ['usb_MBps', 'elapsed_seconds', 'channels']
        if not all(key in stats for key in required):
            reasons.append(f'{direction} missing statistics')
            continue
        ratios[direction] = stats['usb_MBps'] / (rate*channels*width/1e6)
        if not math.isfinite(ratios[direction]) or ratios[direction] < min_ratio:
            reasons.append(f'{direction} throughput ratio {ratios[direction]:.4f}')
        if stats['channels'] != channels or stats['elapsed_seconds'] < seconds:
            reasons.append(f'{direction} channels / duration mismatch')
        for key in FAULTS:
            remaining = stats[key]
            if key == 'device_overflows' or (direction == 'rx' and key == 'resyncs'):
                remaining -= min(remaining, startup['accepted'])
            if remaining: reasons.append(f'{direction} {key}={remaining} after startup allowance')
    return reasons, ratios, startup

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--och', required=True, type=Path)
    ap.add_argument('--out', required=True, type=Path)
    ap.add_argument('--case', action='append', required=True, help='mode:rate_Hz:seconds')
    ap.add_argument('--min-ratio', type=float, default=0.98)
    opt = ap.parse_args()
    if not 0 < opt.min_ratio <= 1:
        ap.error('--min-ratio must be in (0, 1]')
    exe = opt.och.resolve()
    opt.out.mkdir(parents=True, exist_ok=True)
    records = []
    for spec in opt.case:
        mode, rate, seconds = spec.split(':')
        rate, seconds = int(rate), int(seconds)
        assert mode in MODES and rate > 0 and seconds > 0
        label = f'{mode}-{rate}-{seconds}s'
        target = opt.out / (label + '.json')
        if target.exists():
            raise SystemExit(f'Refusing to overwrite evidence: {target}')
        info = run(exe, ['info', '--json'], 30)
        assert info['exit_code'] == 0 and result_json(info)['usb_speed_mbps'] == 480, info
        args, nr, nt, width = MODES[mode]
        command = ['bench'] + args + ['--sample-rate',str(rate), '--seconds',str(seconds),
                                      '--attenuation','60','--fail-fast','--json']
        print(f'START {label}', flush=True)
        try:
            r = run(exe, command, seconds + 90)
        except subprocess.TimeoutExpired as e:
            r = {'args':command,'exit_code':None,'timeout':True,
                 'stdout':(e.stdout or b'').decode(errors='replace'),
                 'stderr':(e.stderr or b'').decode(errors='replace')}
        payload = None
        try:
            payload = result_json(r)
        except (ValueError, KeyError):
            pass
        reasons, ratios, startup = assess(payload, r, mode, rate, seconds, opt.min_ratio)
        record = {'time':datetime.datetime.now(datetime.timezone.utc).isoformat(),
                  'executable':str(exe),'exe_sha256':hashlib.sha256(exe.read_bytes()).hexdigest(),
                  'dll_sha256':hashlib.sha256(exe.with_name('omnicore.dll').read_bytes()).hexdigest(),
                  'mode':mode,'rate':rate,'seconds':seconds,'info':info,'run':r,
                  'minimum_throughput_ratio':opt.min_ratio,
                  'startup':startup,
                  'result':payload,'ratios':ratios,'pass':not reasons,'reasons':reasons}
        target.write_text(json.dumps(record,indent=2,ensure_ascii=False)+'\n',encoding='utf-8')
        records.append({k:record[k] for k in ('mode','rate','seconds','ratios','pass','reasons')})
        print(('PASS ' if not reasons else 'FAIL ') + label + ' ' + json.dumps(ratios) +
              (' '+ '; '.join(reasons) if reasons else ''),flush=True)
        (opt.out/'summary.json').write_text(json.dumps(records,indent=2)+'\n',encoding='utf-8')
    return 0

if __name__ == '__main__':
    raise SystemExit(main())
