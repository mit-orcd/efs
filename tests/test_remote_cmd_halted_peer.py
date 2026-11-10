#!/usr/bin/env python3
"""Run the real host_remote_cmd_impl: during an election a replica that hosts
the group waits for its own leader view and never guesses a halted peer.

Task FF: a deposed coordinator's finisher forwarded DECIDE ABORT while its
group elected; the peer guess landed on a SIGSTOPped member and held the 30 s
mutation wait, so the client's unlink stayed BUSY for its whole budget.
EFS_RAFT_HOST_SOURCE=<old raft_host.c> is the negative control (must fail)."""
from pathlib import Path
import os, shlex, subprocess, tempfile
import csrc
root = Path(__file__).resolve().parents[1]
s = csrc.module('raft_host')
start = s.index('static int host_remote_cmd_impl(')
impl = s[start:s.index('\n}', start) + 2]
code = r'''
#include "efs/common.h"
#include "efs/protocol.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#define HOST_TICK_US 5000
#define HOST_READ_TRIES 80
struct efs_raft_host { int raft_id; };
static uint64_t now_us;                 /* simulated clock */
static uint64_t elected_at_us;          /* local view names rid 0 from here */
static int hosted, halted_calls, leader_calls, picks;
static int usleep(unsigned us) { now_us += us; return 0; }
static void host_deadline_us(struct timespec *ts, long us)
{ uint64_t t = now_us + (uint64_t)us; ts->tv_sec = (time_t)(t / 1000000); ts->tv_nsec = (long)(t % 1000000) * 1000; }
static int host_past_deadline(const struct timespec *e)
{ return now_us >= (uint64_t)e->tv_sec * 1000000 + (uint64_t)e->tv_nsec / 1000; }
static int host_submit_read_only(const uint8_t *c, uint32_t n) { (void)c; (void)n; return 0; }
static int efs_net_remaining_ms(int cap) { return cap; }
static int host_local_leader(struct efs_raft_host *h, uint8_t g)
{ (void)h; (void)g; return hosted && now_us >= elected_at_us ? 0 : -1; }
__attribute__((unused)) static int host_local_view(struct efs_raft_host *h, uint8_t g)
{ (void)h; (void)g; return hosted; }
/* Membership order: rid 0, then rid 2 (self is rid 1). */
static int host_pick_peer(struct efs_raft_host *h, const uint8_t *g, int ng, int skip)
{ (void)g; (void)ng; picks++; for (int r = 0; r < 3; r++) if (r != h->raft_id && r != skip) return r; return -1; }
static void host_fwd_leader_note(uint8_t g, int rid) { (void)g; (void)rid; }
/* rid 2 is SIGSTOPped: TCP accepts, nothing answers, the full wait runs. */
static int host_rpc_submit(struct efs_raft_host *h, int rid, uint8_t g, const uint8_t *c,
                           uint32_t n, struct efs_msg_raft_mkfs_reply *rep, int io_ms)
{
    (void)h; (void)g; (void)c; (void)n;
    memset(rep, 0, sizeof(*rep));
    if (rid == 2) { halted_calls++; now_us += (uint64_t)io_ms * 1000; return EFS_ERR_IO; }
    leader_calls++;
    now_us += 200;
    if (now_us < elected_at_us) { rep->rc = EFS_ERR_NOT_PRIMARY; rep->leader_hint = -1; return EFS_OK; }
    rep->rc = EFS_OK; rep->leader_hint = 0; return EFS_OK;
}
''' + impl + r'''
static int run(int host, uint64_t election_us)
{
    struct efs_raft_host h = { .raft_id = 1 };
    struct efs_msg_raft_mkfs_reply rep;
    uint8_t cmd[4] = { 1, 2, 3, 4 };
    now_us = 0; elected_at_us = election_us; hosted = host;
    halted_calls = leader_calls = picks = 0;
    int rc = host_remote_cmd_impl(&h, 2, cmd, sizeof(cmd), &rep, -1);
    fprintf(stderr, "hosted=%d election_ms=%llu rc=%d rep.rc=%d halted_calls=%d leader_calls=%d picks=%d elapsed_ms=%llu\n",
            host, (unsigned long long)(election_us / 1000), rc, rep.rc, halted_calls, leader_calls, picks,
            (unsigned long long)(now_us / 1000));
    return rc == EFS_OK ? rep.rc : rc;
}
int main(void)
{
    /* The coordinator group elects for 300 ms after the step-down. */
    assert(run(1, 300000) == EFS_OK && halted_calls == 0 && picks == 0);
    assert(now_us < 400000);
    /* An election longer than the routing budget returns NOT_PRIMARY at the
     * budget (the finisher re-tries), still without touching the halted peer. */
    assert(run(1, 2000000) == EFS_ERR_NOT_PRIMARY && halted_calls == 0 && now_us < 500000);
    /* No local replica: peers are the only source of a leader, as before. */
    assert(run(0, 0) == EFS_OK && picks > 0 && leader_calls == 1);
    puts("remote cmd: a hosting replica waits for its own leader view and never asks a halted peer PASS");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='efs-remote-cmd-halted-') as d:
    p = Path(d); (p / 'test.c').write_text(code)
    subprocess.run(shlex.split(os.environ.get('CC', 'cc')) + ['-std=gnu11', '-Wall', '-Wextra', '-Werror',
                   '-I' + str(root / 'include'), str(p / 'test.c'), '-o', str(p / 'test')], check=True)
    subprocess.run([str(p / 'test')], check=True)
