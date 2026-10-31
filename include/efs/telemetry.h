#ifndef EFS_TELEMETRY_H
#define EFS_TELEMETRY_H
#include <stddef.h>
#include <stdio.h>

/* One bounded mailbox per pump. Pending periodic snapshots coalesce; producers
 * never wait for the mailbox, the stdio lock or the sink. The renderer runs
 * only on the reporter thread. stop drains what the sink accepts for 200 ms. */
#define EFS_TELEMETRY_BYTES 16384
struct efs_telemetry;
typedef void (*efs_telemetry_render)(FILE *, const void *);
struct efs_telemetry *efs_telemetry_start(efs_telemetry_render render);
void efs_telemetry_submit(struct efs_telemetry *, const void *, size_t);
void efs_telemetry_printf(struct efs_telemetry *, const char *, ...)
    __attribute__((format(printf, 2, 3)));
/* Routine daemon lines (K79: drop-in for fprintf(stderr, ...), formatted on the
 * calling thread, written by the reporter) share the active host reporter.
 * Critical events and processes without a reporter (startup before the host
 * starts, abort paths, tools) retain synchronous stderr. */
void efs_telemetry_log(int critical, const char *, ...)
    __attribute__((format(printf, 2, 3)));
/* Wait (at most 200 ms) until the bound reporter has written the queued lines. */
void efs_telemetry_flush(void);
void efs_telemetry_stop(struct efs_telemetry *);
#endif
