#!/usr/bin/env python3
"""Packer busy rate: the floor until space pressure (unpacked bytes or quota usage), then linear to the idle rate."""
from pathlib import Path
import os,subprocess,tempfile
root=Path(__file__).resolve().parents[1];s=(root/'src/server/packer.c').read_text()
a=s.index('static double pk_pressure(');b=s.index('/* Highest quota usage',a)
assert 'pk_busy_rate(pk.g[g].pressure)' in s, 'busy accrual must use the pressure-controlled rate'
assert 'pk.g[0].backlog + pk.g[1].backlog' in s, 'unpacked bytes must cover both groups'
assert 'EFS_PACK_LAG_TARGET_MS' not in s, 'the lag-controlled ramp is replaced'
code=r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
static struct { double idle_rate, busy_rate; uint64_t unpacked_start; double space_start, space_full; } pk;
'''+s[a:b]+r'''
#define G (1ull<<30)
int main(void){
 pk.idle_rate=5000;pk.busy_rate=50;pk.unpacked_start=64*G;pk.space_start=.5;pk.space_full=.8;
 assert(pk_pressure(0,0)==0);assert(pk_pressure(64*G,.5)==0);            /* at the starts: no pressure */
 assert(pk_pressure(96*G,0)==.5);assert(pk_pressure(128*G,0)==1);assert(pk_pressure(900*G,0)==1);
 double p=pk_pressure(0,.65);assert(p>.49&&p<.51);assert(pk_pressure(0,.95)==1);
 assert(pk_pressure(96*G,.8)==1);                                       /* the larger signal wins */
 assert(pk_busy_rate(0)==50);assert(pk_busy_rate(1)==5000);assert(pk_busy_rate(.5)==2525);
 double prev=-1;for(uint64_t u=0;u<200*G;u+=G){double q=pk_pressure(u,.3);assert(q>=prev&&q>=0&&q<=1);prev=q;}
 pk.busy_rate=6000;assert(pk_busy_rate(0)==6000);                       /* busy above idle: never lowered */
 pk.space_start=.8;pk.space_full=.8;assert(pk_pressure(0,.79)==0&&pk_pressure(0,.8)==1);
 puts("pack budget: floor until space pressure, unpacked-bytes and quota signals, linear ramp, cap and monotonicity PASS");return 0;}
'''
with tempfile.TemporaryDirectory(prefix='efs-pack-budget-') as d:
    c=os.path.join(d,'t.c');x=os.path.join(d,'t');open(c,'w').write(code)
    subprocess.check_call([os.environ.get('CC','cc'),'-std=gnu11','-Wall','-Werror','-o',x,c])
    subprocess.check_call([x])
