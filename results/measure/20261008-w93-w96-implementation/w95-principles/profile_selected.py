import importlib.util,sys
from pathlib import Path
source=Path('/data1/efs/merge-e6926d85-20261008')
spec=importlib.util.spec_from_file_location('bench_profile',source/'scripts/bench_profile.py')
module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
all_cases=module.cases_for
def selected(a,cpus):
    rows=all_cases(a,cpus)
    return [r for r in rows if '-write-allocating-' in r['name'] and ('-each-' in r['name'] or '-batch-' in r['name'])]
module.cases_for=selected
sys.exit(module.main(['--binary',str(source/'efs-bench'),'--storage-principles','--principles-sizes','512','--principles-logical-size','512','--principles-objects','8192','--principles-directory-sync','ancestors','--qds','4','--storage-root','/data1/efs','--skip-ceiling','--event','cpu-clock','--frequency','999','--strace','--strace-expr','trace=pwrite64,fsync,openat,close,fallocate,unlink','--output','/data1/efs/w95-profile-20261008']))
