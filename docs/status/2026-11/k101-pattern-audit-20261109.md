# K101 pattern audit of the FUSE client's per-caller caches (Task EO)

**Verdict: PASS (Nov 9), no defect found. Audit only: no source or test
change, no K row, blocks alpha: no.** K101
([Task EC](find-res-ownership-20261108.md)) had two parts. The `.find`
result cache matched by directory, term, uid and gid but not by the
supplementary groups the server authorized with (shape 1). It also
fetched under one hold of its mutex and matched again under a second
hold, where another caller could replace the slot (shape 2). This audit
asks the same two questions of every other table or cache in
`src/client/efs_fuse/` and `src/client/`. Only two server RPCs carry the
caller's identity: the query snapshot used by `.find`, which K101 already
fixed, and `.stats`, which keeps no cache. Every other table holds data
the server sends the same way whoever asks. Each is keyed by inode,
open description or lock owner, and permission is checked on the
client, per request, against a fresh row. Branch
`claude/optimistic-murdock-ac0414` on devel `33cd0a839`. Merged into devel in wave 67 (Nov 9).

- Purpose: the follow-up K101 left open: does any other per-caller cache
  in the FUSE client have the K101 shape? The K101 row is in
  [the checklist](../alpha-1.0-2plus1-checklist.md).
- Source: devel `33cd0a839` (wave 65), read unchanged. This page, the
  ledger paragraph and the graph rebuild are the only commits.
- Environment: the Mac only (reading). No reproducer was needed, so
  node9900 was not used and no build or test ran there. No raw output
  exists.
- Method: `graphify explain client/efs_fuse` and
  [`src/client/efs_fuse/README.md`](../../../src/client/efs_fuse/README.md)
  for the list of owners (13 files). Then a grep of the two trees for
  every use of caller identity (`ctx->uid`, `ctx->gid`, `ctx->pid`,
  `fuse_req_getgroups`, `query_credentials`, `groups`). Then the wire
  structs of the RPCs each cache stores. A cache counts as
  identity-dependent only if the server's answer depends on who asked.
  Answer (3) applies only where (1) or (2) is "less" or "re-matched".

## Which RPCs the server authorizes by caller

| RPC | Carries | Evidence |
|---|---|---|
| query snapshot (`.find`, `.stats`) | uid, gid, up to `EFS_QUERY_GROUPS` supplementary groups | `include/efs/protocol.h:1064`; filled by `query_credentials` (`support.c:705`, groups at `:711`), called only at `find_res.c:80` and `support.c:184` |
| GETATTR | export id, ino | `include/efs/protocol.h:1186`; `inode_rpc.c:1534` |
| LOOKUP | export id, parent, name | `include/efs/protocol.h:1132`; `inode_rpc.c:1265` |
| READDIR, GETCHUNKS, lease, flock | export id, ino (lease and lock owner tokens are the mount's, not the caller's) | `getattr.c:493`, `open.c:413`, `plock_init.c:125`, `:243` |
| CREATE, MKDIR, SYMLINK | uid and gid as the new row's owner (data, not authority) | `namespace.c:109`, `:222`, `:460`; `ops.c:1409` |

The client checks permission itself with `check_access` and
`caller_in_group` (`support.c:871`, `:909`), which reads all of the
caller's groups through `fuse_req_getgroups` on every request. It checks
against a fresh row: `getattr.c:482` (READDIR), `getattr.c:527` (ACCESS),
`open.c:859` (OPEN), `lookup.c:151` (LOOKUP search). Kernel
`default_permissions` is not used (`support.c:866`). Kernel caching is
off: `entry_timeout` and `attr_timeout` are 0 (`lookup.c:18`). NFS
re-export is off (`plock_init.c:372`), so a local caller reaches a
nodeid only by its own LOOKUP or through a descriptor someone passed it.

## Per-cache table

(1) Does the match key cover everything the server authorized with?
(2) Is the entry fetched and used under one hold of the mutex, or matched
again after a release? (3) What does a caller observe if (1) or (2)
fails?

