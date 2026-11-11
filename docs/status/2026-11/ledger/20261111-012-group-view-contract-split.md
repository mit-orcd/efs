# Group view ownership Task V6

- [Group view ownership Task V6](../group-view-ownership-20261111.md): **branch claude/task-v6-group-view-contract-70fa8b, not merged, Nov 11**: group_view.h split into the 181-line caller contract and the private 208-line group_view_impl.h (representation and inline bodies, #error on direct inclusion); disassembly of raft_host.o and group_view.o identical; privacy check gains impl-include and slot-access rules and a self-test; make test, ASan and TSan PASS
