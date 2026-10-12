from pathlib import Path
import sys,subprocess,json,time
root=Path(sys.argv[1]).resolve();out=Path(sys.argv[2]);out.mkdir(exist_ok=True)
s=(root/'tests/test_raft.c').read_text();s='#include "'+str(root/'src/raft/raft.c')+'"\n'+s
s=s.replace('static void test_read_applied_floor(void)','static int floor_context_mode;\nstatic int floor_cfg_eio(void *ctx,uint32_t old,uint32_t next){(void)ctx;(void)old;(void)next;return EFS_ERR_IO;}\nstatic void test_read_applied_floor(void)')
needle='    app[leader].apply_limit=0;elect(&n,2);\n    CHECK(efs_raft_read_proven(n.r[leader],&got,&idx)'
a=s.index(needle,s.index('static void test_read_applied_floor'))
inject='''    CHECK(efs_raft_read_begin(n.r[leader])==EFS_OK,"populate second pending record");elect(&n,2);
    struct efs_raft *core=n.r[leader];
    CHECK(core->read_applied_round && core->read_proven_round,"both real records populated");
    if(floor_context_mode){
        if(floor_context_mode==1){
            app[leader].apply_limit=0;elect(&n,2);
            CHECK(efs_raft_change(core,(1u<<leader)|(1u<<((leader+1)%3)))==EFS_OK,"cfg A to B");elect(&n,8);
            CHECK(!core->read_applied_round && !core->read_proven_round,"cfg B clears both");
            CHECK(efs_raft_change(core,7)==EFS_OK,"cfg B to A");elect(&n,8);
            CHECK(!core->read_applied_round && !core->read_proven_round,"cfg ABA cannot resurrect");
        }else if(floor_context_mode==2){
            uint64_t term=core->current_term;
            CHECK(efs_raft_step_down(core)==EFS_OK && core->current_term==term,"same term stepdown");
            CHECK(!core->read_applied_round && !core->read_proven_round,"same term clears both");
            CHECK(become_leader(core)==EFS_OK,"actual leader lifecycle callback");
            CHECK(!core->read_applied_round && !core->read_proven_round,"leader lifecycle cannot restore old proofs");
        }else if(floor_context_mode==3){
            store[leader]->save_cfg=floor_cfg_eio;
            CHECK(efs_raft_snapshot(core)==EFS_ERR_IO,"real snapshot config persistence EIO");
            CHECK(core->storage_failed && !core->read_applied_round && !core->read_proven_round,"storage failure clears both");
        }else if(floor_context_mode==4){
            struct efs_raft_msg msg={.type=EFS_RAFT_MSG_SNAP_REP,.to=(leader+1)%3};
            n.drop[msg.to]=1; /* Isolate callback state from remote reply delivery. */
            CHECK(snap_installed(core,&msg,first,core->current_term,7,0,1)==EFS_OK,"actual snapshot import finalizer");
            CHECK(!core->read_applied_round && !core->read_proven_round,"import clears both");
        }
        for(int k=0;k<3;k++){efs_raft_free(n.r[k]);efs_raft_mem_free(store[k]);}
        return;
    }
'''
s=s[:a]+inject+s[a:];s=s.replace('    test_read_applied_floor();','    for(floor_context_mode=0;floor_context_mode<=4;floor_context_mode++)test_read_applied_floor();')
p=out/'read-floor-context.c';p.write_text(s)
flags=['-O1','-g','-std=gnu11','-D_GNU_SOURCE','-Wall','-Wextra','-fno-omit-frame-pointer']
if '--san' in sys.argv:flags+=['-fsanitize=address,undefined']
cmd=['gcc',*flags,'-I'+str(root/'include'),'-I'+str(root/'deps/blake3'),str(p),str(root/'libefs.a'),'-lpthread','-lm','-ldl','-libverbs','-o',str(out/'read-floor-context')]
start=time.time();subprocess.run(cmd,check=True);subprocess.run([str(out/'read-floor-context')],check=True)
(out/'context-verdict.json').write_text(json.dumps({'commit':'52bcfd3570b0a59076c225caad9157b0825954d8','scope':'supplemental whitebox actual core: real membership transitions and persistence EIO; direct lifecycle/import callbacks, not distributed import','command':cmd,'exit':0,'duration_seconds':time.time()-start},indent=2))
