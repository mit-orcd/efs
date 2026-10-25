#!/usr/bin/env python3
"""K35 (D42): on four or more members, writes place all three fragments on
available members, so a member that is down (or failed a PUT hard, before
the four failures that mark it down) leaves no degraded stripe behind.

The real client placement (efs_place_fragments, efs_place_spare) and the
real reroute (reroute_untried_fragments with its full and away marks, taken
from src/client/write.c) on 3, 4 and 5 members, every member down in turn,
over 4096 (inode, chunk) pairs: on 4 and 5 every stripe has three distinct
available members and every available member takes a share; on 3 the down
member stays in the stripe (no spare: a degraded acknowledgement, W154).
An away mark expires after PUT_AWAY_MARK_MS.

K39: on 4 and 5 members, one member really down and another only marked
down by this client (it returned; the 30 s cooldown has not ended): the
returned member is probed (spare_reprobe) and takes the down member's
fragments, so no stripe keeps the down member; each marked member is
probed at most once per PUT_SPARE_PROBE_MS, not per stripe. Negative control: when the marked
member is really down too, the probe fails, the down member stays (no
spare, as before) and the probe is still rate-limited. Fast (`make test`).
"""
from pathlib import Path
import subprocess, tempfile
root = Path(__file__).resolve().parents[1]
s = (root / 'src/client/write.c').read_text()


def fn(name):
    a = s.index(name + '(')
    a = s.rfind('\n', 0, a) + 1
    return s[a:s.index('\n}', a) + 2]


def macro(name):
    a = s.index('#define ' + name + ' ')
    return s[a:s.index('\n', a) + 1]


