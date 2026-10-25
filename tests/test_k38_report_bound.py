#!/usr/bin/env python3
"""K38/W161 bound: a REPORT stops re-sending a record that lost twice unchanged.

Executes the production streak helpers of src/client/write.c
(report_streak_note, report_rec_stuck) over the rounds of one REPORT, then
checks that the STALE branch of report_dirty_ino_run drives them: it notes
every STALE round's records before the replay, and a rebuilt record the
bound flags is not sent, gets the inode's writeback error and releases its
identity (putid and slot), so the rest of its K32 unit can commit.
"""
from pathlib import Path
import os, re, shlex, subprocess, tempfile

root = Path(__file__).resolve().parents[1]
src = (root/'src/client/write.c').read_text()


def fn(name):
    m = re.search(r'^static [^\n;]*\b'+name+r'\([^;]+?\)\n\{', src, re.M)
    assert m, name
    return src[m.start():src.index('\n}', m.end())+2]


a = src.index('struct report_streak {')
streak = src[a:src.index('\n};', a)+3]
code = r'''
#include "efs/protocol.h"
#include "efs/metadata.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
''' + streak + '\n' + '\n'.join(fn(n) for n in ['report_streak_cmp', 'report_streak_find',
    'report_streak_same', 'report_streak_note', 'report_rec_stuck']) + r'''
#define OLD 0xc289ull
#define NEW 0x244bull
static struct efs_chunk_rec rec(efs_ino_t ino,uint32_t ci,uint64_t gen,uint64_t base,uint32_t len)
{struct efs_chunk_rec r;memset(&r,0,sizeof(r));r.ino=ino;r.chunk_index=ci;r.chunk_generation=gen;
 r.base_gen=base;r.delta_off=len?100:0;r.delta_len=len;return r;}
int main(void)
{
 struct report_streak *st=NULL;uint32_t n=0;
 /* stuck: span on a superseded base; bystander: same unit, committed base;
  * full: a full image on the same stale base; uncond: never stuck. */
 struct efs_chunk_rec sent[4]={rec(7,2033,0xa5,OLD,300),rec(7,2034,0xb1,NEW,0),
                               rec(7,2035,0xc1,OLD,0),rec(7,2036,0xd1,EFS_CHUNK_BASE_UNCOND,0)};
 /* round 1 STALE, rebuild unchanged: one loss is not yet a livelock */
 report_streak_note(&st,&n,sent,4);
 assert(n==4&&!report_rec_stuck(st,n,&sent[0],NEW));
 /* round 2 STALE, rebuild unchanged again: stop sending it */
 report_streak_note(&st,&n,sent,4);
 const struct report_streak *s=report_rec_stuck(st,n,&sent[0],NEW);
 assert(s&&s->stale==2&&s->ino==7&&s->ci==2033);
 assert(report_rec_stuck(st,n,&sent[2],NEW)); /* a full image on a moved base too */
 assert(!report_rec_stuck(st,n,&sent[1],NEW)); /* lost with its unit only */
 assert(!report_rec_stuck(st,n,&sent[3],NEW));
 /* the committed base caught up with the record: it can win, keep it */
 assert(!report_rec_stuck(st,n,&sent[0],OLD));
 /* a replay that changed the record (new object or new base) restarts the count */
 struct efs_chunk_rec moved=rec(7,2033,0xa6,NEW,300);
 assert(!report_rec_stuck(st,n,&moved,0x9999));
 struct efs_chunk_rec next[1]={moved};
 report_streak_note(&st,&n,next,1);
 assert(n==1&&st[0].stale==1&&!report_rec_stuck(st,n,&moved,0x9999));
 report_streak_note(&st,&n,next,1);
 assert(report_rec_stuck(st,n,&moved,0x9999));
 /* a record absent from the last STALE round starts over */
 report_streak_note(&st,&n,sent+1,1);
 assert(!report_rec_stuck(st,n,&moved,0x9999)&&!report_rec_stuck(st,n,&sent[0],NEW));
 report_streak_note(&st,&n,NULL,0);
 assert(!n&&!st);
 free(st);
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='efs-k38-bound-') as d:
    p = Path(d)/'test.c'; p.write_text(code)
    cmd = shlex.split(os.getenv('CC', 'cc'))+['-std=gnu11', '-g', '-O1', '-Wall', '-Wextra', '-Werror',
        '-I'+str(root/'include')] + shlex.split(os.environ.get('WB_RUNTIME_CFLAGS', ''))
    subprocess.run(cmd+[str(p), '-o', str(p.with_suffix(''))], check=True)
    subprocess.run([str(p.with_suffix(''))], check=True)

# The REPORT loop drives the helpers: notes each STALE round before its replay,
# and drops a flagged record from the rebuild with a writeback error.
loop = fn('report_dirty_ino_run')
stale = loop[loop.index('if (rc == EFS_ERR_STALE) {\n            uint32_t k;'):]
note = stale.index('report_streak_note(&streaks, &nstreaks, crecs, cn);')
assert note < stale.index('stale_repull_replay(&ds, only_ino, crecs, cseq, cn)'), 'streaks must be noted before the replay'
rebuild = stale[stale.index('report_rec_stuck(streaks, nstreaks, &crecs[cn], ce.generation)'):]
rebuild = rebuild[:rebuild.index('cn++;')]
for need in ('report-stuck ino=', 'wb_error_record_add(crecs[cn].ino)', 'dcache_identity_abandon(',
             'putid_drop_snapshot(', 'ds.chunk_inos[i] = 0;', 'continue;'):
    assert need in rebuild, need
assert 'free(streaks);' in loop
print('K38 REPORT bound: two unchanged STALE rounds on a moved base stop the record; '
      'unit bystanders, UNCOND, caught-up base and changed records stay; loop wiring PASS')
