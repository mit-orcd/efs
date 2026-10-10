"""Compare equal first-180-second windows; full 600-second results are separate."""
import json
from pathlib import Path
base=Path(__file__).resolve().parent
rows=[]
for tag in ('10ms','100ms','10ms-return'):
    cell=base/tag
    if not (cell/'complete.json').exists():continue
    samples=[json.loads(x) for x in (cell/'samples.jsonl').read_text().splitlines()]
    segments=[]
    for lo,hi in ((0,60),(60,120),(120,180),(0,180)):
        a=min(samples,key=lambda x:abs(x['seconds']-lo));b=min(samples,key=lambda x:abs(x['seconds']-hi))
        d=[y-x for x,y in zip(a['progress']['counts'],b['progress']['counts'])];elapsed=b['seconds']-a['seconds']
        segments.append(dict(start_s=a['seconds'],end_s=b['seconds'],files_s=sum(d)/elapsed,group0_files_s=sum(d[:4])/elapsed,group2_files_s=sum(d[4:])/elapsed))
    rows.append(dict(tag=tag,windows=segments))
(base/'common-window.json').write_text(json.dumps(rows,indent=2))
for r in rows:print(json.dumps(r))
