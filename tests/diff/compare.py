#!/usr/bin/env python3
"""FAAD2 differential gate; stdlib only. PCM CRCs are diagnostic, never gates."""
import array
import collections
import json
import math
import pathlib
import subprocess
import sys
import tempfile

# Registered policy before running any comparisons. Conditions below deliberately
# match fields and stream properties, never an arbitrary fixture-name allowlist.
POLICY = (
    'LC <= 24 kHz: real core rate (including sample count and no upsampling SBR flag)',
    'implicit-SBR Init2: core rate',
    '32-bit output: positive full-scale clipping',
    'explicit HE ASC objectTypeIndex: 5 versus 2',
    'PS: metadata only; PCM excluded',
    'PCM: diagnostic CRC, SNR >= 35 dB for LC at matching rates (stereo44_lc: FAAD2, libfaad3 and FFmpeg agree pairwise only to 38 dB)',
)

def compare(shim, oracle, root):
    observed = collections.Counter()
    snrs = []
    runs = 0
    def record(reason, expected):
        observed[(reason, 'expected' if expected else 'UNEXPECTED')] += 1
    with tempfile.TemporaryDirectory() as tmp:
        for path in sorted(root.rglob('*')):
            if path.suffix not in ('.aac', '.dab'):
                continue
            for raw in ((1,) if path.suffix == '.dab' else (0, 1)):
                for fmt in (1, 4):
                    runs += 1
                    results, pcms = [], []
                    label = f'{path.relative_to(root)} raw={raw} fmt={fmt}'
                    for i, exe in enumerate((shim, oracle)):
                        pcm = pathlib.Path(tmp) / f'{i}.pcm'
                        run = subprocess.run([exe, str(path), str(raw), str(fmt), str(pcm)], capture_output=True, text=True)
                        if run.returncode:
                            record(f'{label}: binary {i} exit {run.returncode}: {run.stdout[-500:]} {run.stderr}', False)
                        try:
                            results.append([json.loads(line) for line in run.stdout.splitlines()])
                        except ValueError:
                            record(f'{label}: invalid runner output', False)
                            results.append([])
                        pcms.append(pcm.read_bytes() if pcm.exists() else b'')
                    a, b = results
                    if len(a) != len(b):
                        record(f'{label}: record count {len(a)} vs {len(b)}', False)
                    af = [x for x in a if x['kind'] == 'frame']
                    bf = [x for x in b if x['kind'] == 'frame']
                    ps = any(x['ps'] for x in af + bf)
                    low = bool(af and bf) and all(x['object_type'] == 2 and x['samplerate'] <= 24000 for x in af) and all(y['samplerate'] == 2*x['samplerate'] for x,y in zip(af,bf))
                    for x, y in zip(a,b):
                        if x['kind'] != y['kind']:
                            record(f'{label}: record kind', False); continue
                        for field in x:
                            if x[field] == y.get(field) or field in ('kind','value'):
                                continue
                            expected, reason = False, f'{label}: {x["kind"]}.{field} {x[field]} vs {y.get(field)}'
                            if low and ((x['kind']=='init' and field=='rate' and y[field]==2*x[field]) or (x['kind']=='frame' and ((field in ('samplerate','samples') and y[field]==2*x[field]) or (field=='sbr' and x[field]==0 and y[field]==3)))):
                                expected, reason = True, POLICY[0]
                            elif raw and x['kind']=='init' and field=='rate' and af and af[0]['sbr']==1 and y[field]==2*x[field]:
                                expected, reason = True, POLICY[1]
                            elif x['kind']=='asc' and field=='object' and {x[field],y[field]}=={2,5} and path.suffix=='.dab':
                                # Only an explicit AOT 5 wrapper qualifies.
                                data=path.read_bytes()
                                if data[4]>>3 == 5: expected,reason=True,POLICY[3]
                            record(reason, expected)
                    if ps:
                        record(POLICY[4],True)
                    elif af and bf and len(af)==len(bf) and all(x['samplerate']==y['samplerate'] and x['samples']==y['samples'] for x,y in zip(af,bf)):
                        code='h' if fmt==1 else 'f'
                        va,vb=array.array(code),array.array(code)
                        va.frombytes(pcms[0]);vb.frombytes(pcms[1])
                        if len(va)!=len(vb) or not va:
                            record(f'{label}: PCM length {len(va)} vs {len(vb)}',False);continue
                        if not all(math.isfinite(v) for v in va) or not all(math.isfinite(v) for v in vb):
                            record(f'{label}: nonfinite PCM',False);continue
                        signal=sum(float(v)*v for v in vb)
                        noise=sum((float(x)-y)**2 for x,y in zip(va,vb))
                        snr=10*math.log10(signal/noise) if noise and signal else (math.inf if not noise else -math.inf)
                        snrs.append((label,snr))
                        lc=all(x['object_type']==2 for x in af+bf)
                        if not math.isfinite(snr) and snr!=math.inf or lc and snr<35:
                            record(f'{label}: PCM SNR {snr:.2f} dB',False)
                    print(f'{label}: CRC shim={a[-1].get("value","missing") if a else "missing"} stock={b[-1].get("value","missing") if b else "missing"}')
    print('\n| Divergence observed | Classification | Count |\n|---|---|---:|')
    for (reason,status),count in sorted(observed.items()):
        print(f'| {reason} | {status} | {count} |')
    for label,snr in snrs:
        print(f'SNR {label}: {snr:.2f} dB')
    print(f'{runs} comparisons; {sum(v for (r,s),v in observed.items() if s=="UNEXPECTED")} unexpected differences')
    return int(any(s=='UNEXPECTED' for r,s in observed))

if __name__ == '__main__':
    if len(sys.argv)!=4:
        sys.exit('usage: compare.py shim-binary stock-binary data-directory')
    sys.exit(compare(sys.argv[1],sys.argv[2],pathlib.Path(sys.argv[3])))
