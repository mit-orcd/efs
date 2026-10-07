#!/usr/bin/env python3
"""WITHHOLD must filter the outgoing records, never send a fabricated failure."""
from pathlib import Path
import os,shlex,subprocess,tempfile
root=Path(__file__).resolve().parents[1]
w=(root/'src/client/write.c').read_text()
a=w.index('#if defined(EFS_FAULTS) && EFS_FAULTS\n#ifndef EFS_FAULT_FILE')
# Exclude retry/deadline helpers unrelated to the fault-injection test.
b=w.index('static int report_retry_pause(',a)
source=r'''
#include "efs/protocol.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
struct {efs_export_id_t export_id;} g_client;
static unsigned calls,count;static int rpc_error;
static struct efs_chunk_rec sent[3];
static int efs_client_rpc_report_dirty_raft(efs_export_id_t id,
    const struct efs_chunk_rec *r,uint32_t n,const struct efs_ino_size_rec *in,
    uint32_t ni,int sync) {
    (void)id;(void)in;(void)ni;(void)sync;calls++;count=n;
    for(unsigned i=0;i<n;i++)sent[i]=r[i];
    return rpc_error;
}
'''+w[a:b]+r'''
int main(void) {
    struct efs_chunk_rec r[3]={{.ino=42,.chunk_index=0},
       {.ino=42,.chunk_index=1},{.ino=43,.chunk_index=0}};
    assert(!setenv("EFS_FAULT_WITHHOLD","42:0",1));
#if EFS_FAULTS
    assert(report_send_records(r,3,NULL,0,1)==EFS_ERR_STALE);
    assert(calls==1&&count==2&&sent[0].chunk_index==1&&sent[1].ino==43);
    assert(r[0].ino==42&&r[0].chunk_index==0); /* original snapshot survives */
    assert(report_send_records(r,1,NULL,0,1)==EFS_ERR_STALE&&calls==1);
    rpc_error=EFS_ERR_NET;
    assert(report_send_records(r,3,NULL,0,1)==EFS_ERR_NET&&count==2);
    rpc_error=0;
    FILE *f=fopen(EFS_FAULT_FILE,"w");assert(f);fputs("OFF\n",f);fclose(f);
    assert(report_send_records(r,3,NULL,0,1)==EFS_OK&&count==3);
    f=fopen(EFS_FAULT_FILE,"w");assert(f);fputs("WITHHOLD 42:1\n",f);fclose(f);
    assert(report_send_records(r,3,NULL,0,1)==EFS_ERR_STALE&&count==2);
    assert(sent[0].chunk_index==0&&sent[1].ino==43);
    f=fopen(EFS_FAULT_FILE,"w");assert(f);fputs("WITHHOLD 42:1garbage\n",f);fclose(f);
    assert(report_send_records(r,3,NULL,0,1)==EFS_OK&&count==3);
    remove(EFS_FAULT_FILE);
    assert(!unsetenv("EFS_FAULT_WITHHOLD"));
    assert(report_send_records(r,3,NULL,0,1)==EFS_OK&&count==3);
#else
    assert(report_send_records(r,3,NULL,0,1)==EFS_OK&&count==3);
#endif
    puts("WITHHOLD: outgoing omission, retained snapshot, runtime disable and production exclusion PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-wb-fault-') as directory:
    p=Path(directory)/'test.c';p.write_text(source)
    for enabled in (0,1):
        command=shlex.split(os.environ.get('CC','cc'))+['-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror','-I'+str(root/'include'),'-DEFS_FAULTS='+str(enabled),'-DEFS_FAULT_FILE="'+str(Path(directory)/'fault')+'"']
        command+=shlex.split(os.environ.get('WB_RUNTIME_CFLAGS',''))
        subprocess.run(command+[str(p),'-o',str(p.with_suffix(''))],check=True)
        subprocess.run([str(p.with_suffix(''))],check=True)
