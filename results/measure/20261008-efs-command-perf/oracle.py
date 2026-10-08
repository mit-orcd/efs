import os,json,subprocess,hashlib,time
root='/data1/efs/mnt/tiny-files4';start=time.monotonic();expected=set();nfiles=ndirs=0
for directory,dirs,files in os.walk(root):
 nfiles+=len(files);ndirs+=len(dirs)
 for name in dirs+files:
  if name.startswith('file'):
   expected.add(os.fsencode(os.path.relpath(os.path.join(directory,name),root)))
p=subprocess.run([root+'/.efs/find','file','--match','prefix','--json'],capture_output=True,check=True)
result=json.loads(p.stdout);found=[bytes.fromhex(x['path_hex']) for x in result['entries']]
assert len(found)==len(set(found)), 'duplicate returned paths'
assert set(found)==expected, (len(expected-set(found)),len(set(found)-expected))
s=json.loads(subprocess.check_output([root+'/.efs/stats','--json'],stderr=subprocess.DEVNULL))
assert s['stats']['tree_files']==nfiles,(s['stats'],nfiles)
assert s['stats']['tree_dirs']==ndirs,(s['stats'],ndirs)
print(json.dumps(dict(status='PASS',files=nfiles,dirs=ndirs,matching_paths=len(found),path_sha256=hashlib.sha256(b'\0'.join(sorted(found))).hexdigest(),seconds=time.monotonic()-start),indent=2))
