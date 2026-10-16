import pathlib,time,json,sys,os,hashlib
pid=int(sys.argv[1]);out=pathlib.Path(sys.argv[2]);p=pathlib.Path('/proc')/str(pid)
env=dict(x.decode().split('=',1) for x in (p/'environ').read_bytes().split(b'\0') if b'=' in x)
meta=dict(pid=pid,exe=os.readlink(p/'exe'),sha256=hashlib.sha256((p/'exe').read_bytes()).hexdigest(),env={k:v for k,v in env.items() if k.startswith('EFS_')},args=(p/'cmdline').read_bytes().replace(b'\0',b' ').decode(),verbs=[os.readlink(x) for x in (p/'fd').iterdir() if 'uverbs' in os.readlink(x)],hz=os.sysconf('SC_CLK_TCK'))
assert meta['verbs'] and env['EFS_TRANSPORT']=='rdma'
assert env['EFS_COMMANDS_SERVER' if 'efsd' in meta['exe'] else 'EFS_COMMANDS']=='1'
out.with_suffix('.identity.json').write_text(json.dumps(meta,indent=2))
with out.open('w') as f:
 for n in range(240):
  try:
   st=(p/'stat').read_text().rsplit(')',1)[1].split();io=(p/'io').read_text()
   f.write(json.dumps(dict(time=time.time(),ticks=int(st[11])+int(st[12]),rss_pages=int(st[21]),io=io))+'\n');f.flush()
  except FileNotFoundError:break
  time.sleep(1)