| Cache (owner) | Key | (1) key vs authority | (2) fetch then use | (3) observable |
|---|---|---|---|---|
| `.find` result table `g_find_res` (`find_res.c:24`) | dir, term; uid, gid kept only as the publisher hint | Fixed by K101: every operation serves only the slot its own authorized fetch just filled (`find_res.c:134`, used at `:145`, `:164`, `:178`, `:194`). The uid/gid match at `find_res_caller` (`:40`) picks which node to ask first and serves no bytes (`:74`); the server checks the request again | One hold: `find_query_ensure` returns with `g_find_res_mu` held and `find_res_done` releases it after use (`:34`, `:134`) | None (closed by K101). A wrong hint only routes the RPC to another node |
| Virtual query inodes `g_vq` (`support.c:524`) | dir, term, FNV version of the result bytes, length (`support.c:551`) | Covers it: a row is found by a caller's own fetched bytes, so two identities share a nodeid only when their results are byte-identical. GETATTR of the pinned size with no handle (`getattr.c:196`, `:241`) reaches a row only through that caller's own LOOKUP (`entry_timeout` 0) or a passed descriptor | One hold: `vq_intern` finds or creates and reserves under `g_vq_mu` (`support.c:545`–`580`). OPEN checks the version under both `g_find_res_mu` and `g_vq_mu` (`find_res.c:194`, `support.c:674`). A reused slot gets a new nodeid (`support.c:569`), so a stale nodeid misses | No wrong data. A result that changed between LOOKUP and OPEN gets `ESTALE` (`find_res.c:195`), and the kernel retries with a fresh LOOKUP. That is a true change, not another identity |
| `.stats` snapshot (`support.c:178`) | none (no cache) | Not applicable: `stats_snapshot` fetches with the caller's credentials into the caller's own `stats_ent` (`support.c:197`). An open description copies the bytes (`open.c:815`) | Not applicable | None |
| Open virtual snapshots `h->snapshot` (`support.c:446`) | open description | Not applicable: the bytes belong to the description the opener's own authorized fetch created; later READs on that fd are POSIX open-time permission | Copied under the owner's hold (`find_res.c:194`) | None |
| LOOKUP memo `g_lookup_memo` (`getattr.c:26`) | ino | Covers it: the stored row comes from GETATTR or LOOKUP, which carry no caller identity (`protocol.h:1132`, `:1186`), so the row is the same for every caller. Any later operation checks permission per request on the client (above) | `lookup_memo_take` matches and consumes the row under one hold (`getattr.c:124`–`150`). Mutations fence it with a serial (`getattr.c:54`, `:83`, 0j) | None |
| Thread-locals `t_getattr_row`, `t_getattr_ino`, `t_lookup_memo_serial` (`getattr.c:30`–`32`) | handler thread | Not shared: set and read inside one FUSE request (`getattr.c:160`, `lookup.c:60`, `ll_run.c:20`–`23`) | Not applicable | None |
| `t_open_read_size` (`open.c:127`) | handler thread | Not shared: set and cleared around one `efs_open_acquire` (`open.c:879`–`882`) | Not applicable | None |
| READDIR listing `struct efs_dirh` (`getattr.h:55`) | open directory (`fi->fh`) | Not applicable: filled once after the opener's `R_OK` check against a fresh row (`getattr.c:472`–`483`) and reused by later READDIRs on the same descriptor (`ll_run.c:557`–`563`), which is POSIX open-time semantics. Entry rows carry no caller identity | One request at a time per descriptor (`getattr.h:15`) | None |
| Open-reference table `g_open_refs` (`open.c:11`, `:35`) | ino | Not identity-keyed: one lease per (mount session, inode); the lease request carries no caller identity. OPEN checks permission first (`open.c:859`) | Under `g_open_mu` (`open.c:34`); rows are never handed out (README) | None |
| Write-back pool `g_wb` and per-description errors (`writeback.c:38`, `:75`; `support.c:418`) | ino, open description | Not identity-keyed: dirty chunks and the sticky error belong to the inode or the description, not the writer's identity | Private mutex (`writeback.h`) | None |
| Append and pressure state (`append.c:150`), bounce pool (`writeback.c:89`), statfs capacity (`namespace.c:276`), feature mask `g_feat_cache` (`support.c:228`) | none, peer, export | Not identity-keyed: mount- or export-wide | Not applicable | None |
| POSIX lock table `g_plocks` (`plock_init.c:11`) | ino, `lock_owner` | The key is the kernel's lock owner, which is the right POSIX key; no uid authority is involved | `setlk` takes the server grant, then checks for conflicts again under `g_plock_mu` and rolls the grant back on a conflict (`plock_init.c:207`–`221`) | None |
| Deep-callback sampler `callers[128]` (`lookup.c:260`) | pid | Diagnostic only (`EFS_DEEP_PATH_PROF`); selects which pids get timing samples, serves no data | Not applicable | None |
| Client library: staging table, dirty cache, chunk map, node marks (`src/client/README.md:6`–`8`) | ino, chunk, peer | Not identity-keyed: rows come from GETATTR, LOOKUP and GETCHUNKS, which carry no caller identity. CREATE's uid and gid are the new row's owner (`ops.c:1409`) | Not applicable to this question | None |

## Checks

- No source or test change, so nothing was built or run on node9900. The
  doc gates ran on the Mac: `python3 scripts/check_doc_links.py`,
  `python3 scripts/status_layout.py`, `make docs-check`,
  `python3 scripts/check_k_register.py`. The code graph was rebuilt with
  `sh scripts/rebuild-code-graph.sh`, then `python3 tests/test_code_graph.py`.
- Reproduce: on devel `33cd0a839`, run
  `grep -rn -e query_credentials -e fuse_req_getgroups -e 'ctx->uid' -e 'ctx->gid' -e 'ctx->pid' src/client`.
  Then read the lines cited above, and the wire structs at
  `include/efs/protocol.h:1064`, `:1132`, `:1186`.

## Limits

- This is a reading audit by the implementing agent, not a blind reader.
  No concurrency test was added, because no cache was found with a
  re-match or a short key to test.
- Not covered: the server side (`src/server/`), which checks the query
  credentials (`raft_host.c:13422`) and the session identity
  (`raft_host.c:12960`). The kernel FUSE module is assumed to send
  LOOKUP on every path walk because `entry_timeout` is 0.
- If NFS re-export is ever turned on (D42, W152 are outside alpha), a
  remote caller could present the nodeid of another identity's `.find`
  result. GETATTR would then report that result's pinned length
  (`getattr.c:241`); OPEN still returns `ESTALE`. That length leak would
  need a recheck when W152 is planned.
