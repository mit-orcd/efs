#ifndef EFS_BENCH_LOCAL_H
#define EFS_BENCH_LOCAL_H

/* Saturate PATH with fragment-sized (64 KiB) synthetic writes for time_sec.
 * Writes under PATH/.efs-bench/ and removes that tree before return.
 * Returns 0 on success. */
int server_run_local_bench(const char *path, double time_sec, int writers,
                           int direct_io);

#endif
