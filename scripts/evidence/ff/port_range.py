#!/usr/bin/env python3
"""port_range.py TREE SLOT SCRIPT [ARGS]: run SCRIPT with k33_daemons ports taken
from 18700+12*SLOT .. +11 (intel1 private range 18700-18799)."""
import runpy,socket,sys
from pathlib import Path
tree=Path(sys.argv[1]).resolve();slot=int(sys.argv[2]);script=sys.argv[3]
sys.path.insert(0,str(tree/'tests'))
import k33_daemons as k
lo=18700+12*slot;used=set()
assert 0<=slot<=7
def free_port(host=k.HOST,udp=False):
    for p in range(lo,lo+12):
        if p in used:continue
        with socket.socket(socket.AF_INET,socket.SOCK_DGRAM if udp else socket.SOCK_STREAM) as s:
            try:s.bind((host,p))
            except OSError:continue
        used.add(p);return p
    raise RuntimeError(f'slot {slot}: no free port in {lo}..{lo+11}')
k.free_port=free_port
sys.argv=[script]+sys.argv[4:]
runpy.run_path(script,run_name='__main__')
