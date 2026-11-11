# PUT-window ownership

[PUT-window ownership](../put-window-ownership-20261110.md): **PASS, no K row, blocks alpha: no** (Nov 10, claude/task-u1-put-window-72984d). The PUT window state, mutex and five functions moved out of write.c into put_window.c behind put_window.h with a compiled 10-case interface test; instruction counts identical with inlining off, make test, routed write tests, ASan and TSan lanes all pass on intel1.
