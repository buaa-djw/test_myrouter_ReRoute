#!/usr/bin/env python3
import argparse, math, pathlib

def parse_net_info(path):
    nets=[]; cur={}
    for line in path.read_text().splitlines():
        if line.startswith('NET '):
            if cur: nets.append(cur)
            cur={'net_name':line[4:].strip()}
        elif line.startswith('  ') and ':' in line:
            k,v=line.strip().split(':',1); cur[k.strip()]=v.strip()
    if cur: nets.append(cur)
    return nets

def f(d,k,default=0.0):
    try:return float(d.get(k,default))
    except:return default

def i(d,k,default=0):
    try:return int(float(d.get(k,default)))
    except:return default

def check_dir(d):
    s=(d/'summary.rpt').read_text(); n=parse_net_info(d/'net_info.txt')
    total=int([x.split('=')[1] for x in s.splitlines() if x.startswith('total_nets=')][0])
    assert total==len(n), f'total_nets mismatch {total}!={len(n)}'
    eh=float([x.split('=')[1] for x in s.splitlines() if x.startswith('edcompute_hbt_res=')][0])
    ec=float([x.split('=')[1] for x in s.splitlines() if x.startswith('edcompute_hbt_cap=')][0])
    for r in n:
        if r.get('success')=='1': assert r.get('validation')=='OK'
        if r.get('delay_ready')=='1':
            assert math.isfinite(f(r,'avg_sink_delay')) and math.isfinite(f(r,'max_sink_delay'))
            assert f(r,'max_sink_delay')>=f(r,'avg_sink_delay')
            assert i(r,'expected_sink_count')==i(r,'mapped_sink_count')
        if r.get('type')=='2D': assert i(r,'hbt_count')==0
        if r.get('type')=='3D': assert i(r,'hbt_count')>0
        if i(r,'single_pin_net')==1: assert i(r,'expected_sink_count')==0
        else: assert i(r,'expected_sink_count')>0
        if r.get('type')=='3D' and i(r,'hbt_count')>0 and eh>0 and ec>0:
            assert f(r,'ed_hbt_delay_contrib')>0
    return {x['net_name']:f(x,'ed_hbt_delay_contrib') for x in n if x.get('type')=='3D'}

def main():
    ap=argparse.ArgumentParser(); ap.add_argument('dirs', nargs='+', type=pathlib.Path); a=ap.parse_args()
    maps=[check_dir(d) for d in a.dirs]
    if len(maps)==4:
        names=set(maps[0]).intersection(*[set(m) for m in maps[1:]])
        for nm in names:
            h,d,l,z=[m[nm] for m in maps]
            assert h>=d>=l>=z, f'non-monotonic {nm}: {h},{d},{l},{z}'
    print('PASS')

if __name__=='__main__': main()
