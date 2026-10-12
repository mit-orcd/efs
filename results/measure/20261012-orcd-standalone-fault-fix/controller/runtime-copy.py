from pathlib import Path
s=Path('operations/orcd-valgrind-fix/runtime.py').read_text().replace("Path('operations/orcd-valgrind-fix')","Path('operations/orcd-fault-fix')")
s=s.replace("'EFS_ADMISSION_PRESSURE']","'EFS_ADMISSION_PRESSURE','EFS_REPAIR_EXPERIMENT','EFS_REPAIR_INIT_STORE']")
exec(compile(s,'runtime-copy.py','exec'))
