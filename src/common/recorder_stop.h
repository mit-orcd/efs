#ifndef EFS_RECORDER_STOP_H
#define EFS_RECORDER_STOP_H
#include <errno.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
/* The caller owns this child and is its only reaper. Never signal after a
 * successful reap/ECHILD: the numeric PID is no longer owned by us. */
static inline void efs_stop_recorder(pid_t *pidp)
{
    pid_t pid = *pidp;
    int sent_term = 0;
    if (pid <= 0)
        return;
    for (int i = 0; i < 50; i++) {
        pid_t result = waitpid(pid, NULL, WNOHANG);
        if (result == pid || (result < 0 && errno == ECHILD)) {
            *pidp = -1;
            return;
        }
        if (result < 0) {
            if (errno == EINTR)
                continue;
            return; /* Unknown ownership: retain the handle, do not signal. */
        }
        if (!sent_term) {
            kill(pid, SIGTERM);
            sent_term = 1;
        }
        usleep(100000);
    }
    if (!sent_term)
        return; /* Every ownership check was interrupted. */
    kill(pid, SIGKILL);
    pid_t result;
    do {
        result = waitpid(pid, NULL, 0);
    } while (result < 0 && errno == EINTR);
    if (result == pid || (result < 0 && errno == ECHILD))
        *pidp = -1;
}
#endif
