#ifndef EFS_LOG_TS_H
#define EFS_LOG_TS_H

/* Replace stdout/stderr with line-buffered streams that prefix every
 * line with a UTC timestamp ("2026-10-01T13:41:24.780Z "). Call once at
 * the top of a daemon's main(), before any thread. EFS_LOG_TS=0 skips it. */
void efs_log_timestamps_install(void);

#endif
