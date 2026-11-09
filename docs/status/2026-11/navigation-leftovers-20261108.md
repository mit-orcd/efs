# Navigation leftovers from Task EB: guide drift, tag audit, ASan lane, prefix tags (Task ED)

**Verdict: PASS (Nov 9), all four pieces done; one drift reported, not
fixed, by design. (a) `scripts/check_guide_counts.py` runs in
`make docs-check` and checks every catalogue claim a component guide
states against `scripts/tests_for.py`; it fixed `src/client/README.md`
(`client/efs_fuse` "all 50", catalogue 55) and reports
`src/client/efs_fuse/README.md:23` (`.open` 7, catalogue 10), which Task
EC edits this wave, so `make docs-check` fails on this branch until the
merge refreshes that cell. (b) Every one of the 594 catalogue rows was
audited by one stated method; 71 rows gained second tags, 17 findings are
listed and left alone. (c) ASan+UBSan+LSan `open_interface` and
`wb_interface` on fstor007: PASS. (d) The `raft_host.c` prefix map is a
`# prefixes` line in the `tests/budgets.tsv` header: `tests_for.py
server/raft_host.publish` lists nothing (exit 1), `server/raft_host.publsh`
is refused (exit 2), and the graph has a `server/raft_host.publish`
component node.** fstor007 `make test` rc 0 (175 s with `make all`). No K row;
blocks alpha: no. Branch `claude/xenodochial-jepsen-f0bc41` on devel
`df7d11371` (wave 64). Merged into devel in wave 65 (Nov 9); the merge-time commit `3edd95ae1` refreshed the `.open` cell, so `make docs-check` passes on devel. Status Nov 9: remaining failures none; active run none; next action none on this page, W171 continues on the [structure plan](structure-plan-20261102.md).

