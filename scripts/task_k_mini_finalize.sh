#!/bin/bash
# Close a successful Task K evidence tree without stopping the product cluster.
# Run only after all test controllers and raw-output copies have finished.
set -euo pipefail
host=$(hostname -s)
case "$host" in node9900|node9901) ;; *) exit 2;; esac
root=/data1/efs/evidence/taskK-orcd-mini-20261027
python3 "$root/capture.py"
python3 - "$root" "$host" <<'PY'
import json,sys
from pathlib import Path
root=Path(sys.argv[1]);host=sys.argv[2]
if host=='node9900':
    for name in ('sweep-exit','query-exit','two-client-exit'):
        assert (root/name).read_text().strip()=='0',name
    for name in ('rdma-sweep','stable-queries'):
        assert json.loads((root/name/'result.json').read_text())['verdict']=='PASS',name
    text=(root/'two-client.log').read_text()
    assert '68/68 pass' in text and '0 fail' in text,text[-1000:]
else:
    assert (root/'deployment-exit').read_text().strip()=='0'
    assert json.loads((root/'peer-full-read/result.json').read_text())['verdict']=='PASS'
    for i in (1,2,3):
        text=(root/f'previous-n{i}-exit.txt').read_text()
        assert 'outcome=orderly' in text and 'rc=0' in text
text=(root/'final-status.txt').read_text()
assert 'Cluster state: OK (4 up, 0 down)' in text,text
assert 'Degraded chunks: 0' in text and 'Degraded segments: 0' in text,text
assert '4/4 identical' in text,text
PY
if [[ "$host" == node9901 ]]; then
 tar -tzf "$root/previous-logs.tar.gz" > /dev/null
else
 tar -tzf "$root/previous-client-logs.tar.gz" > /dev/null
fi
python3 - "$root" <<'PY'
import hashlib,sys
from pathlib import Path
root=Path(sys.argv[1]);rows=[]
for file in sorted(root.rglob('*')):
    assert not file.is_symlink(),file
    if not file.is_file() or file.name=='SHA256SUMS':continue
    digest=hashlib.sha256()
    with file.open('rb') as stream:
        for block in iter(lambda:stream.read(1<<20),b''):digest.update(block)
    rows.append(f'{digest.hexdigest()}  {file.relative_to(root)}\n')
(root/'SHA256SUMS').write_text(''.join(rows))
PY
cd "$root"
sha256sum -c SHA256SUMS > /data1/efs/evidence/taskK-orcd-mini-20261027-verify.log
sha256sum SHA256SUMS
