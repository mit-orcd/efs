# Consistent EFS help — Oct 8, 2026

User requirement: all EFS binaries and directory-local commands use the same
help format, with a concise default overview. Worktree/branch:
`work/efs-find-stats`, `devel-find-stats`; main devel checkout is untouched.

The five native programs (`efsd`, `efs-fuse`, `efs-bench`, `efs-mgmt`,
`efs-query`), standalone `blake3-bench`, six benchmark modes and administrative
subcommands use `include/efs/cli_help.h`. The matching Python formatter supplies
`.efs` help. Both use description, Usage, applicable Commands/Arguments,
Options, Examples, Notes, two-space indentation and a 22-character label field.
Success help goes to stdout and exits zero before network/storage/mount work.
`-h` and `--help` are supported. FUSE's upstream reference is explicitly
separate (`--help-fuse`). Existing argument errors remain nonzero/stderr;
management argument syntax diagnostics now use the same capitalized Usage
label/indentation. The ignored legacy mkfs label remains explicitly documented.

`.efs/help` shows command descriptions, a few examples and output options;
`.efs/help --extended` holds literal-pattern limits, snapshot semantics, expiry
availability, Python and noexec requirements. Each command's `--help` has its
own options/examples. Reading any command still returns source and performs
no query. Query/storage/permission/durability behavior is unchanged.

Python is not a storage or query-protocol requirement. FUSE returns file
bytes and has no execute(argv) callback. The client OS must run an interpreter
or native program to parse `find PATTERN` and encode a query path. The selected
self-contained scripts use Python 3 for portable argument/JSON/raw-filename
handling; the shebang starts it for direct invocation. A native client wrapper
could replace that dependency but would need the corresponding OS/CPU build.

The maintenance rule is recorded in
[developing](../how-it-works/developing.md#command-line-help-style).
Final candidate: `c22754236b38-help-abf6838b`. Exact source patch, build/test/help
output and deployment evidence are retained under
`results/measure/20261008-help-format` (archived raw evidence: `archive/raw-evidence-20261021:results/measure/20261008-help-format`).
Relevant existing checks: benchmark CLI ownership/parser/scratch protection and
14 directory-command/codec/source tests. All native help entry points are also
inspected for success/stdout/section layout. Engine acceptance is inherited from
the preceding `58997ac5` rollout; these help-only edits do not close W91/W92's
loaded freshness/scale/fault/NFS gates and do not justify claiming new engine
performance or rerunning unrelated broad suites.

NUC rollout uses matching clean-built binaries, graceful generator/client/node
drains and preserved data. The generator resumes in the existing `tiny_files`
screen under a fresh output directory. Final observed rollout details follow.

Final rollout succeeded on NUC. All six installed binary hashes match the
clean-tested candidate. **82 native help entry points** (both flags, all
administrative subcommands and local benchmark modes) and four standalone
Python command-help pages passed stdout/status/section checks; CLI regression
and 14 command tests passed. The user's exact path,
`/data1/efs/mnt/tiny-files1/.efs/help`, shows the revised overview; extended help
contains the interpreter and limit details. Root stats still returned a complete
server snapshot after restart. Data/configuration was preserved. Generator PID
2185374 at launch runs in the same screen, new directory
`/data1/efs/mnt/tiny-help-abf6838b`. Binary rollback copies remain in
`/data1/efs/help-rollback-abf6838b`. No code was committed during this update.
