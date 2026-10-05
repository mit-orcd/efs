#ifndef EFS_VERSION_H
#define EFS_VERSION_H

#include <stddef.h>

/* Human-facing build identity, baked in by the Makefile as -DEFS_VERSION
 * (git describe), -DEFS_GIT_BRANCH and -DEFS_BUILD_TIME. This is purely
 * informational — EFS_BUILD_ID stays the strict HELLO same-build gate. */

/* "prog v0.1.0-...-dirty (branch devel, built 2026-10-05T09:31:47Z on efs1)"
 * into buf (never overflows; buf is always NUL-terminated). prog may be an
 * argv[0] with a path; only the basename is printed. Returns buf. */
const char *efs_version_string(const char *prog, char *buf, size_t len);

/* If any of argv[1..argc) is "--version", print the version line and
 * exit(0). Call first thing in main(). */
void efs_version_check_argv(const char *prog, int argc, char **argv);

#endif
