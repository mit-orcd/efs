"""K53 oracle replay: the retained ORCD trace fails the old rule and is explained by the new one."""
from b6_full_oracle import all_enospc, judge

Q = 16 * 1024 * 1024
FILL = 16736256          # node 3 used after the fill: 40960 B of room
LARGE = 300000

# fcstor007 3769f44bb, case-464: W0 succeeded, W1 and the rest ENOSPC; the
# packer retired 8 seed fragments (524288 B) before the writers ran.
k53 = [('W0', 'ok', LARGE), ('W1', 'ENOSPC', LARGE), ('W2', 'ENOSPC', LARGE), ('Q0', 'ENOSPC', 1)]
trail = [('fill', FILL), ('before W0', FILL - 524288), ('after W0', Q - 65536), ('after Q3', Q - 65536)]
assert not all_enospc(k53), 'the old rule must reject the K53 trace'
ok, why = judge(Q, FILL, trail, k53, 524288)
assert ok, why

# The same success with nothing reclaimed is the product finding.
ok, why = judge(Q, FILL, [('fill', FILL), ('after W0', FILL)], k53, 0)
assert not ok and 'unexplained' in why, why

# A ledger above its quota fails whatever the writers saw.
ok, why = judge(Q, FILL, [('fill', FILL), ('after W0', Q + 65536)], k53, 524288)
assert not ok and 'above its quota' in why, why

# Refusals alone pass; a writer with another errno does not.
refused = [('W0', 'ENOSPC', LARGE), ('Q0', 'ENOSPC', 1)]
assert judge(Q, FILL, [('fill', FILL)], refused, 0)[0]
assert not judge(Q, FILL, [('fill', FILL)], [('W0', 'EIO', LARGE)], 0)[0]
print('b6 full oracle: ok')
