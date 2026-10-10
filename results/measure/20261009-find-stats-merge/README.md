# devel-find-stats committed-branch integration — October 9, 2026

Merge boundary: f4f95ed7. Current local branch tips are in branch-boundary.txt;
every listed tip is an ancestor of the merged branch. No uncommitted changes in
other worktrees are imported. No push or deployed-service change.

- 9e87533a merges devel fee363f5. Generated architecture HTML is regenerated
  from the merged sources, preserving both sets of status/backlog updates.
- 5e0fe113 merges devel-w66 70daa5a4, including retained PUT admission manifests
  and durable mapping receipt bindings.
- f4f95ed7 merges devel-persistence-pressure 437b7890. Confirmed GETCHUNKS absence
  returns a hole without erasing retained staging, including NOT_FOUND and DECODE
  recovery cases. Inbox retirement uses the opt-in helper while preserving newer
  checkpoint-failure log suppression and the shutdown KV durability tail.

Focused stale-read and inbox-retirement regressions pass locally; all five
architecture checks pass. Full Linux build/unit validation runs from the committed
archive in xefs1:/home/efs/w92-merged-f4f95ed7. This private directory does not
replace /tmp/efs or stop services. Runtime remains b4212453 from the preceding
profiling task. Full Linux make all and make test both complete with exit code 0.
The build log contains no compiler warning lines. Full logs are retained as
w92-merged-build.log and w92-merged-test.log. Final indexed documentation
is validated separately locally after the source-only merge snapshot.
Every recorded branch tip is verified as an ancestor. This is build/unit
acceptance, not live acceptance of newly merged protocol behavior.
