#!/usr/bin/env python3
"""Sanitizer regressions against the vendored 32-bit Lua runtime."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1];src=root/'lib/lua/src'
code=r'''
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include <stdio.h>
int main(void){
 lua_State *L=luaL_newstate();
 luaL_requiref(L,"_G",luaopen_base,1);lua_pop(L,1);
 luaL_requiref(L,"string",luaopen_string,1);lua_pop(L,1);
 luaL_requiref(L,"utf8",luaopen_utf8,1);lua_pop(L,1);
 luaL_requiref(L,"math",luaopen_math,1);lua_pop(L,1);
 const char *test=
 "assert(not utf8.len(string.char(255,143,143,143,143,143,143,143)))\n"
 "for i=1,1000 do local parent={};parent.__newindex=parent;collectgarbage();local child=setmetatable({},parent);child.__newindex={x='hello'};collectgarbage('step');assert(parent.__newindex.x=='hello') end\n"
 "local iter=string.gmatch(string.rep('a',2000),string.rep('a?',2000));pcall(iter);pcall(iter)\n"
 "collectgarbage('step',math.maxinteger);collectgarbage('step',-math.maxinteger)\n"
 "for i=1,1000 do local t=setmetatable({},{__mode='kv'});collectgarbage('step');setmetatable(t,{__mode='kv'});collectgarbage();assert(getmetatable(t).__mode=='kv') end\n";
 int status=luaL_dostring(L,test);if(status)fprintf(stderr,"%s\n",lua_tostring(L,-1));
 lua_close(L);return status;
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d);(p/'test.c').write_text(code)
 subprocess.run(['cc','-std=c99','-O1','-g','-fsanitize=address,undefined','-fno-sanitize-recover=all','-I'+str(src),*[str(f) for f in src.glob('*.c')],str(p/'test.c'),'-lm','-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True,timeout=45)
print('PASS vendored Lua GC, UTF8 and pattern-error regressions under ASan/UBSan')
