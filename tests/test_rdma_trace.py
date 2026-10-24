#!/usr/bin/env python3
"""D42: EFS_RDMA_TRACE (renamed from EFS_RDMA_FIRST) is off by default and
changes only diagnostics.

Source check over every reader: efs_rdma_trace_on() is the one reader of
EFS_RDMA_TRACE; it is on only for the value 1; and every statement it guards
calls nothing but stderr formatting, backtraces and read-only identity
helpers, assigns only to variables it declares itself, and never returns,
breaks, jumps or changes a connection. The removed name is refused at startup
(tests/test_env_removed.py). Behavior check: the reader itself, compiled
from src/common/common.c, under unset, 0, 1, true and empty values.

Usage: python3 tests/test_rdma_trace.py"""
from pathlib import Path
import os, re, subprocess, tempfile
root = Path(__file__).resolve().parents[1]

ALLOWED_CALLS = {'fprintf', 'fflush', 'backtrace', 'backtrace_symbols_fd', '__sync_fetch_and_add',
                 'getpeername', 'ntohs', 'efs_rdma_qpn', 'efs_conn_fd_matches', 'strerror', 'sizeof',
                 'if', 'while', 'for', 'efs_rdma_trace_on'}
KEYWORDS = {'static', 'int', 'unsigned', 'socklen_t', 'struct', 'sockaddr_in', 'void', 'const', 'char',
            'uint32_t', 'uint64_t', 'if', 'else', 'stderr', 'errno', 'sizeof'}


def guarded(text, at):
    """The statement guarded by the `if (...efs_rdma_trace_on()...)` at `at`."""
    i = text.index('(', text.rindex('if', 0, at))
    depth = 0
    for j in range(i, len(text)):
        depth += text[j] == '('
        depth -= text[j] == ')'
        if depth == 0:
            break
    k = j + 1
    while text[k].isspace():
        k += 1
    if text[k] == '{':
        depth = 0
        for m in range(k, len(text)):
            depth += text[m] == '{'
            depth -= text[m] == '}'
            if depth == 0:
                return text[k:m + 1]
    return text[k:text.index(';', k) + 1]


sites = 0
for path in sorted((root / 'src').rglob('*.c')):
    text = path.read_text()
    assert 'EFS_RDMA_FIRST' not in text or path.name == 'common.c', path
    if 'getenv("EFS_RDMA_TRACE")' in text:
        assert path.name == 'common.c', f'{path}: only efs_rdma_trace_on reads EFS_RDMA_TRACE'
    for m in re.finditer(r'efs_rdma_trace_on\(\)', text):
        line = text[text.rindex('\n', 0, m.start()) + 1:text.index('\n', m.start())]
        if line.startswith('int efs_rdma_trace_on'):
            continue
        assert re.match(r'\s*if \((?:[\w>.-]+ && )?efs_rdma_trace_on\(\)\)', line), \
            f'{path}: {line.strip()} (only an if guard)'
        body = guarded(text, m.start())
        # Strip string literals and comments so their words are not taken for code.
        code = re.sub(r'"(\\.|[^"\\])*"', '""', body)
        code = re.sub(r'/\*.*?\*/|//[^\n]*', ' ', code, flags=re.S)
        calls = set(re.findall(r'\b([A-Za-z_]\w*)\s*\(', code))
        assert calls <= ALLOWED_CALLS, f'{path}: trace block calls {calls - ALLOWED_CALLS}'
        assert not re.search(r'\b(return|break|continue|goto)\b', code), f'{path}: trace block alters control flow'
        declared = set(re.findall(r'\b(?:int|unsigned|socklen_t|void \*|struct sockaddr_in)\s+\**(\w+)', code))
        declared |= set(re.findall(r'static int (\w+)', code))
        for target in re.findall(r'\b([A-Za-z_]\w*)(?:\[[^\]]*\])?\s*(?<![=!<>])=(?!=)', code):
            assert target in declared, f'{path}: trace block assigns {target}, which it does not own'
        sites += 1
assert sites >= 7, sites

# The reader: off unless exactly "1".
common = (root / 'src/common/common.c').read_text()
a = common.index('int efs_rdma_trace_on(void)')
b = common.index('\n}\n', a) + 3
program = '#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n' + common[a:b] + \
    'int main(void){printf("%d\\n",efs_rdma_trace_on());return 0;}\n'
with tempfile.TemporaryDirectory(prefix='efs-rdma-trace-') as d:
    src, exe = Path(d) / 't.c', Path(d) / 't'
    src.write_text(program)
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-Wall', '-Werror', str(src), '-o', str(exe)], check=True)
    base = {k: v for k, v in os.environ.items() if k != 'EFS_RDMA_TRACE'}
    for value, want in ((None, '0'), ('0', '0'), ('1', '1'), ('true', '0'), ('', '0')):
        env = dict(base) if value is None else dict(base, EFS_RDMA_TRACE=value)
        out = subprocess.run([str(exe)], env=env, capture_output=True, text=True, check=True).stdout.strip()
        assert out == want, (value, out)
print(f'RDMA trace: {sites} guarded sites print diagnostics only; off by default, on only for 1 PASS')
