# perf script -> share of context switches parked in a mutex wait, by the
# first two frames above the pthread lock call.
import sys,collections
tot=0; lock=0; ri=0; by=collections.Counter(); st=[]
# Fresh-read admission/begin callers (LOOKUP pair, GETATTR lanes, scalar).
ri_frames=("server_raft_host_lookup","host_read_index","host_read_inode_lanes_reuse")
libc=("__lll","___pthread","__pthread","pthread_","lll_","futex","__futex","[unknown]")
def flush():
    global tot,lock,ri
    if not st: return
    tot+=1
    w=[i for i,f in enumerate(st) if "lll_lock_wait" in f]
    if w:
        lock+=1
        callers=[f for f in st[w[0]+1:] if not f.startswith(libc)]
        by[" <- ".join(callers[:2]) or "?"]+=1
        if callers and callers[0].startswith(ri_frames): ri+=1
for l in sys.stdin:
    l=l.rstrip()
    if not l: flush(); st=[]; continue
    if not l.startswith(("\t"," ")): continue
    p=l.split()
    if len(p)>1: st.append(p[1].split("+")[0])
flush()
print("  cs switches %d, mutex waits %d (%.1f%%), ReadIndex-caller waits %d (%.1f%%)"%(
    tot,lock,100.0*lock/max(tot,1),ri,100.0*ri/max(tot,1)))
for k,v in by.most_common(10): print("    %5.1f%% of switches  %s"%(100.0*v/max(tot,1),k[:160]))
