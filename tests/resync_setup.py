"""Bound an explicit management resync request and its documented BUSY refusal."""
import time

def request_before(request, deadline, *, clock=time.monotonic, sleep=time.sleep):
    attempts=0
    while clock()<deadline:
        rc,out=request();attempts+=1
        if rc==0:
            return attempts
        # Raft refuses without setting resync_requested while recv_persisting.
        # Do not retry NOT_PRIMARY, network failures or arbitrary command errors.
        if rc!=1 or 'rc=-13' not in out.split():
            raise AssertionError(f'raft-resync rc={rc}: {out}')
        sleep(min(0.1,max(0,deadline-clock())))
    raise AssertionError(f'raft-resync remained BUSY through deadline ({attempts} requests)')
