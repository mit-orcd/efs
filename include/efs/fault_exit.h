/* Test-only crash emulation for the EFS_FAULTS hooks (EFS_FAULT_CRASH,
 * EFS_FAULT_PACK_CRASH, EFS_FAULT_COMPACT_DIE, EFS_FAULT_MEMBER_EXIT): end
 * the process now, as SIGKILL would, with the given status.
 *
 * The raw exit_group system call, not _exit(): libtsan intercepts _exit and
 * finalises its runtime first, which took about a second under
 * tests/test_k36_recovery.py on the F1 TSan gate, and the "crashed" daemon
 * kept answering meanwhile (a W154 rebuild PUT landed on node 3 after its
 * crash point, so the next recovery found one lost fragment back and the
 * test's counts were off). No sanitizer, atexit or stdio path runs after
 * the call. The routine lines queued on the K79 reporter are written first
 * (bounded, 200 ms, so a "crashed" node answers at most that much longer);
 * the caller flushes its own synchronous stdio. */
#ifndef EFS_FAULT_EXIT_H
#define EFS_FAULT_EXIT_H
#include <unistd.h>
#include "efs/telemetry.h"
#ifdef __linux__
#include <sys/syscall.h>
#endif

static inline void efs_fault_exit(int status)
{
    efs_telemetry_flush();
#if defined(__linux__) && defined(SYS_exit_group)
    syscall(SYS_exit_group, status);
#endif
    _exit(status);
}
#endif
