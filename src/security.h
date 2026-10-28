/* Nordstjernen — startup security: refuse-root + landlock + seccomp sandbox.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */

#ifndef NS_SECURITY_H
#define NS_SECURITY_H

#include <glib.h>

G_BEGIN_DECLS

gboolean ns_security_env_flag(const char *name);
void ns_security_runtime_dir_read_only(void);
void ns_security_config_dir_read_only(void);
gboolean ns_security_refuse_root(void);
gboolean ns_security_require_landlock(void);
int ns_security_landlock_abi(void);

typedef struct {
    int      landlock_abi;
    gboolean landlock_enforced;
    gboolean landlock_all_threads;
    gboolean seccomp_enforced;
    gboolean seccomp_disabled;
    gboolean no_new_privs;
    gboolean seatbelt_enforced;
    gboolean win32_mitigations;
    gboolean win32_child_processes_blocked;
    gboolean allocator_hardened;
    gboolean sandbox_disabled;
} ns_security_status;

void ns_security_get_status(ns_security_status *out);

void ns_security_sandbox_init(const char *self_exe);

void ns_security_add_writable_dir(const char *dir);

void ns_security_add_exec_dir(const char *dir);

void ns_security_allow_gpu_devices(void);

void ns_security_seccomp_init(void);

void ns_security_seccomp_deny_unix_sockets(void);
void ns_security_seccomp_deny_exec_memory(void);

void ns_security_win32_mitigations_init(gboolean allow_child_processes);

gboolean ns_security_csprng_fill(void *buf, gsize len);

gboolean ns_security_sri_check(const char *integrity_attr,
                               const void *body,
                               gsize       body_len);

void ns_security_mark_download_origin(const char *path, const char *url);

void ns_security_harden_allocator(void);

G_END_DECLS

#endif
