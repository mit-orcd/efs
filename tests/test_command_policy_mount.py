#!/usr/bin/env python3
"""Exercise the actual mount opt-in gate for partial durable feature masks."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile
root = Path(__file__).resolve().parents[1]
source = (root / 'src/client/efs_fuse.c').read_text()
begin = source.index('static int commands_enabled(void)')
end = source.index('\n}\n', begin) + 3
code = '''
#include "efs/common.h"
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
static uint32_t mask;
static uint32_t features_current(void){return mask;}
''' + source[begin:end] + '''
int main(void) {
 unsetenv("EFS_COMMANDS");mask=3;assert(!commands_enabled());
 setenv("EFS_COMMANDS","0",1);assert(!commands_enabled());
 setenv("EFS_COMMANDS","1",1);
 for(mask=0;mask<8;mask++)assert(!!commands_enabled()==!!(mask&3));
 setenv("EFS_COMMANDS","true",1);assert(!commands_enabled());
 puts("command mount: explicit opt-in, disabled and partial feature masks PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-command-policy-') as directory:
    path = Path(directory) / 'test.c'
    path.write_text(code)
    executable = path.with_suffix('')
    subprocess.run(shlex.split(os.environ.get('CC', 'cc')) + ['-std=gnu11', '-Wall', '-Wextra',
                   '-Werror', '-I' + str(root / 'include'), str(path), '-o', str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
