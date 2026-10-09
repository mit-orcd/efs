from pathlib import Path
import os,json,time
base=Path('/data1/efs');name='gate-release-f6233987-'+str(os.getpid());a=base/'mnt'/name;b=base/'mnt2'/name
a.mkdir()
try:
 p=a/'f';q=b/'f'
 fd=os.open(p,os.O_WRONLY|os.O_CREAT|os.O_DSYNC,0o600)
 try:
  body=b'f6233987 synchronous ordinary bytes\n';assert os.write(fd,body)==len(body);assert q.read_bytes()==body
 finally:os.close(fd)
 fd=os.open(p,os.O_WRONLY|os.O_APPEND|os.O_DSYNC)
 try:
  tail=b'append before close\n';assert os.write(fd,tail)==len(tail);assert q.read_bytes()==body+tail
 finally:os.close(fd)
 with q.open('wb'):pass
 fd=os.open(p,os.O_WRONLY|os.O_APPEND|os.O_DSYNC)
 try:
  final=b'after peer truncate\n';assert os.write(fd,final)==len(final);assert q.read_bytes()==final
 finally:os.close(fd)
 print(json.dumps({'source_commit':'f6233987b9d1','ordinary_odsync_peer_before_close':True,'append_odsync_peer_before_close':True,'peer_truncate_then_append_exact':True,'test_path':str(a),'time_unix':time.time()},indent=2))
finally:
 (a/'f').unlink(missing_ok=True);a.rmdir()