code = r'''
#include "efs/placement.h"
#include "efs/protocol.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static struct { struct efs_node nodes[EFS_MAX_NODES]; uint32_t node_count; } g_client;
static uint64_t now_ms = 1000000;
static uint64_t report_clock_ms(void) { return now_ms; }
static efs_node_id_t down_id, stale_id;
static int stale_back;
static unsigned probes;
static uint64_t deadline;
static int efs_client_node_is_down(efs_node_id_t id) { return id == down_id || id == stale_id; }
/* The real probe connects; here the marked member answers when stale_back. */
static int efs_client_node_reprobe(efs_node_id_t id)
{
    assert(id == stale_id || id == down_id);
    probes++;
    if (id == stale_id && stale_back) {
        stale_id = 0;
        return 1;
    }
    return 0;
}
static uint64_t efs_client_rpc_deadline_ms(void) { return deadline; }
static void efs_client_rpc_set_deadline_ms(uint64_t d) { deadline = d; }
static int efs_client_rpc_past_deadline(void) { return deadline && now_ms >= deadline; }
''' + macro('PUT_FULL_MARK_MS') + macro('PUT_AWAY_MARK_MS') + \
    'static uint64_t g_node_full_until_ms[EFS_MAX_NODES + 1];\n' + \
    'static uint64_t g_node_away_until_ms[EFS_MAX_NODES + 1];\n' + \
    macro('PUT_SPARE_PROBE_MS') + 'static uint64_t g_node_spare_probe_ms[EFS_MAX_NODES + 1];\n' + \
    fn('static int spare_reprobe') + fn('static int node_is_full') + fn('static uint64_t put_away_mark_ms') + fn('static void node_note_away') + \
    fn('static int node_is_away') + fn('static void reroute_untried_fragments') + r'''
static void place(efs_ino_t ino, uint32_t ci, efs_node_id_t out[3])
{
    efs_place_fragments(g_client.nodes, g_client.node_count, ino, ci, out);
    reroute_untried_fragments(ino, ci, out);
}
int main(void)
{
    for (uint32_t n = 3; n <= 5; n++) {
        g_client.node_count = n;
        for (uint32_t i = 0; i < n; i++) {
            memset(&g_client.nodes[i], 0, sizeof(g_client.nodes[i]));
            g_client.nodes[i].id = (efs_node_id_t)(i + 1);
            snprintf(g_client.nodes[i].addr, sizeof(g_client.nodes[i].addr), "127.0.0.%u", i + 1);
            g_client.nodes[i].port = 9000;
        }
        for (efs_node_id_t d = 0; d <= n; d++) {
            unsigned share[EFS_MAX_NODES + 1] = {0}, with_down = 0;
            down_id = d;
            for (efs_ino_t ino = 2; ino < 2 + 512; ino++)
                for (uint32_t ci = 0; ci < 8; ci++) {
                    efs_node_id_t f[3];
                    place(ino, ci, f);
                    assert(f[0] != f[1] && f[1] != f[2] && f[0] != f[2]);
                    for (int k = 0; k < 3; k++) {
                        assert(f[k] >= 1 && f[k] <= n);
                        share[f[k]]++;
                        with_down += d && f[k] == d;
                    }
                }
            if (n >= 4)
                assert(!with_down);            /* never a degraded stripe */
            else if (d)
                assert(with_down == 4096);     /* 3 members: no spare */
            for (efs_node_id_t k = 1; k <= n; k++)
                if (k != d || n == 3)
                    assert(share[k] > 4096 * 3 / n / 2); /* every available member */
            printf("members=%u down=%u shares=%u,%u,%u,%u,%u\n", n, d, share[1], share[2],
                   share[3], share[4], share[5]);
        }
        /* A member that failed a PUT hard is avoided before it is marked
         * down, and offered again once the mark expires. */
        down_id = 0;
        if (n >= 4) {
            node_note_away(2);
            for (efs_ino_t ino = 2; ino < 200; ino++) {
                efs_node_id_t f[3];
                place(ino, 0, f);
                assert(f[0] != 2 && f[1] != 2 && f[2] != 2);
            }
            now_ms += PUT_AWAY_MARK_MS + 1;
            unsigned back = 0;
            for (efs_ino_t ino = 2; ino < 200; ino++) {
                efs_node_id_t f[3];
                place(ino, 0, f);
                back += f[0] == 2 || f[1] == 2 || f[2] == 2;
            }
            assert(back > 0);
        }
        /* K39: member d really down, member m only stale-marked down. */
        if (n >= 4) {
            for (int back = 1; back >= 0; back--) {
                for (efs_node_id_t d = 1; d <= n; d++) {
                    efs_node_id_t m = d % n + 1;
                    unsigned with_down = 0, on_m = 0;
                    down_id = d;
                    stale_id = m;
                    stale_back = back;
                    probes = 0;
                    deadline = 0;
                    now_ms += PUT_SPARE_PROBE_MS;
                    memset(g_node_spare_probe_ms, 0, sizeof(g_node_spare_probe_ms));
                    for (efs_ino_t ino = 2; ino < 2 + 512; ino++)
                        for (uint32_t ci = 0; ci < 8; ci++) {
                            efs_node_id_t f[3];
                            place(ino, ci, f);
                            assert(f[0] != f[1] && f[1] != f[2] && f[0] != f[2]);
                            for (int k = 0; k < 3; k++) {
                                with_down += f[k] == d;
                                on_m += f[k] == m;
                            }
                        }
                    assert(!deadline); /* the probe's bound is restored */
                    if (back) {
                        /* at most one probe per marked member per window;
                         * on five a live spare is always left: no probe */
                        assert(!with_down && probes <= 2);
                        if (n == 4)
                            assert(on_m > 0 && probes >= 1 && !stale_id);
                        else
                            assert(!probes && stale_id == m);
                    } else {
                        /* both really down: on 4 members a stripe holding
                         * both keeps one (no spare); one probe per window */
                        assert(probes <= 2 && stale_id == m && (n == 5 || probes >= 1));
                        if (n == 4)
                            assert(with_down > 0);
                        else
                            assert(!with_down); /* five: a third spare is left */
                        now_ms += PUT_SPARE_PROBE_MS;
                        efs_node_id_t f[3];
                        unsigned before = probes;
                        for (efs_ino_t ino = 2; ino < 2 + 64 && probes == before; ino++)
                            for (uint32_t ci = 0; ci < 8; ci++)
                                place(ino, ci, f);
                        assert(n == 5 || probes > before);
                    }
                    printf("K39 members=%u down=%u stale=%u back=%d probes=%u on_stale=%u with_down=%u\n",
                           n, d, m, back, probes, on_m, with_down);
                }
            }
            down_id = stale_id = 0;
        }
    }
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='efs-k35-place-') as d:
    p = Path(d) / 't.c'
    p.write_text(code)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-I' + str(root / 'include'),
                    '-I' + str(root / 'src/common'), str(p), str(root / 'src/common/placement.c'),
                    '-o', str(p.with_suffix(''))], check=True)
    out = subprocess.run([str(p.with_suffix(''))], check=True, timeout=30,
                         capture_output=True, text=True).stdout
print(out, end='')
print('K35 placement: on 4 and 5 members every stripe goes to three available members '
      '(down or away member avoided, every available member takes a share); on 3 the '
      'down member stays (degraded, W154); away marks expire; K39: a member only stale-marked '
      'down is probed and takes the down member\'s share, once per window PASS')
