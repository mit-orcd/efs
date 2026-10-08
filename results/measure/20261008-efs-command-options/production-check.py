import json,os,pathlib,stat,subprocess,time,hashlib
root=pathlib.Path('/data1/efs/mnt/tiny-files4');out=pathlib.Path('/data1/efs/command-options/final')
rows=[]
def run(command,*args):
 start=time.monotonic();p=subprocess.run([str(root/'.efs'/command),*args],capture_output=True,timeout=45)
 assert p.returncode==0,(command,args,p.returncode,p.stderr)
 rows.append(dict(command=command,args=args,seconds=time.monotonic()-start,bytes=len(p.stdout),stderr=p.stderr.decode()))
 return p.stdout
result=json.loads(run('find','file','--match','prefix','--type','file','--sort','mtime','--json'))
assert len(result['entries'])==25725
entries=result['entries'];keys=[(-e['mtime_ns'],bytes.fromhex(e['path_hex'])) for e in entries];assert keys==sorted(keys)
for entry in entries[::max(1,len(entries)//12)]:
 path=root/os.fsdecode(bytes.fromhex(entry['path_hex']));st=os.lstat(path)
 assert (entry['mode'],entry['uid'],entry['gid'],entry['size'],entry['mtime_ns'])==(st.st_mode,st.st_uid,st.st_gid,st.st_size,st.st_mtime_ns),(path,entry,st)
ordered=sorted(bytes.fromhex(e['path_hex']) for e in entries)
digest=hashlib.sha256(b'\0'.join(ordered)).hexdigest()
# Use the independent established oracle for the authoritative digest framing.
subprocess.run(['python3','/data1/efs/command-perf/oracle.py'],check=True,stdout=open(out/'production-oracle.json','w'))
(out/'ls-long.txt').write_bytes(run('ls','-l','--human-readable','--sort','mtime','--relative'))
(out/'find-full.txt').write_bytes(run('find','file_000000.txt','--match','exact','-l','--time-style','full','--relative'))
(out/'find-relative.txt').write_bytes(run('find','file_000000.txt','--match','exact','-l','--time-style','relative','--relative'))
size=entries[0]['size']
filtered=json.loads(run('find','file','--match','prefix','--type','file','--min-size',str(size),'--max-size',str(size),'--json'))
assert {e['path_hex'] for e in filtered['entries']}=={e['path_hex'] for e in entries if e['size']==size}
for name in ('ls','find'):(out/(name+'-help.txt')).write_bytes(run(name,'--help'))
(out/'production-options.json').write_text(json.dumps(dict(status='PASS',paths=len(entries),metadata_samples=len(entries[::max(1,len(entries)//12)]),samples=rows),indent=2))
print('production options PASS: 25,725 paths, folded metadata samples, dates, sort, type/size filters and help',flush=True)
