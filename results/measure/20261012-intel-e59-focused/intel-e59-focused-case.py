import importlib.util,sys
from pathlib import Path
source=Path(sys.argv[1]);sys.argv=sys.argv[1:]
spec=importlib.util.spec_from_file_location('efs_original_posix',source);suite=importlib.util.module_from_spec(spec);spec.loader.exec_module(suite)
selected={'basic_write_read','basic_seek_cur_past_eof_write','fsync_reopen_visible','basic_rdwr_no_reopen','basic_append','basic_oappend_flag','basic_chunk_boundary','fsync_write','fsync_then_fstat_size','basic_dup_write','dir_readdir_listing','names_crazy_dirs'}
suite.TESTS[:]=[(n,f) for n,f in suite.TESTS if n in selected]
assert {n for n,f in suite.TESTS}==selected
suite.main()
