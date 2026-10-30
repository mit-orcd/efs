#ifndef EFS_TELEMETRY_H
#define EFS_TELEMETRY_H
#include <stddef.h>
#include <stdio.h>

/* One bounded mailbox per pump. Pending periodic snapshots coalesce; producers
 * never wait for the mailbox, the stdio lock or the sink. No critical events
 * belong here. The renderer runs only on the reporter thread. */
#define EFS_TELEMETRY_BYTES 16384
struct efs_telemetry;
typedef void (*efs_telemetry_render)(FILE *, const void *);
struct efs_telemetry *efs_telemetry_start(efs_telemetry_render render);
void efs_telemetry_submit(struct efs_telemetry *, const void *, size_t);
void efs_telemetry_printf(struct efs_telemetry *, const char *, ...)
    __attribute__((format(printf, 2, 3)));
/* Common transport diagnostics share the active host reporter. Critical
 * events and processes without a reporter retain synchronous stderr. */
void efs_telemetry_log(int critical, const char *, ...)
    __attribute__((format(printf, 2, 3)));
void efs_telemetry_stop(struct efs_telemetry *);
#endif