- Purpose: finish the four pieces Task EB left
  ([EB page](taskEB-review-followup-20261108.md), item 2's limit and "Not
  done"): guide drift, a full tag audit, the ASan lane, prefix tags.
  W171 ([structure plan](structure-plan-20261102.md)), no release gate.
- Source: devel `df7d11371`; commit `ef0bff3fa` (all code and catalogue
  changes), then this page and the graph rebuild. ASan ran on the archive
  of `df7d11371`; `src/client/efs_fuse/`, `tests/test_open_interface.c`
  and `tests/test_wb_interface.c` are byte-identical between that commit
  and the branch head (`git diff --stat` empty). `make test` ran on the
  archive of `ef0bff3fa` (tar sha256 `0657d7ae…96bf`).

## Environment

fstor007 (ORCD, 96 CPUs, Linux 5.14.0-687.36.1.el9_8.x86_64, gcc 11.5.0),
user erbmi1 through orcd-login; libfuse 3.16.2 through a shim
`/tmp/efs-taskED-fuse` (`lib64`, `include` symlinks to the orcd-test deps)
with `PKG_CONFIG_PATH` and `LD_LIBRARY_PATH` at its `lib64` (the Makefile
reads `pkg-config fuse3`; `EFS_FUSE_PREFIX` alone does not reach it, which
made the first ASan attempt fail to build against the system fuse3:
`fuse_cmdline_opts` has no `max_threads`). Trees under `/tmp/efs-taskED`,
`TMPDIR=/tmp/efs-taskED-tmp`, screen sessions `efs-taskED-*`, no daemon
ports used. The guide check, tag audit and graph ran on the Mac worktree.

## (a) Guide counts

`scripts/check_guide_counts.py [GUIDE...]` (default: every
`src/**/README.md`), one line in `make docs-check`. It checks:

- every `` `tests_for.py TAG` `` a guide names resolves to rows;
- "`` `tests_for.py TAG` `` … all N" equals `len(tests_for TAG)`;
- "<Number in words> catalogue rows … the bare `` `TAG` `` tag" equals the
  rows carrying TAG itself;
- in a file table under "`tests_for.py BASE.<file>`", a Tests cell
  "`` `.x` (N `` equals `len(tests_for BASE.x)`, a bare "`` `.x` ``" has at
  least one row, and "bare tag" / "none routed" means `BASE.<file stem>`
  has no row of its own.

It cannot check numbers in any other wording, counts outside
`src/**/README.md` (status pages, `testing.md`), or whether a tag is right
(that is (b)). Its cases (each form right, each form drifted, a misspelled
command) are in `tests/test_tests_for.py` (`make test`).

Drift found on devel `df7d11371`: `src/client/README.md:31` all 50 →
55 (fixed); `src/client/efs_fuse/README.md:23` `.open` (7), catalogue 10
(not edited: Task EC owns the file this wave; the merge refreshes it).
The tag audit added no `client/efs_fuse*` tag, so the efs_fuse counts are
unchanged by this branch.

## (b) Tag audit

Method, per row of `tests/budgets.tsv` (594 rows), by
`scripts/evidence/ed/tag_audit.py`: take the files the command names
(`tests/…`, `scripts/…`, multi-line recipes through `make -n`), and from
their code (module docstring and comment lines dropped) collect the `src/`
implementation files the test **reads** (literal `src/…c` paths,
`'src'/'dir'/'file'` joins, `csrc.module()` splices, which expand to the
module's files), **#includes** (`#include "../src/….c"`, also inside
embedded C), **compiles** (a `src/….c` on a `cc` line) or **links** (a
`src/….c` prerequisite or recipe word of its Makefile rule; `$(LIB)`, the
whole library, is not counted). Each file maps to its component
(`src/server/reclaim.c` → `server/reclaim`; a module splice to the module,
`meta/meta_apply`); `kv`, `raft`, `data`, `wire` and `sim` keep their
directory tags as the catalogue already does. A component is a second
component when no row tag names its directory/file. The daemons and
mounts a test drives (`efsd`, a FUSE mount, `efs_mgmt`) are recorded per
row but do not add a tag: nearly every integration test drives all three,
so such a tag would route everything everywhere.

Result: 88 rows showed a second component; 71 changed, 17 left.
Tags added (rows): `kv` 20, `sim` 10, `server/reclaim` 9,
`server/segment_store` 9, `server/raft_host` 8, `common/checksum` 5,
`server/store` 4, `common/batch_obs`/`log_ts`/`telemetry` 4 each,
`server/efsd` 4, `server/put_upload`, `client/write`, `raft`,
`common/publication`, `client/inode_rpc`, `client/node_cache`,
`common/common` 3 each, and 28 tags on one or two rows. Most are link
dependencies (`kv_key.c` key encoding, `opid.c` op-id windows, the store
stack the C store tests `#include`); by the catalogue's rule ("reads or
links") they route a change of that file to the tests that would break.

Left alone (findings):

| Row (line) | Tag | Shows | Why not changed |
|---|---|---|---|
| `test_k33_bookkeeping.py` (62) | server/member_store | common/placement | named only in a function docstring (a Python twin) |
| `test_client_memory.py` (117), `test_put_drain.py` (278), `test_faults_env_ignored.py` (280), `test_pressure_cohort.py` (311), `test_create_errors.py` (402), `test_recorder_stop.py` (473), `test_declaration_gate_handler.py` (481) | various | `client/efs_fuse` (whole module through `csrc.module('efs_fuse')`) | other second tags added; the efs_fuse tag not added this wave (Task EC owns the efs_fuse counts) |
| `test_inode_lock_stripes.py` (216) | client/efs_fuse | client/ops | row tagged `client/efs_fuse*` (Task EC) |
| `test_wb_runtime.py` (340) | client/efs_fuse.write,client/write | client/writer_state | row tagged `client/efs_fuse*` |
| `test_address_parser.py` (475) | client/efs_fuse.main | client/efs_bench, client/node_cache, mgmt/efs_mgmt, server/efsd | row tagged `client/efs_fuse*` |
| `test_rdma_fork_poller.py` (482) | client/efs_fuse.main | common/rdma | row tagged `client/efs_fuse*` |
| `test_tests_for.py` (127), `test_graph_community_labels.py` (165), `test_tmpdir_hygiene.py` (310), `csrc.py` (641) | harness | source files as data | tooling that scans sources, not a component test |
| rdma-message recipe (152) | harness | common/rdma_message | compiles and runs `rdma_message.c`; a retag (`harness` → `common/rdma_message`), not a second component |

Re-running the audit on the branch head leaves exactly those 17 rows.
`python3 tests/test_tests_for.py` PASS, `python3 tests/test_make_test_recipe.py`
PASS, misspelling refusal (`meta/meta_apply.publsh`,
`server/raft_host.publsh`) exit 2.

## (c) ASan lane

```
EFS_SAN_ONLY="open_interface wb_interface" bash tests/sanitizer_check.sh asan
built asan tree with EXTRA_DEFS='-fsanitize=address,undefined -fno-omit-frame-pointer -march=x86-64-v3 -mtune=generic' (59s)
PASS wb_interface (0s)
PASS open_interface (0s)
VERDICT: PASS (2 run, 0 failed, 0s)
```

LeakSanitizer on (`detect_leaks=1` on Linux), UBSan with ASan,
`detect_stack_use_after_return=1`. EB ran only TSan `open_interface`.

## (d) Prefix tags

`tests/budgets.tsv` header: `# prefixes server/raft_host: tag=C prefixes …`
with the 20 tags of the plan's map (definitions with at least six
functions) under the catalogue's names: `read_index=host_read_`,
`publish=host_pub_,host_publish_`, `lane=host_bootstrap_lane,…`, and the
rest one-to-one. `host_reap_` from the plan's Nov 2 list defines no
function today and is left out. `tests_for.py` lists nothing for a
declared prefix with no rows (exit 1) instead of the parent's rows;
`build-code-graph.py` adds a component node per declared tag (guide
`src/server/README.md`, no tests); `tests/test_tests_for.py` checks the
publish/publsh exits and that every declared C prefix defines a function in
`raft_host.c`; `tests/test_code_graph.py` checks the node.

