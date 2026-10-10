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

## Supplementary-group bounds repair

The installed Intel libfuse header specifies that `fuse_req_getgroups()` returns
the total group count even when the caller's buffer is smaller. The permission
helper supplied 64 slots but iterated over the returned total. A controlled
100-group request reaches an out-of-bounds stack read in the unmodified source
under UBSan/ASan.

The helper keeps small lists on the stack, obtains larger lists into bounded
storage (at most 65536 groups), and refuses unavailable, allocation-failed or
newly oversized second reads. A valid group beyond the first 64 is recognized;
only directories/entries whose group matches the complete caller list receive
the group permission class. Primary-group and no-request paths retain their
prior behavior. This does not expand the separate query wire group limit.

`tests/test_permission_groups.py` executes the actual helper with sanitizers,
covering small/large membership, missing groups, list growth/shrink, API errors,
allocation failure and the upper bound. The baseline's index-64 overread is
captured as a negative control. Live Intel users currently have short group
lists; the large-list condition is exercised by the production-function fixture.

The persistence inheritance fixture passes both prepare and verify on native
Linux on Intel1 and Intel2. Native macOS directory-bit inheritance differs and
is not used as the Linux oracle. The fixture explicitly sets its new parent's
group before applying setgid, so the expected ownership is independent of
native BSD default group inheritance.
