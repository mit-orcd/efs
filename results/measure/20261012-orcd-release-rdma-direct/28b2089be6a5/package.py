from pathlib import Path
import subprocess,json,hashlib,tarfile,io,posixpath,re,tempfile
repo='/Users/mike/Documents/Codex/2026-10-08/efs/work/devel-orcd';rev=subprocess.check_output(['git','-C',repo,'rev-parse','release-v0.3.0-pre-alpha'],text=True).strip();assert rev=='28b2089be6a5df835cc7bb46b9c2a9e2f10df190'
entries={}
for row in subprocess.check_output(['git','-C',repo,'ls-tree','-r',rev],text=True).splitlines():
 mode,kind,oid,name=row.split(None,3);assert kind=='blob' and mode in ('100644','100755');entries[name]=(mode,oid)
def blobs(names):
 p=subprocess.run(['git','-C',repo,'cat-file','--batch'],input=''.join(entries[n][1]+'\n' for n in names).encode(),stdout=subprocess.PIPE,check=True);b=p.stdout;k=0;out={}
 for n in names:
  e=b.index(b'\n',k);h=b[k:e].split();assert h[1]==b'blob';s=int(h[2]);out[n]=b[e+1:e+1+s];k=e+s+2
 assert k==len(b);return out
md=[n for n in entries if n=='README.md' or n.startswith('docs/') and n.endswith('.md')];text=blobs(md);cited=set()
for n,b in text.items():
 for x in re.findall(r'\[[^\]]*\]\(([^)]+)\)',b.decode(errors='replace')):
  f=x.split('#',1)[0]
  if f and not re.match(r'^[a-zA-Z][a-zA-Z0-9+.-]*:',f):cited.add(posixpath.normpath(posixpath.join(posixpath.dirname(n),f)))
names=sorted(n for n in entries if not n.startswith(('logs/','.cursor/')) and (not n.startswith('results/') or n.endswith('.md') or n in cited));contents=blobs(names);dirs=sorted(subprocess.check_output(['git','-C',repo,'ls-tree','-r','-d','--name-only',rev],text=True).splitlines())
p=Path('operations/orcd-release-28b2089b');a=p/'source.tar.gz'
with tarfile.open(a,'w:gz') as t:
 for n in names:
  b=contents[n];i=tarfile.TarInfo(n);i.size=len(b);i.mode=int(entries[n][0][-3:],8);i.mtime=int(subprocess.check_output(['git','-C',repo,'show','-s','--format=%ct',rev],text=True));t.addfile(i,io.BytesIO(b))
r={n:hashlib.sha256(contents[n]).hexdigest() for n in names};(p/'inputs.json').write_text(json.dumps(r,indent=2)+'\n');(p/'dirs.json').write_text(json.dumps(dirs,indent=2)+'\n')
meta=dict(source=rev,branch='release-v0.3.0-pre-alpha',inputs=len(r),dirs=len(dirs),sha256={n:hashlib.sha256((p/n).read_bytes()).hexdigest() for n in ['source.tar.gz','inputs.json','dirs.json']});(p/'bundle.json').write_text(json.dumps(meta,indent=2)+'\n')
with tempfile.TemporaryDirectory(prefix='efs-release-orcd-docs-') as d:
 with tarfile.open(a) as t:t.extractall(d)
 for n in dirs:Path(d,n).mkdir(parents=True,exist_ok=True)
 q=subprocess.run(['python3','docs/check-architecture.py'],cwd=d,capture_output=True,text=True);(p/'local-doc-gate.log').write_text(q.stdout+q.stderr);assert q.returncode==0,q.stdout+q.stderr
print(json.dumps(meta));print('actualGit blob manifest and localdocumentation PASS')
