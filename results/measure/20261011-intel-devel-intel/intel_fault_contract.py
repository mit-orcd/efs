"""Honor the fault harness's documented GAP exit without calling GAP a PASS."""


def fault_contract(rows, exit_code):
    expected = {('sim', case, None, None) for case in (
        'one_server_lost', 'restart', 'one_fragment_corrupt',
        'missing_checksum', 'two_fragments_unavailable',
        'repair_restores_protection')}
    expected.update(('integration', case, node, None)
                    for case in ('one_server_lost', 'restart')
                    for node in (1, 2, 3, 4))
    expected.update(('integration', 'one_fragment_corrupt', None, fragment)
                    for fragment in (0, 1, 2))
    expected.update(('integration', case, None, None) for case in (
        'missing_checksum', 'crash_after_fsync', 'crash_before_fsync',
        'repair_restores_protection'))
    identities = [(r.get('layer'), r.get('case'), r.get('node_id'),
                   r.get('fragment_index')) for r in rows]
    return (exit_code == 2 and len(rows) == 21
            and len(set(identities)) == 21 and set(identities) == expected
            and all(r.get('status') == (
                'GAP' if r.get('case') == 'repair_restores_protection'
                else 'PASS') for r in rows))


def main_contract(rows, faults, extra_names=()):
    names = [r.get('name') for r in rows]
    extras = {'all-independent-live-gates', 'fault-simulator-build',
              'direct-fault-integration', 'buffered-fault-integration'}
    extras.update(extra_names)
    count = 114 + len(extra_names)
    if len(rows) != count or len(set(names)) != count or not extras <= set(names):
        return False
    for row in rows:
        mode = next((m for m in ('direct', 'buffered')
                     if row['name'] == m + '-fault-integration'), None)
        if mode:
            if not fault_contract(faults[mode]['rows'], row.get('exit')):
                return False
        elif row.get('exit') != 0:
            return False
    return True
