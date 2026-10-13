#!/usr/bin/env python3
'One-shot Linux /proc CPU/RSS/I/O sample for the current user EFS processes.\nRun on each host at a recorded interval; keep JSONL output outside Git.\nCPU ticks are cumulative and need elapsed-time differencing. VmRSS includes\nauthoritative metadata and other allocations, not just the query cache.'
import pathlib,json,os,time,subprocess
rows=[]
for p in pathlib.Path('/proc').iterdir():
 if not p.name.isdigit():continue
 try:
  name=(p/'comm').read_text().strip()
  if name not in ('efsd','efs-fuse') or p.stat().st_uid!=os.getuid():continue
  status={k:v.strip() for k,v in (line.split(':',1) for line in (p/'status').read_text().splitlines() if ':' in line)}
  stat=(p/'stat').read_text().rsplit(')',1)[1].split()
  rows.append(dict(pid=int(p.name),name=name,status={k:status.get(k) for k in ['VmRSS','VmHWM','VmSize','Threads']},cpu_ticks_user=int(stat[11]),cpu_ticks_system=int(stat[12]),clock_ticks=os.sysconf('SC_CLK_TCK'),io=(p/'io').read_text(),fd_count=len(list((p/'fd').iterdir()))))
 except (FileNotFoundError,PermissionError,ProcessLookupError):pass
print(json.dumps(dict(time_ns=time.time_ns(),processes=rows,meminfo=pathlib.Path('/proc/meminfo').read_text(),load=pathlib.Path('/proc/loadavg').read_text(),diskstats=pathlib.Path('/proc/diskstats').read_text())))
