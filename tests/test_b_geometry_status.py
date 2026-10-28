#!/usr/bin/env python3
"""Unknown protection status cannot satisfy the full-member geometry gate."""
import ast
import re
from pathlib import Path
from types import SimpleNamespace

source = (Path(__file__).with_name('test_b_geometry.py')).read_text()
tree = ast.parse(source)
function = next(n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name == 'wait_status')
phase = next(n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name == 'full_phase')
gates = [n for n in ast.walk(phase) if isinstance(n, ast.Call) and
         isinstance(n.func, ast.Name) and n.func.id == 'wait_status']
assert len(gates) == 2
predicates = [next(k.value for k in n.keywords if k.arg == 'out_pred') for n in gates]
for predicate in predicates:
    check = eval(compile(ast.Expression(predicate), '<actual geometry predicate>', 'eval'), {'re': re})
    for replies, expected, expected_time in [
        (['Degraded chunks: unknown ', 'Degraded chunks: 2 ', 'Degraded chunks: 0 '], 'Degraded chunks: 0 ', 1.0),
        (['Degraded chunks: unknown '], 'Degraded chunks: unknown ', 2.0),
        (['Degraded chunks: 2 '], 'Degraded chunks: 2 ', 2.0),
    ]:
        clock = [0.0]
        calls = [0]
        def status(_c, _i):
            text = replies[min(calls[0], len(replies)-1)]
            calls[0] += 1
            return {'usable': {'capacity': 1024, 'free': 0}}, text
        def sleep(seconds):
            clock[0] += seconds
        namespace = {'status': status, 'time': SimpleNamespace(time=lambda: clock[0], sleep=sleep),
                     'b_checks': SimpleNamespace(statfs=lambda _: {'total': 1024})}
        exec(compile(ast.Module(body=[function], type_ignores=[]), '<actual wait_status>', 'exec'), namespace)
        _, answer = namespace['wait_status'](None, 1, SimpleNamespace(path=None), lambda _: True,
                                              timeout=2, out_pred=check)
        assert answer == expected and clock[0] == expected_time, (answer, clock)
        assert calls[0] > 1
print('geometry status: unknown/positive counts wait; zero required; original deadline preserved PASS')
