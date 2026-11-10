#!/usr/bin/env python3
"""Task FB: instrument a copy of src/bench/io_bench.c for start-gate diagnosis.

Usage (in a throwaway tree copy, then `make efs-bench`):
  io_start_diag.py release-loop TREE   # pre-fix source (2dd280cee): time when main signalled each worker
  io_start_diag.py spread TREE         # any source: percentiles of worker start delays after release
The binary then prints `DIAG ...` lines on stderr. Never apply to a tree you commit from.
"""
import sys
from pathlib import Path
mode,tree=sys.argv[1],Path(sys.argv[2])
p=tree/'src/bench/io_bench.c';s=p.read_text()
def sub(a,b):
    global s
    assert s.count(a)==1,f'{mode}: anchor not found once: {a[:60]!r}'
    s=s.replace(a,b)
anchor="    double wall = (double)(end - start) / 1e9;\n"
if mode=='release-loop':
    sub("    uint64_t start_delay_ns;\n","    uint64_t start_delay_ns, signal_ns;\n")
    sub("        pthread_mutex_lock(&w[i].go_mu);\n        w[i].released = 1;",
        "        w[i].signal_ns = now_ns() - start;\n        pthread_mutex_lock(&w[i].go_mu);\n        w[i].released = 1;")
    sub(anchor,"""    { uint64_t ms=0, gap=0; for (int i=0;i<qd;i++){ if (w[i].signal_ns>ms) ms=w[i].signal_ns;
        if (w[i].start_delay_ns>w[i].signal_ns && w[i].start_delay_ns-w[i].signal_ns>gap) gap=w[i].start_delay_ns-w[i].signal_ns; }
      fprintf(stderr,"DIAG release_loop_ms=%.3f max_signal_to_start_ms=%.3f last_signalled_start_ms=%.3f\\n",ms/1e6,gap/1e6,w[qd-1].start_delay_ns/1e6); }
"""+anchor)
elif mode=='spread':
    sub(anchor,"""    { uint64_t d[256]; for (int i=0;i<qd;i++) d[i]=w[i].start_delay_ns;
      for (int i=1;i<qd;i++){uint64_t v=d[i];int j=i;while(j&&d[j-1]>v){d[j]=d[j-1];j--;}d[j]=v;}
      fprintf(stderr,"DIAG start_ms p0=%.3f p50=%.3f p90=%.3f p99=%.3f max=%.3f\\n",d[0]/1e6,d[qd/2]/1e6,d[qd*9/10]/1e6,d[qd*99/100]/1e6,d[qd-1]/1e6); }
"""+anchor)
else:sys.exit(__doc__)
p.write_text(s)
