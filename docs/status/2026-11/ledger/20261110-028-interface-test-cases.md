# Named cases and a case filter in the interface tests

[Named cases and a case filter in the interface tests](../interface-test-cases-20261110.md): **PASS, no K row, blocks alpha: no** (Nov 10, branch `claude/competent-hermann-01b16e`). `test_wb_interface`, `test_open_interface` and `test_find_res_interface` now run named `test_...` cases with `--list` and a name filter (exit 2 for no match); same 49/157/50 checks, node9900 `make test` and both sanitizer lanes pass.
