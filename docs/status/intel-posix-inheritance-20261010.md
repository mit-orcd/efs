# Intel POSIX creation inheritance follow-up — Oct 10, 2026

## Confirmed issue

On Intel2, the integrated `d579c3bf` FUSE client created a child directory
with mode `0750` inside a `2770` parent. The same efs account on native `/tmp`
created mode `2750`. Both paths used the same umask and primary group.
The FUSE CREATE, MKDIR and SYMLINK callbacks also always supplied the creator's
primary group, even when the parent directory carried setgid.

## Repair and focused evidence

All three callbacks derive ownership from a shared credential helper:
new entries under a setgid directory take its group; only new directories
inherit the setgid bit. UID remains the creator's UID. Ordinary parents
retain the caller group, including the existing internal no-request context.
No wire format, permission-admission rule or storage format changes.

`tests/test_create_credentials.py` compiles and executes the actual production
CREATE, MKDIR and SYMLINK functions with different caller/parent groups.
It checks group inheritance, directory-only bit inheritance, ordinary parents
and internal calls. The unmodified `800ce361` source fails the same test;
the repaired source passes on macOS. Linux and live Intel acceptance are
recorded after deployment, rather than inferred from this helper result.

A new single-client POSIX case checks setgid propagation through two directory
levels, regular-file bit behavior and symlink group ownership. The Intel efs
account has no alternate group for a live cross-group ownership check; that
condition is covered by the production-function regression with distinct GIDs.

## Confirmed competing-MKDIR false success

A synchronized, two-physical-host probe on the deployed `d579c3bf` clients
reported two successful `mkdir` calls for one new name in 16 of 20 trials.
Four trials correctly had one success and one EEXIST. Each trial used a new,
disposable name; its directory was removed only after both calls completed.

The FUSE callback performed a lookup after a refused MKDIR and treated an
existing directory as proof that this request had succeeded. A competing
request's directory is not such proof. The repaired callback preserves the
original failure, including known EEXIST and unknown BUSY, without substituting
another creator's inode. Existing create errno mapping still distinguishes
quota, access and authority failures. This does not extend legacy directory
CREATE into the retained regular-file CREATE protocol.

The production-function test now asserts EEXIST/BUSY/quota refusals leave the
output inode untouched. Source `0f540108` fails that deterministic assertion.
A new peer case synchronizes both clients across eight fresh names and requires
exactly one successful mkdir and one EEXIST for each. The native macOS two-client
fixture passes; deployment acceptance follows.
