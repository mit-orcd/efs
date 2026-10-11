#!/usr/bin/env python3
"""Compare actual status identities without confusing build time with code."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/mgmt/efs_mgmt.c').read_text();a=s.index('static void status_version_identity(');b=s.index('\n}',a)+2
code='#include <stdio.h>\n#include <string.h>\n#include <assert.h>\n'+s[a:b]+r'''
int main(void){char a[256],b[256];
 status_version_identity("efsd v1-g123 (branch devel, built today on n1)",a,sizeof(a));
 status_version_identity("efsd v1-g123 (branch devel, built tomorrow on n2)",b,sizeof(b));
 assert(!strcmp(a,b)&&!strcmp(a,"efsd v1-g123 (branch devel)"));
 status_version_identity("efsd v1-g456 (branch devel, built today on n1)",b,sizeof(b));assert(strcmp(a,b));
 status_version_identity("efsd v1-g123 (branch other, built today on n1)",b,sizeof(b));assert(strcmp(a,b));
 status_version_identity("efsd v1-g123-dirty (branch devel, built today on n1)",b,sizeof(b));assert(strcmp(a,b));
 const char *unknown="efsd (no git metadata, branch devel, built today on n1)";
 status_version_identity(unknown,a,sizeof(a));assert(!strcmp(a,unknown));
 char tiny[4];status_version_identity("abcdef",tiny,sizeof(tiny));assert(!strcmp(tiny,"abc"));return 0;
}
'''
with tempfile.TemporaryDirectory() as directory:
 p=Path(directory)/'test.c';p.write_text(code)
 subprocess.run(['cc','-std=c99','-Wall','-Wextra','-Werror',str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True)
print('status identity: timestamps/hosts ignored; commit, branch, dirty and unknown preserved PASS')
