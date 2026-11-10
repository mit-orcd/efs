# Task CS — K54 harness port race

[Task CS — K54 harness port race](../k54-harness-port-race-20261103.md): **PASS, harness defect, no K row**. `tests/test_k54_member_down_fence.py` failed with `Address already in use` on a restarted member because `ticket_put.Cluster` released its probed ports at construction; it now holds one SO_REUSEADDR socket per member port until `close()`. With 8 loopback port churners on node9900: devel harness 24 of 30, fixed 30 of 30; clean-build gate `FAIL 0`.
