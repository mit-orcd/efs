# K54 one-off (Task CU)

[K54 one-off (Task CU)](../k54-revoked-epoch-oneoff-20261103.md): verdict **C**. CS's single `returned member stored a revoked-epoch fragment` failure is not the port race (victim answered STALE and sealed its certificate; the port-race failures die earlier at `cluster.start`), but it did not recur in 60 churn runs on node9900 (49 pass, 9 port race, 2 new `participant-floor` revoke failures, 0 revoked-epoch). The assertion now prints the added/removed files; an *added* file on recurrence would open a K row. No K row added.
