# K114 uncovered-waits design

[K114 uncovered-waits design](../k114-uncovered-waits-design-20261111.md): **design study, Nov 11 (Task FY), no code changed**; for each reply wait outside the K106 (a) cut it says whether a resend is safe. SETATTR truncate, APPEND and CREATE_RETAINED already carry a stored op-id and can join the cut; chmod, chown, utimens, XATTR, HOLD and FLOCK have no stored seq and are not safe against a delayed duplicate; blocking FLOCK, RDMA and a powered-off host stay uncovered. Row K114 unchanged
