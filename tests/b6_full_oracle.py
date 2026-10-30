"""K53: when may a writer succeed on a full node 3 (B6 full mode)?

Capacity can legitimately appear after the refusal (a pack retirement, a GC
pass or a session cleanup lands late), so "every writer gets ENOSPC" is too
strong. A success is explained when the bytes it admitted fit the room left
at the fill plus the bytes node 3 reclaimed since; anything else, or a
ledger above its quota, is a product finding. Pure, so
tests/test_b6_full_oracle.py replays recorded traces.
"""
FRAGMENT = 65536    # one 128 KiB chunk charges one 64 KiB fragment per node
CHUNK = 128 * 1024


def admitted_bytes(size):
    return -(-size // CHUNK) * FRAGMENT


def all_enospc(writers):
    """The oracle before K53: every writer refused."""
    return all(w[1] == 'ENOSPC' for w in writers)


def judge(quota, fill_used, trail, writers, reclaimed):
    """(ok, why). trail: [(label, used)] ledger samples after the fill;
    writers: [(name, verdict, size)]; reclaimed: bytes node 3's reclaim
    counters moved since the fill sample."""
    over = [t for t in trail if t[1] > quota]
    if over:
        return False, f'node 3 used above its quota {quota}: {over}'
    bad = [w for w in writers if w[1] not in ('ok', 'ENOSPC')]
    if bad:
        return False, f'writer neither ok nor ENOSPC: {bad}'
    won = [w for w in writers if w[1] == 'ok']
    if not won:
        return True, 'every writer refused ENOSPC'
    admitted = sum(admitted_bytes(w[2]) for w in won)
    freed = max(reclaimed, sum(max(0, a[1] - b[1]) for a, b in zip(trail, trail[1:])))
    room = quota - fill_used
    why = (f'{[w[0] for w in won]} admitted {admitted} B; room at the fill {room} B, '
           f'reclaimed since {freed} B')
    if admitted <= room + freed:
        return True, 'explained: ' + why
    return False, 'unexplained success: ' + why
