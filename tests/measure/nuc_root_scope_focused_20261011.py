#!/usr/bin/env python3
"""Run isolated production PUT regression after the final timed placement cell."""
import hashlib,json,resource,subprocess,tarfile,time,traceback
from pathlib import Path

base=Path('/data1/efs/devel-nuc-20261011')
out=base/'root-scope-focused';out.mkdir(exist_ok=False)
src=base/'root-scope-focused-src';old=base/'root-scope-old-src'
try:
    deadline=time.monotonic()+3600
    while not (base/'data-placement/2-sata/complete.json').exists():
        assert not (base/'data-placement/failure.txt').exists(),'placement failed'
        assert time.monotonic()<deadline,'final cell wait exceeded'
        time.sleep(1)
    archive=base/'efs-root-scope-766814c8.tar'
    assert hashlib.sha256(archive.read_bytes()).hexdigest()=='5a93449aee83416c535b3d622c5f7f91d0aa8e156a05d9fabe27af5466943c5b'
    prior=base/'efs-root-scope-old-store.c'
    assert hashlib.sha256(prior.read_bytes()).hexdigest()=='ed43832a3f8e03c19ddb4d2aee6bd3cdbd47b4a124bb29f9355667e1dbf77274'
    src.mkdir();old.mkdir()
    for target in (src,old):
        with tarfile.open(archive) as tar:
            assert set(tar.getnames())=={'src/server/store.c','src/server/packed_fence.h','tests/test_store_durability.c'}
            tar.extractall(target,filter='data')
    (old/'src/server/store.c').write_bytes(prior.read_bytes())
    deps=base/'batch-src'
    (old/'src/server/packed_fence.h').write_bytes((deps/'src/server/packed_fence.h').read_bytes())
    resource.setrlimit(resource.RLIMIT_CORE,(0,0))
    codes={}
    for name,target in (('fixed',src),('old-scope',old)):
        command=['gcc','-O2','-g','-D_GNU_SOURCE','-std=c99','-Wall','-Wextra',
            '-ffunction-sections','-fdata-sections','-Wl,--gc-sections']
        for folder in ('include','src/common','src/server','deps/blake3'):
            command+=['-I'+str(deps/folder)]
        command += ['-o',str(target/'test'),str(target/'tests/test_store_durability.c'),
            str(deps/'src/bench/writer.o'),str(deps/'src/bench/store_containers.o'),
            str(deps/'libefs.a'),'-lpthread','-lm','-ldl','-libverbs']
        (out/(name+'-command.json')).write_text(json.dumps(command,indent=2))
        with (out/(name+'-compile.log')).open('w') as log:
            rc=subprocess.run(command,stdout=log,stderr=subprocess.STDOUT,timeout=120).returncode
        (out/(name+'-compile-exit.txt')).write_text(str(rc)+'\n');assert rc==0
        with (out/(name+'.log')).open('w') as log:
            codes[name]=subprocess.run([str(target/'test')],stdout=log,stderr=subprocess.STDOUT,timeout=120).returncode
        (out/(name+'-exit.txt')).write_text(str(codes[name])+'\n')
    assert codes=={'fixed':0,'old-scope':-6},codes
    provenance=dict(candidate='766814c8',root_scope_core='19cbb11f',codes=codes,
        sources={str(p.relative_to(src)):hashlib.sha256(p.read_bytes()).hexdigest()
            for p in src.rglob('*') if p.suffix in ('.c','.h')},
        dependencies={str(p.relative_to(deps)):hashlib.sha256(p.read_bytes()).hexdigest()
            for p in (deps/'libefs.a',deps/'src/bench/writer.o',deps/'src/bench/store_containers.o')},
        limits='Focused Linux production shard-I/O regression after timed placement. Not full native/live acceptance or measured speedup; no installed images changed.')
    (out/'provenance.json').write_text(json.dumps(provenance,indent=2)+'\n')
    (out/'run-exit.txt').write_text('0\n')
except BaseException:
    (out/'failure.txt').write_text(traceback.format_exc());raise
