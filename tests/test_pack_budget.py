#!/usr/bin/env python3
"""Packer busy rate follows the lag: floor up to the target, linear to the idle rate over one span."""
from pathlib import Path
import os,subprocess,tempfile
root=Path(__file__).resolve().parents[1];s=(root/'src/server/packer.c').read_text()
a=s.index('static double pk_busy_rate(uint64_t lag_ns)');b=s.index('\n}\n',a)+3
assert 'pk_busy_rate(pk.g[g].lag_ns)' in s, 'busy accrual must use the lag-controlled rate'
assert 'pk.g[pk_gi(group)].lag_ns =' in s, 'the scan must record the lag'
code=r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
static struct { double idle_rate, busy_rate; uint64_t lag_target_ns, lag_span_ns; } pk;
'''+s[a:b]+r'''
#define S 1000000000ull
int main(void){
 pk.idle_rate=5000;pk.busy_rate=50;pk.lag_target_ns=60*S;pk.lag_span_ns=300*S;
 assert(pk_busy_rate(0)==50);assert(pk_busy_rate(60*S)==50);
 assert(pk_busy_rate(210*S)==2525);           /* half a span: halfway */
 assert(pk_busy_rate(360*S)==5000);assert(pk_busy_rate(3600*S)==5000); /* capped */
 double prev=0;for(uint64_t l=0;l<400*S;l+=S){double r=pk_busy_rate(l);assert(r>=prev&&r>=50&&r<=5000);prev=r;}
 pk.busy_rate=6000;assert(pk_busy_rate(3600*S)==6000); /* busy above idle: never lowered */
 pk.busy_rate=50;pk.lag_target_ns=0;assert(pk_busy_rate(1)>50); /* target 0: ramps at once */
 puts("pack budget: lag-controlled busy rate, floor, linear ramp, cap and monotonicity PASS");return 0;}
'''
with tempfile.TemporaryDirectory(prefix='efs-pack-budget-') as d:
    c=os.path.join(d,'t.c');x=os.path.join(d,'t');open(c,'w').write(code)
    subprocess.check_call([os.environ.get('CC','cc'),'-std=gnu11','-Wall','-Werror','-o',x,c])
    subprocess.check_call([x])
