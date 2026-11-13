"""Compile actual automation.cpp and production JSON/policy headers on the host."""
from pathlib import Path
import subprocess

root=Path(__file__).resolve().parents[1]
out=root/'.cache/recipe-alert-tests'
out.mkdir(parents=True,exist_ok=True)
stubs={
 'Arduino.h': '''#pragma once
#include <stdint.h>
#include <string.h>
inline uint32_t millis(){return 1000;}
inline size_t strlcpy(char* d,const char* s,size_t n){size_t l=strlen(s);if(n){size_t k=l<n-1?l:n-1;memcpy(d,s,k);d[k]=0;}return l;}
struct SerialStub{void println(const char*){}template<class... T>void printf(const char*,T...){}}; inline SerialStub Serial;
''',
 'Pins.h':'#pragma once\n',
 'hal/hal_io.h':'#pragma once\n',
 'core/board_time.h':'#pragma once\nnamespace core {namespace time {inline bool isSynced(){return true;}}}\n',
 'services/morning_quote.h':'#pragma once\n#include <time.h>\nnamespace services {namespace morning_quote {inline void requestForEpoch(time_t){}}}\n',
}
for name,content in stubs.items():
 p=out/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_text(content)
arduino=root/'.pio/libdeps/esp32-s3-menu-n16r8-production/ArduinoJson/src'
subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-I'+str(out),'-I'+str(root/'include'),'-I'+str(arduino),
 str(root/'tests/recipe_alerts_test.cpp'),str(root/'src/services/automation.cpp'),'-o',str(out/'test')],check=True)
result=subprocess.run([str(out/'test')],check=True,capture_output=True,text=True)
(out/'results.txt').write_text(result.stdout);print(result.stdout,end='')
