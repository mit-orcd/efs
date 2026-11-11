# Group view ownership Task V3

- [Group view ownership Task V3](../group-view-ownership-20261111.md): **branch claude/task-v3-group-view-ceb2d8, not merged, Nov 11**: the published group view and its sleepers moved from raft_host.c (26,712 to 26,314 lines) to group_view.c/.h, wait lock bracket because the three waits have three predicates, 11-case owner test, privacy check, 20 splice tests ported; instruction counts identical or explained, make test, ASan and TSan PASS
