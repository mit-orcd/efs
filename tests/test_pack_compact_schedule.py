#!/usr/bin/env python3
"""Compaction is scheduled maintenance: idle batches and a daily window, never while busy; it favours tiny holes and cold segments."""
from pathlib import Path
import os,subprocess,tempfile
root=Path(__file__).resolve().parents[1];s=(root/'src/server/packer.c').read_text()
def fn(sig):
    a=s.index(sig);return s[a:s.index('\n}\n',a)+3]
assert 'pk_compact_due(pk.g[pk_gi(group)].busy' in s, 'compaction must run from the schedule'
assert 'pk.g[pk_gi(group)].tokens -= (double)nfiles' not in s, 'compaction must not draw packing tokens'
assert 'pk_now_ns() - batch_start > pk.compact_batch_budget_ns' in s, 'a compaction batch is bounded in time'
assert 'env_u64("EFS_PACK_COMPACT_IDLE_SEGS", 256, 1)' in s, 'idle batches are large enough to clear a churn backlog'
code=r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
static struct { uint64_t compact_idle_ns, compact_cold_ns; uint32_t compact_idle_segs, compact_daily_start, compact_daily_min, segments_per_pass; } pk;
'''+fn('static double pk_compact_priority(')+fn('static uint64_t pk_segment_age_ns(')+fn('static uint32_t pk_compact_due(')+r'''
#define S 1000000000ull
int main(void){
 pk.compact_idle_ns=60*S;pk.compact_idle_segs=8;pk.compact_daily_start=180;pk.compact_daily_min=120;pk.segments_per_pass=8;pk.compact_cold_ns=86400*S;
 uint64_t idle=0,last=0,t=1000*S;
 /* busy: nothing, and the idle clock restarts */
 assert(pk_compact_due(1,t,&idle,&last,600)==0&&idle==0);
 assert(pk_compact_due(0,t,&idle,&last,600)==0&&idle==t);             /* just went idle */
 assert(pk_compact_due(0,t+59*S,&idle,&last,600)==0);
 assert(pk_compact_due(0,t+60*S,&idle,&last,600)==8);                 /* idle 60 s: a batch */
 assert(pk_compact_due(0,t+61*S,&idle,&last,600)==0);                 /* one batch per idle period */
 assert(pk_compact_due(0,t+120*S,&idle,&last,600)==8);                /* still idle: another */
 assert(pk_compact_due(1,t+121*S,&idle,&last,600)==0&&idle==0);
 assert(pk_compact_due(0,t+122*S,&idle,&last,600)==0);                /* idle again: waits 60 s */
 /* daily window 03:00-05:00 UTC: larger batches every pass, but never while busy */
 idle=0;last=0;
 assert(pk_compact_due(0,t,&idle,&last,200)==8&&pk_compact_due(0,t+S,&idle,&last,200)==8);
 assert(pk_compact_due(1,t+2*S,&idle,&last,200)==0);
 assert(pk_compact_due(0,t+3*S,&idle,&last,300)==0);                  /* 05:00: window closed */
 pk.compact_daily_start=1380;                                          /* 23:00 + 2 h wraps midnight */
 idle=0;last=0;assert(pk_compact_due(0,t,&idle,&last,30)==8&&pk_compact_due(0,t,&idle,&last,1400)==8);
 idle=0;last=0;assert(pk_compact_due(0,t,&idle,&last,61)==0);
 /* priority: tiny holes first, then cold first */
 double tiny=pk_compact_priority(120000,128000,0,0),big=pk_compact_priority(20000,128000,0,0);
 assert(tiny>big);
 assert(pk_compact_priority(64000,128000,86400*S,0)>pk_compact_priority(64000,128000,3600*S,0));
 assert(pk_compact_priority(64000,128000,0,1)==pk_compact_priority(64000,128000,999999*S,0)); /* COLD flag = cold */
 /* segment age from a time-ordered id, modulo 2^56 ns */
 uint64_t now=1792500000000000000ull,id=((now-5*S)<<8)|3;
 assert(pk_segment_age_ns(id,now)==5*S);
 puts("pack compaction: idle batches, daily window (with wrap), never busy, tiny holes and cold first PASS");return 0;}
'''
with tempfile.TemporaryDirectory(prefix='efs-pack-compact-') as d:
    c=os.path.join(d,'t.c');x=os.path.join(d,'t');open(c,'w').write(code)
    subprocess.check_call([os.environ.get('CC','cc'),'-std=gnu11','-Wall','-Werror','-o',x,c])
    subprocess.check_call([x])
