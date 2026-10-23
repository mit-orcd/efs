#!/usr/bin/env python3
"""Missed fresh probes may vote before batch I/O without consuming frames."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'src/server/raft_host.c').read_text()
a = source.index('static void host_note_queued_reads(')
helper = source[a:source.index('\n}', a) + 2]
code = r'''
#include "efs/raft.h"
#include "efs/wire.h"
#include "efs/batch_obs.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
int efs_batch_obs_on;
void efs_batch_obs_count(enum efs_batch_counter k){(void)k;}
struct efs_raft_host {int unused;};
struct inbox_tag {int fenced;};
struct host_inbox_item {uint8_t *buf;uint32_t len;uint64_t queued_us;struct inbox_tag tag;};
static int adm_check_control(const struct inbox_tag *t){return t->fenced?EFS_ERR_STALE:EFS_OK;}
static struct efs_raft_msg decoded[10];
static int calls,seen[10],refused;
int efs_wire_raft_decode(const uint8_t *p,uint32_t n,struct efs_raft_msg *m,
                        uint8_t *b,uint32_t z) {
 (void)n;(void)b;(void)z;if(p[1]==9)return EFS_ERR_PROTO;
 *m=decoded[p[1]];return EFS_OK;
}
static struct efs_raft *group_raft(struct efs_raft_host *h,uint8_t g) {
 return g==2 ? (struct efs_raft *)h : NULL;
}
int efs_raft_read_control(struct efs_raft *r,const struct efs_raft_msg *m) {
 seen[calls++]=(int)m->last_log_index;return r&&!refused?2:0;
}
''' + helper + r'''
int main(void) {
 struct efs_raft_host h={0};
 uint8_t frames[10][EFS_WIRE_RAFT_HDR_LEN+1]={0},saved[sizeof(frames)];
 struct host_inbox_item items[10],before[10];
 for(int i=0;i<10;i++) {
  frames[i][0]=EFS_RAFT_MSG_AE_REQ;frames[i][1]=(uint8_t)i;
  items[i]=(struct host_inbox_item){frames[i],EFS_WIRE_RAFT_HDR_LEN,100+i,{0}};
  decoded[i].type=EFS_RAFT_MSG_AE_REQ;decoded[i].group=2;
  decoded[i].last_log_index=100+i;decoded[i].vote_granted=1;
 }
 items[0].len++;decoded[1].nentries=1;decoded[2].prev_index=1;
 decoded[3].prev_term=1;decoded[4].last_log_index=0;
 decoded[5].vote_granted=0;frames[6][0]=EFS_RAFT_MSG_AE_REP;
 decoded[8].group=0; /* Unknown group delegates refusal to the core. */
 memcpy(saved,frames,sizeof(frames));memcpy(before,items,sizeof(items));
 host_note_queued_reads(&h,items,10);
 assert(calls==2&&seen[0]==107&&seen[1]==108);
 assert(!memcmp(saved,frames,sizeof(frames))&&!memcmp(before,items,sizeof(items)));
 /* Storage/term/config refusal must retain the identical original frames. */
 refused=1;host_note_queued_reads(&h,items,10);
 assert(calls==4&&seen[2]==107&&seen[3]==108);
 assert(!memcmp(saved,frames,sizeof(frames))&&!memcmp(before,items,sizeof(items)));
 /* D39 C7 (b): an item whose connection tuple the binding no longer
  * admits is skipped here (and refused again at the drain). */
 refused=0;items[7].tag.fenced=1;host_note_queued_reads(&h,items,10);
 assert(calls==5&&seen[4]==108);
 puts("queued read control: zero-prefix fresh probes, core refusal, original frame retention, per-item admission PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-queued-read-') as d:
    path = Path(d) / 'test.c'
    path.write_text(code)
    binary = path.with_suffix('')
    subprocess.run(['cc', '-std=c99', '-D_GNU_SOURCE', '-Wall', '-Wextra',
                    '-Werror', '-I' + str(root / 'include'), str(path),
                    '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
