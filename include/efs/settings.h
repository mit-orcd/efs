#ifndef EFS_SETTINGS_H
#define EFS_SETTINGS_H
/* S4/W122: the process's effective settings, recorded as they are read, for
 * `efs-mgmt settings`. Read-only; a note never changes a value. */
#include <stddef.h>

#define EFS_SETTINGS_DAEMON 1u
#define EFS_SETTINGS_CLIENT 2u
/* Counts of the kinds the listing does not carry (settings inventory;
 * scripts/gen_settings.py keeps them equal to it). */
#define EFS_SETTINGS_OBSERVABILITY 35
#define EFS_SETTINGS_FAULT_HOOKS 61

/* Start the table for this process: rows for `role` take the environment
 * text (or the default) until their reader notes the effective value. */
void efs_settings_init(unsigned role);
/* The effective value as the reader resolved it. Provenance: environment
 * when the variable is set and was accepted, else built-in (automatic when
 * the default is "auto"). Unknown names are ignored. */
void efs_setting_u64(const char *name, unsigned long long value);
void efs_setting_str(const char *name, const char *value);
void efs_setting_cmdline(const char *name, unsigned long long value);
/* Transport as resolved at startup (efs_transport_startup). */
void efs_settings_transport(const char *how);
/* Tab-separated lines "name value default provenance kind", sorted by kind
 * then name, after a "#" header. Returns the length; *truncated is set when
 * the page did not fit. */
size_t efs_settings_render(char *buf, size_t size, int *rows, int *truncated);
#endif
