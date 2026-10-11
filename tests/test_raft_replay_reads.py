#!/usr/bin/env python3
"""Real durable replay: bounded reads, boundary payloads and nondestructive EIO."""
from pathlib import Path
import hashlib,os,re,struct,subprocess,sys,tempfile,zlib
if sys.platform!='linux':
 print('Raft replay pread fault injection requires Linux: SKIP');raise SystemExit(0)
root=Path(__file__).resolve().parents[1]
driver=r'''
#include <efs/raft_disk.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(int argc,char **argv){assert(argc==3);struct efs_raft_disk *d=efs_raft_disk_open(argv[1],EFS_RAFT_DISK_SYNC);
 if(!strcmp(argv[2],"fail")){assert(!d);return 0;}assert(d);
 for(unsigned i=1;i<=1024;i++){unsigned group=i%2?0:2;uint8_t buf[2048];uint32_t len=sizeof(buf);uint64_t term=0;
  struct efs_raft_store *s=efs_raft_disk_group(d,group);assert(s);
  assert(s->get(s,(i+1)/2,&term,buf,&len)==0&&term==7&&len==(i%2048)+1);
  for(unsigned j=0;j<len;j++)assert(buf[j]==(uint8_t)i);
 }efs_raft_disk_close(d);return 0;}
'''
fault=r'''
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>
static unsigned calls;static int interrupted;
ssize_t pread(int fd,void *buf,size_t n,off_t off){
 char path[64],target[512];snprintf(path,sizeof(path),"/proc/self/fd/%d",fd);
 ssize_t len=readlink(path,target,sizeof(target)-1);int islog=0;
 if(len>0){target[len]=0;islog=__builtin_strstr(target,"/raft.log")!=NULL;}
 if(islog){calls++;const char *fail=getenv("EFS_TEST_REPLAY_FAIL");if(fail&&calls==(unsigned)strtoul(fail,NULL,10)){errno=EIO;return -1;}
  if(getenv("EFS_TEST_REPLAY_EINTR")&&!interrupted){interrupted=1;errno=EINTR;return -1;}
  if(getenv("EFS_TEST_REPLAY_SHORT")&&n>137)n=137;
 }
 ssize_t (*real)(int,void*,size_t,off_t)=dlsym(RTLD_NEXT,"pread");return real(fd,buf,n,off);
}
__attribute__((destructor)) static void report(void){fprintf(stderr,"replay-preads=%u\n",calls);}
'''
blob=bytearray(struct.pack('>II',0x52464c31,1))
for i in range(1,1025):
 body=struct.pack('>BIQQI',2,0 if i%2 else 2,(i+1)//2,7,(i%2048)+1)+bytes([i&255])*((i%2048)+1)
 blob.extend(struct.pack('>II',zlib.crc32(body),len(body))+body)
with tempfile.TemporaryDirectory(prefix='efs-raft-replay-') as td:
 p=Path(td);(p/'test.c').write_text(driver);(p/'fault.c').write_text(fault)
 subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(p/'test.c'),str(root/'libefs.a'),'-pthread','-lm','-ldl','-o',str(p/'test')],check=True)
 subprocess.run(['cc','-shared','-fPIC','-Wall','-Wextra','-Werror',str(p/'fault.c'),'-ldl','-o',str(p/'fault.so')],check=True)
 for label,extra in [('normal',{}),('eintr',{'EFS_TEST_REPLAY_EINTR':'1'}),('short',{'EFS_TEST_REPLAY_SHORT':'1'}),('io',{'EFS_TEST_REPLAY_FAIL':'2'})]:
  folder=p/label;folder.mkdir();log=folder/'raft.log';log.write_bytes(blob);before=hashlib.sha256(blob).digest()
  r=subprocess.run([str(p/'test'),str(folder),'fail' if label=='io' else 'ok'],env=dict(os.environ,LD_PRELOAD=str(p/'fault.so'),**extra),text=True,capture_output=True)
  assert not r.returncode,(label,r.returncode,r.stdout,r.stderr)
  assert hashlib.sha256(log.read_bytes()).digest()==before,(label,'durable log changed')
  count=int(re.search(r'replay-preads=(\d+)',r.stderr).group(1))
  if label=='normal':assert count<=(len(blob)+65535)//65536+1,count
  if label=='io':assert count==2,count
  print(label+' replay: exact payloads/log preservation, preads='+str(count)+' PASS')
print('Raft replay batching and nondestructive read failure PASS')
