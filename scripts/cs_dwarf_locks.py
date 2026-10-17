# perf script (DWARF cs) -> all switches by kind; lock waits split into
# condvar re-acquire vs plain mutex, by the first 3 non-libc callers.
import sys,collections
tot=0; lw=0; cond=0; kinds=collections.Counter(); by=collections.Counter(); st=[]
libc=("__lll","___pthread","__pthread","pthread_","lll_","futex","__futex","__GI_","[unknown]","syscall","__libc","__poll","__GI","epoll","__epoll")
def flush():
    global tot,lw,cond
    if not st: return
    tot+=1
    w=[i for i,f in enumerate(st) if "lll_lock_wait" in f]
    user=[f for f in st if not f.startswith(libc) and not f.startswith(("__schedule","schedule","do_","__x64","entry_","futex_","__futex","x64_sys","ksys","vfs_","__se_","__do","io_schedule","rwsem","rdma_","ib_","mlx","poll_","hrtimer"))]
    if w:
        lw+=1; c=len(st)>w[0]+1 and "__pthread_mutex_cond_lock" in st[w[0]+1]; cond+=c
        callers=[f for f in st[w[0]+1:] if not f.startswith(libc)]
        by[("cond-reacquire " if c else "lock ")+" <- ".join(callers[:3])]+=1
        kinds["mutex wait"]+=1
    elif any("pthread_cond" in f for f in st): kinds["condvar sleep: "+(user[0] if user else "?")]+=1
    else: kinds["other: "+(user[0] if user else "?")]+=1
for l in sys.stdin:
    l=l.rstrip()
    if not l: flush(); st=[]; continue
    if not l.startswith(("\t"," ")): continue
    p=l.split()
    if len(p)>1: st.append(p[1].split("+")[0])
flush()
print("  switches %d; lock waits %d (%.1f%%); cond re-acquire %d (%.1f%% of lock waits)"%(tot,lw,100.0*lw/max(tot,1),cond,100.0*cond/max(lw,1)))
print("  switch kinds:")
for k,v in kinds.most_common(10): print("    %5.1f%%  %s"%(100.0*v/max(tot,1),k[:150]))
print("  lock-wait sites (share of lock waits):")
for k,v in by.most_common(18): print("    %5.1f%%  %s"%(100.0*v/max(lw,1),k[:170]))