## Reproduce

```
git checkout <branch head>
python3 scripts/check_guide_counts.py            # 1 drift until EC's README merges
python3 scripts/evidence/ed/tag_audit.py . | awk -F'\t' '$5!=""'   # the 17 rows above
python3 tests/test_tests_for.py && python3 tests/test_code_graph.py
python3 scripts/tests_for.py server/raft_host.publish; echo $?   # 1, nothing listed
python3 scripts/tests_for.py server/raft_host.publsh; echo $?    # 2
# on a Linux host with fuse3 >= 3.12 visible to pkg-config:
EFS_SAN_ONLY="open_interface wb_interface" bash tests/sanitizer_check.sh asan
make all && make test
```

Cleanup: `/tmp/efs-taskED*` and the uploaded tars removed; no screen left.

## Raw evidence

fstor007 `~erbmi1/efs-artifacts/taskED-20261108/` (ASan output and first
failed-build attempt, environment, `make all`/`make test` logs and exits,
the tests_for and guide-check outputs, the audit TSVs before and after),
`SHA256SUMS` sha256 `62fbc6612a9864495b7e0b08074a41f841768f06dff4bfffd109f1369f3ab620` (14 files).

## Not done

- One host; no `make test-all` gate (fast suite only).
- `src/client/efs_fuse/README.md` `.open` count and the `client/efs_fuse`
  second tags of the eight rows above: Task EC's files this wave.
- The audit does not follow a test's Python helper imports (one level:
  the files the command names) and does not judge whether a linked file is
  exercised or only satisfies the link.
- `graphify query` free text still expands a component through `contains`
  (EB's limit); `graphify explain <tag>` remains the compact start.
