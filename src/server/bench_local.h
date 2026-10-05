#ifndef EFS_BENCH_LOCAL_H
#define EFS_BENCH_LOCAL_H

struct efsd_server;

/* Single-node storage bench (P3): drives the fragment store, the writer
 * pool, the KV and the Raft log exactly as the PUT/GET handlers and the
 * metadata engine do — no cluster, no sockets, no quorum.
 *   kind "data": --storage roots, write+read ladders (paths × QD).
 *   kind "meta": --meta-storage (or the first --storage root), KV + Raft log.
 * Storage roots must be bench-scratch: they are created if absent and the
 * bench export tree is removed between path-count blocks and at the end. */
int server_run_local_bench(struct efsd_server *s, const char *kind,
                           double time_sec);

#endif
