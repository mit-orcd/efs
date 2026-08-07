#ifndef EFS_NUMA_LOCALITY_H
#define EFS_NUMA_LOCALITY_H

#include "efs/common.h"
#include <net/if.h>
#include <sched.h>
#include <stdint.h>

/* Parse Linux cpulist text (e.g. "0-3,8,10-11") into set. Returns 0 on success. */
int efs_numa_parse_cpulist(const char *text, cpu_set_t *set);

/* Intersect set with the calling process affinity. Returns 0 if non-empty. */
int efs_numa_intersect_process_affinity(cpu_set_t *set);

/* Fill set with CPUs of NUMA node N (already intersected). Returns 0 on success. */
int efs_numa_cpus_for_node(int node, cpu_set_t *set);

/* Best-effort: filesystem path → block device NUMA + CPUs.
 * Returns 0 if affinity is usable; on failure *node_out is -1. */
int efs_numa_for_path(const char *path, int *node_out, cpu_set_t *set);

/* Best-effort: peer host:port → egress NIC NUMA + CPUs (+ optional ifname).
 * Returns 0 if affinity is usable; on failure *node_out is -1. */
int efs_numa_for_peer(const char *host, uint16_t port, int *node_out, cpu_set_t *set,
                      char *ifname, size_t ifname_len);

/* Format a cpu_set_t as a compact cpulist into buf (for logging). */
void efs_numa_format_cpuset(const cpu_set_t *set, char *buf, size_t buflen);

/* Best-effort pthread_setaffinity_np for the given set (ignores errors). */
void efs_numa_apply_affinity(const cpu_set_t *set);

/* Soft NUMA is opt-in: returns 1 only when EFS_NUMA_AFFINITY is 1/on/true.
 * Default (unset/empty/other) is off — no discovery and no pinning. */
int efs_numa_affinity_enabled(void);

#endif
