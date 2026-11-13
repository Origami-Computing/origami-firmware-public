"""Exercise actual text transport admission state with real ArduinoJson."""
from pathlib import Path
import subprocess, tempfile
root=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory() as directory:
 p=Path(directory);(p/'freertos').mkdir()
 (p/'Arduino.h').write_text(r'''
#pragma once
#include <cstdint>
#include <cstring>
inline uint32_t millis(){return 10;}
inline size_t strlcpy(char* dst,const char* src,size_t n){auto len=strlen(src);if(n){auto count=len<n-1?len:n-1;memcpy(dst,src,count);dst[count]=0;}return len;}
struct SerialType {template<class... T>void printf(const char*,T...) {}};
inline SerialType Serial;
''')
 (p/'freertos/FreeRTOS.h').write_text('#pragma once\n')
 (p/'freertos/portmacro.h').write_text('''#pragma once
using portMUX_TYPE=int;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(p) ((void)(p))
#define portEXIT_CRITICAL(p) ((void)(p))
''')
 (p/'test.cpp').write_text(r'''
#include "core/device_agent_text.h"
#include <cassert>
using namespace core::device_agent_text;
bool send(const char*){return true;}
int main(){
 assert(!hasPendingRequest());
 assert(submit("one","fixture"));assert(hasPendingRequest());
 disconnected();assert(hasPendingRequest());
 ready(true);pump(11,true,false,send);assert(hasPendingRequest());
 JsonDocument message;message["requestId"]="one";message["type"]="message";message["text"]="reply";
 assert(receive(message));assert(hasPendingRequest());
 message["type"]="done";assert(receive(message));assert(!hasPendingRequest());
 Result result;assert(take(result));assert(result.outcome==Outcome::Ready);
 assert(submit("two","fixture"));assert(hasPendingRequest());cancel();assert(!hasPendingRequest());
 assert(submit("three","fixture"));ready(false);pump(11,true,false,send);
 assert(!hasPendingRequest());assert(take(result));assert(result.outcome==Outcome::Unsupported);
 assert(submit("four","fixture"));disconnected();pump(150011,false,false,send);
 assert(!hasPendingRequest());assert(take(result));assert(result.retryable);
}
''')
 subprocess.run(['c++','-std=c++17','-fsanitize=address,undefined','-I',str(p),'-I',str(root/'include'),
  '-I',str(root/'.pio/libdeps/esp32-s3-menu-n16r8-production/ArduinoJson/src'),str(root/'src/core/device_agent_text.cpp'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('PASS actual Agent text admission: queued, disconnected, in-flight, completed, canceled, unsupported and timeout states')
