import importlib.util,json,time,statistics,pathlib
root=pathlib.Path('/data1/efs/command-perf/round7')
def module(name,path):
 spec=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m
before=module('before',root/'command-before.py')
after=module('after','/data1/efs/command-build/src/client/commands/command.py')
result=json.loads((root/'wrapper-before.json').read_text());expected=before.validate_result(result,'find')
assert after.validate_result(result,'find')==expected
rows=[]
for i in range(12):
 for name,m in ([('before',before),('after',after)] if i%2==0 else [('after',after),('before',before)]):
  start=time.perf_counter();paths=m.validate_result(result,'find');elapsed=time.perf_counter()-start
  assert paths==expected
  rows.append(dict(implementation=name,seconds=elapsed,entries=len(paths)))
summary={name:statistics.median(r['seconds'] for r in rows if r['implementation']==name) for name in ('before','after')}
(root/'validation-benchmark.json').write_text(json.dumps(dict(medians=summary,samples=rows),indent=2))
print(json.dumps(summary,indent=2))
