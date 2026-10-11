#!/usr/bin/env python3
"""Exercise all six actual CLI/bootstrap parser wrappers without networking."""
from pathlib import Path
import argparse
import os
import re
import shlex
import subprocess
import tempfile
root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--revision', help='Read wrappers from a historical Git revision')
parser.add_argument('--sanitize', action='store_true')
args = parser.parse_args()
files = [('src/client/efs_fuse.c','parse_addr'),('src/client/node_cache.c','parse_host_port'),
         ('src/mgmt/efs_mgmt.c','parse_host_port'),('src/query/efs_query.c','parse_addr'),
         ('src/server/efsd.c','parse_host_port'),('src/client/efs_bench.c','parse_host_port')]
source = '#include <assert.h>\n#include <stdlib.h>\n#include <stdio.h>\n#include "src/common/parse_host_port.h"\n'
for i, (filename, name) in enumerate(files):
    text = (subprocess.check_output(['git','show',args.revision+':'+filename],cwd=root,text=True)
            if args.revision else (root/filename).read_text())
    match = re.search(r'^static int '+name+r'\([^;]+?\)\n\{',text,re.M)
    if not match:
        raise RuntimeError(filename)
    body = text[match.start():text.index('\n}',match.end())+2]
    source += body.replace(name,'parse_'+str(i),1)+'\n'
source += r'''
static void check(int (*parse)(const char *,char *,size_t,uint16_t *)) {
    struct { const char *input,*host; unsigned port; } good[] = {
        {"10.0.0.203:7443","10.0.0.203",7443}, {"localhost:1","localhost",1},
        {"host-name:65535","host-name",65535}, {"name:0000080","name",80},
        {"::1:7443","::1",7443}, {"[::1]:7443","[::1]",7443}
    };
    for(unsigned i=0;i<sizeof(good)/sizeof(good[0]);i++) {
        char host[64];uint16_t port=0;
        assert(!parse(good[i].input,host,sizeof(host),&port));
        assert(!strcmp(host,good[i].host) && port==good[i].port);
    }
    const char *bad[]={"", "host", ":7443", "host:", "host:0", "host:000", "host:-1",
        "host:+1", "host:65536", "host:131071", "host:12junk", "host:0x50",
        "host: 80", "host:80 ", "host:80\n", "host:99999999999999999999999999999999999", "host:\xff"};
    for(unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);i++) {
        char host[64]="unchanged";uint16_t port=17;
        assert(parse(bad[i],host,sizeof(host),&port)==-1);
        assert(!strcmp(host,"unchanged") && port==17);
    }
    char host[5]="keep";uint16_t port=17;
    assert(!parse("abcd:80",host,sizeof(host),&port) && !strcmp(host,"abcd") && port==80);
    strcpy(host,"keep");port=17;
    assert(parse("abcde:80",host,sizeof(host),&port)==-1);
    assert(!strcmp(host,"keep") && port==17);
    assert(parse("a:80",host,0,&port)==-1);
}
int main(void) {
    int (*parsers[])(const char *,char *,size_t,uint16_t *)={parse_0,parse_1,parse_2,parse_3,parse_4,parse_5};
    for(unsigned i=0;i<sizeof(parsers)/sizeof(parsers[0]);i++) check(parsers[i]);
    char host[8]="keep";uint16_t port=17;
    assert(efs_parse_host_port(NULL,host,sizeof(host),&port)==-1);
    assert(efs_parse_host_port("a:80",NULL,8,&port)==-1);
    assert(efs_parse_host_port("a:80",host,8,NULL)==-1);
    for(unsigned value=1;value<=UINT16_MAX;value++) {
        char input[32];snprintf(input,sizeof(input),"host:%u",value);
        assert(!efs_parse_host_port(input,host,sizeof(host),&port) && port==value);
    }
    puts("address parsing: six actual wrappers, complete decimal port/range, host bounds and unchanged failure outputs PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-addr-test-') as tmp:
    c=Path(tmp)/'test.c';exe=Path(tmp)/'test';c.write_text(source)
    subprocess.run(shlex.split(os.environ.get('CC','cc'))+
                   ['-std=c99','-Wall','-Wextra','-Werror','-I'+str(root),
                    *(['-fsanitize=address,undefined'] if args.sanitize else []),
                    str(c),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
