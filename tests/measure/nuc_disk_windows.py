"""Physical block-counter windows; includes host I/O and block queue time."""
def disk(raw,name):
    for line in raw.splitlines():
        fields=line.split()
        if fields[2]==name:return list(map(int,fields[3:]))
    raise ValueError('missing physical device '+name)
def windows(hosts,device,last_seconds):
    origin=hosts[0]['monotonic'];result={}
    for label,start,end in [('first',0,30),('late',last_seconds-30,last_seconds)]:
        a=min(hosts,key=lambda h:abs(h['monotonic']-origin-start))
        b=min(hosts,key=lambda h:abs(h['monotonic']-origin-end))
        duration=b['monotonic']-a['monotonic'];assert 20<=duration<=40
        before=disk(a['diskstats'],device);after=disk(b['diskstats'],device)
        delta=[v-u for u,v in zip(before,after)]
        assert delta[4]>=0 and delta[7]>=0
        result[label]=dict(seconds=duration,write_requests=delta[4],write_mean_ms=delta[7]/delta[4] if delta[4] else None,
                           host_write_MiB=delta[6]*512/2**20,
                           flush_requests=delta[15] if len(delta)>15 else None,
                           flush_mean_ms=delta[16]/delta[15] if len(delta)>16 and delta[15] else None)
    return result
