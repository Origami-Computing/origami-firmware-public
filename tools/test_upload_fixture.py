"""Host contract for the development enqueue adapter; no device mutation."""
from pathlib import Path
import subprocess,tempfile
ROOT=Path(__file__).resolve().parents[1]
s=(ROOT/'src/tooling/dev_serial_input.cpp').read_text()
a=s.index('  if (strncmp(line, "queue enqueue ", 14) == 0)');b=s.index('  if (strcmp(line, "queue clear")',a)
code=r'''
#include <cassert>
#include <cstdio>
#include <cstring>
struct {void println(const char*){}template<class... T>void printf(const char*,T...){}}Serial;
int enqueued=0;bool initialized=true;
namespace services {namespace uploads {
 bool ensureInit(){return initialized;}
 struct Q {bool enqueue(const char* type,const char* path,const char* mime,bool owned){
  assert(!owned);assert(!strncmp(path,!strcmp(type,"video")?"/videos/test-":"/upload_queue/test-",!strcmp(type,"video")?13:19));
  assert(!strcmp(mime,!strcmp(type,"image")?"image/jpeg":!strcmp(type,"video")?"video/x-msvideo":"audio/wav"));++enqueued;return true;
 }}q;Q& queue(){return q;}
}}
void command(const char* line){
'''+s[a:b]+r'''
}
int main(){
 for(const char* invalid:{"queue enqueue image /photos/personal.jpg","queue enqueue audio /upload_queue/test-../private.wav","queue enqueue text /upload_queue/test-a.txt","queue enqueue audio /upload_queue/test-a.wav trailing","queue enqueue video /videos/VID_user.avi","queue enqueue video /videos/test-../user.avi","queue enqueue video /upload_queue/test-v.avi","queue enqueue"}){
  command(invalid);assert(enqueued==0);
 }
 command("queue enqueue image /upload_queue/test-a.jpg");assert(enqueued==1);
 command("queue enqueue audio /upload_queue/test-a.wav");assert(enqueued==2);
 command("queue enqueue video /videos/test-av.avi");assert(enqueued==3);
 initialized=false;command("queue enqueue audio /upload_queue/test-b.wav");assert(enqueued==3);
 puts("PASS development fixture delegates to production queue and rejects unrelated paths/types");
}
'''
code='#include <initializer_list>\n'+code
with tempfile.TemporaryDirectory() as d:
 p=Path(d);(p/'test.cpp').write_text(code)
 subprocess.run(['c++','-std=c++17','-fsanitize=address,undefined',str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
