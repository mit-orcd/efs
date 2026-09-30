set pagination off
set confirm off
set print pretty off
set print elements 8
printf "=== group slot 0\n"
p g_host->g[0].group
p g_host->g[0].hosted
p *g_host->g[0].r
printf "=== group slot 1\n"
p g_host->g[1].group
p g_host->g[1].hosted
p *g_host->g[1].r
detach
quit
