"""The pre-fix real helper must fail the immediate retry timestamp contract."""
import os,runpy,subprocess,tempfile
from pathlib import Path
base=Path(__file__).resolve().parent;root=base.parents[2]
context=runpy.run_path(str(root/'tests/test_applied_interval.py'))
code=context['code'];current=context['fn']('persist_applied')
code=code[:code.index('int main(void)')]+r'''
int main(void){
 uint64_t us;assert(host_applied_interval(NULL,&us)==EFS_OK);
 struct efs_raft_host h={0};h.applied_persist_us=us;h.g[0]=(struct group){1,&h,10,100000,3,0};
 fail_sync=1;
 assert(persist_applied(&h,0,11,200000,0)==EFS_ERR_IO);
 assert(h.g[0].applied_saved==10);
 assert(h.g[0].applied_saved_us==100000); /* old code incorrectly updates this */
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='efs-retry-negative-') as d:
 for name,source,expected in [('corrected',code,0),('baseline',code.replace(current,(base/'baseline-persist-applied.c').read_text().strip()),None)]:
  p=Path(d)/(name+'.c');p.write_text(source);out=p.with_suffix('')
  subprocess.run([os.environ.get('CC','cc'),'-std=c99','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(p),'-o',str(out)],check=True)
  result=subprocess.run([str(out)],capture_output=True,text=True)
  if expected==0:assert result.returncode==0,result.stderr
  else:assert result.returncode!=0 and 'applied_saved_us' in result.stderr,result.stderr
  print(name,'exit',result.returncode,result.stderr.strip())
print('negative control: original updates failed-attempt timestamp; corrected preserves immediate retry PASS')
