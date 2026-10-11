#!/usr/bin/env python3
from pathlib import Path
import hashlib,json,subprocess
root=Path('/data1/efs/amd-round-20261010/w94-ab')
sources={'separate':Path('/data1/efs/devel-amd'),'combined':Path('/data1/efs/devel-amd-adopt')}
expected={'separate': 'f729a565ce68e315744b47baf4a4f778a2f0a5452c7908484ee6429dd9c0b76d', 'combined': '62f1896d4abbd3c69bfaac8f202de50023157f99da2e6283d37883a26b959d0e'}
paths=('src/server/raft_host.c','src/client/inode_rpc.c','src/client/efs_fuse.c')
proof={v:{p:hashlib.sha256((source/p).read_bytes()).hexdigest() for p in paths} for v,source in sources.items()}
full={v:{str(p.relative_to(source)):hashlib.sha256(p.read_bytes()).hexdigest() for folder in ('src','include') for p in (source/folder).rglob('*') if p.is_file() and p.suffix in ('.c','.h')} for v,source in sources.items()}
assert full['separate'].keys()==full['combined'].keys()
differences=[path for path in full['separate'] if full['separate'][path]!=full['combined'][path]]
assert differences==['src/client/efs_fuse.c'],differences
for path in paths[:2]:assert proof['separate'][path]==proof['combined'][path],path
for variant,digest in expected.items():assert proof[variant][paths[2]]==digest,(variant,proof)
diff=subprocess.run(['diff','-u',str(sources['separate']/paths[2]),str(sources['combined']/paths[2])],capture_output=True,text=True)
assert diff.returncode==1,diff.stderr
(root/'adoption-source.patch').write_text(diff.stdout)
(root/'source-controls.json').write_text(json.dumps(dict(source_hashes=proof,expected_fuse_hashes=expected,matching_server_rpc=True,production_source_hashes=full,production_source_differences=differences),indent=2)+'\n')
print('frozen source comparison: identical server/RPC; exact expected separate/combined FUSE revisions PASS')
