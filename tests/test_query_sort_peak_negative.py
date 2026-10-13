#!/usr/bin/env python3
"""The measured file-byte oracle must reject missing sort scratch accounting."""
from pathlib import Path
import os
import subprocess
import tempfile
root = Path(__file__).resolve().parents[1]
source = (root/'src/meta/query_disk.c').read_text()
charge = 'rc=charge_disk(d,temporary);if(rc){free(rows);return rc;}'
release = 'if(!rc)d->disk_bytes-=temporary;'
assert source.count(charge) == source.count(release) == 1
mutant = source.replace(charge, 'rc=EFS_OK; /* negative control: no scratch charge */')
mutant = mutant.replace(release, '/* negative control: no scratch release */')
with tempfile.TemporaryDirectory(prefix='efs-sort-unaccounted-') as directory:
    work = Path(directory)
    (work/'src/meta').mkdir(parents=True)
    (work/'tests').mkdir()
    (work/'src/meta/query_disk.c').write_text(mutant)
    test = work/'tests/test_query_disk_io.c'
    test.write_text((root/'tests/test_query_disk_io.c').read_text())
    binary = work/'negative'
    command = [os.environ.get('CC', 'cc'), '-O1', '-g', '-std=gnu11',
               '-D_GNU_SOURCE', '-I'+str(root/'include'), '-I'+str(root/'src/meta'),
               '-I'+str(root/'src/common'), '-I'+str(root/'deps/blake3'), '-I'+str(root),
               str(test), str(root/'libefs.a'), '-lpthread', '-lm', '-ldl', '-libverbs',
               '-o', str(binary)]
    subprocess.run(command, check=True, capture_output=True)
    result = subprocess.run([str(binary)], capture_output=True, timeout=120)
    assert result.returncode != 0, 'unaccounted scratch passed the oracle'
    assert b'bytes<=peak_disk->disk_bytes' in result.stderr, result.stderr[-2000:]
print('sort scratch negative control: actual file-byte peak rejects omitted charge PASS')
