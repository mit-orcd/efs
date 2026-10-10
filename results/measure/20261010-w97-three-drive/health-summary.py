"""Read timestamped nvme-cli SMART JSON; NVMe temperature fields are Kelvin."""
import json,re
from pathlib import Path
base=Path(__file__).resolve().parent;p=base/'nvme-health.log'
if not p.exists():raise SystemExit('No root health telemetry')
text=p.read_text();rows=[];decoder=json.JSONDecoder()
for match in re.finditer(r'(?m)^(\d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZ)\n',text):
 try:value,end=decoder.raw_decode(text[match.end():].lstrip())
 except ValueError:continue
 rows.append(dict(utc=match[1],smart=value))
assert rows
idctrl=json.loads((base/'nvme-id-ctrl.json').read_text());keys=('critical_warning','media_errors','num_err_log_entries','warning_temp_time','critical_comp_time','thm_temp1_trans_count','thm_temp2_trans_count','thm_temp1_total_time','thm_temp2_total_time','data_units_written','controller_busy_time')
summary=dict(samples=len(rows),first_utc=rows[0]['utc'],last_utc=rows[-1]['utc'],temperatures_C={k:dict(min=min(r['smart'][k]-273.15 for r in rows),max=max(r['smart'][k]-273.15 for r in rows)) for k in ('temperature','temperature_sensor_1','temperature_sensor_2')},counters={k:dict(first=rows[0]['smart'][k],last=rows[-1]['smart'][k],delta=rows[-1]['smart'][k]-rows[0]['smart'][k]) for k in keys},identity={k:idctrl.get(k) for k in ('mn','fr','wctemp','cctemp','hctma','mntmt','mxtmt','vwc','apsta')},limit='Collection starts after NVMe slowdown begins; no cache-occupancy or NAND write amplification measurement. Zero reported transitions is not an exhaustive vendor-specific throttle oracle.')
(base/'health-samples.jsonl').write_text('\n'.join(json.dumps(r) for r in rows)+'\n');(base/'health-summary.json').write_text(json.dumps(summary,indent=2)+'\n');print(json.dumps(summary,indent=2))
