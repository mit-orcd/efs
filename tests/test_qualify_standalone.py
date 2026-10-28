#!/usr/bin/env python3
"""Standalone gates retain quiet-host isolation and restore owned fixtures."""
import importlib.util
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

root = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('qualify', root / 'scripts/qualify.py')
Q = importlib.util.module_from_spec(spec)
spec.loader.exec_module(Q)


def exercise(verdict=Q.PASS, *, owned=True, allow=True, busy=False,
             stop_rc=0, ready=True, raises=False, plan=False):
    events = []
    mounted = {'value': True}
    running = {'value': True}
    m = dict(host='local', path='/tmp/owned/mount')

    def client_stop(g, mount):
        events.append('unmount')
        mounted['value'] = False
        return 0, ''

    def stop(g):
        events.append('stop')
        running['value'] = False
        return stop_rc, 'stop refused' if stop_rc else ''

    def start(g):
        events.append('start')
        running['value'] = True
        return 0, ''

    def client_start(g, mount):
        events.append('mount')
        mounted['value'] = True
        return 0, ''

    def plugin(q, g):
        events.append('plugin')
        if not plan:
            assert not running['value'] and not mounted['value'], 'fixture overlaps standalone workload'
        if raises:
            raise ValueError('original plugin failure')
        return verdict, 'original verdict'

    control = SimpleNamespace(owned=owned, has=lambda *a: True,
        client_stop=client_stop, stop_cluster=stop, start_cluster=start,
        client_start=client_start)
    q = SimpleNamespace(control=control, allow={'restart'} if allow else set(),
                        cluster=dict(mounts=[m]), plan=plan)
    g = SimpleNamespace(step=lambda *a: events.append('permission'),
                        run=lambda *a: (0, '1234') if busy else (1, ''))
    with patch.object(Q, 'g_plugin', return_value=plugin), \
         patch.object(Q, 'wait_cluster_ready', return_value=ready):
        try:
            result = Q.g_standalone_plugin('soak', {})(q, g)
        except Exception as e:
            result = e
    return result, events, running['value'], mounted['value']


for verdict in (Q.PASS, Q.FAIL, Q.NOTRUN):
    result, events, running, mounted = exercise(verdict)
    assert result == (verdict, 'original verdict')
    assert events == ['permission', 'unmount', 'stop', 'plugin', 'start', 'mount']
    assert running and mounted
for case in (dict(owned=False), dict(allow=False), dict(busy=True)):
    result, events, _, _ = exercise(**case)
    assert result[0] == Q.NOTRUN and not events
result, events, _, _ = exercise(stop_rc=1)
assert result[0] == Q.FAIL and 'plugin' not in events
result, events, _, _ = exercise(ready=False)
assert isinstance(result, RuntimeError) and 'fixture restore failed' in str(result)
assert events[-1] == 'start' and 'mount' not in events
result, events, running, mounted = exercise(raises=True)
assert isinstance(result, ValueError) and str(result) == 'original plugin failure'
assert running and mounted and events[-2:] == ['start', 'mount']
plugin = Q.default_plugins({}, [])['soak']
assert plugin['standalone'] and 'restart' in plugin['needs']
print('standalone gate: no overlapping fixture, no external stop, checked restore, original failures retained PASS')


# D5 owns four sockets; never collide with the live parent fixture. The
# original loopback-3 used 18940 for both and failed before expiry ran.
for base, first, count in ((18900, 18940, 3), (19600, 19740, 5),
                           (18700, 18740, 16), (19000, 19044, 3)):
    q = SimpleNamespace(cluster={'nodes': [dict(addr=f'127.0.0.1:{p}')
                         for p in range(first, first + count)]},
                        fixture=dict(posix_jobs=4, posix_timeout_s=150))
    commands, fixture = Q.b_d5(q, '/source', '/owned/d5', base)
    start, end = map(int, fixture['ports'].split('-'))
    assert end == start + 3
    assert not set(range(start,end+1)) & set(range(first,first+count)), (base,fixture)
    assert not set(range(start,end+1)) & Q.RESERVED_PORTS
    assert f'--port {start} ' in commands[0][1]
    if base == 19600:
        assert start == 19640, 'non-overlapping five-member range need not change'
q = SimpleNamespace(cluster={'nodes': []},fixture=dict(posix_jobs=4,posix_timeout_s=150))
try:Q.b_d5(q,'/source','/owned/d5',65500)
except ValueError:pass
else:raise AssertionError('overflow D5 range accepted')
print('D5 port ownership: three/five/sixteen members, caller overlap, overflow rejection PASS')


# --plan must not call in-process controls, wait readiness, or alter mounts.
result, events, running, mounted = exercise(plan=True, ready=False)
assert result == (Q.PASS, 'original verdict')
assert events == ['plugin'] and running and mounted, events
print('standalone --plan: command only, no stop/start/readiness or mount effects PASS')
