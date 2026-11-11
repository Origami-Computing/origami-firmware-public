"""Real Photos sync/cover policy: watching an upload survives idle return."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1];source=(root/'src/apps/photos_app.cpp').read_text()
def function(name):
 start=source.index(name);brace=source.index('{',start);depth=1;end=brace+1
 while depth:
  depth += (source[end]=='{')-(source[end]=='}');end+=1
 return source[start:end]
with tempfile.TemporaryDirectory() as temp:
 p=Path(temp)
 code=r'''
#include <cassert>
#include <cstring>
#include "core/health_policy.h"
enum {PH_EMPTY,PH_VIEWING,PH_SYNCING};
int s_state=PH_VIEWING,s_count=1,s_idx=0,s_thumbnailIndex=0,closed=0,cleared=0;
bool s_browsePending=false,s_actionsOpen=true;char s_resumePath[96]={};
struct File {const char* path="/videos/fixture.avi";};
struct Library {File file;const File& operator[](int){return file;}void close(){++closed;}} s_library;
struct Thumbnail {void clear(){++cleared;}} s_thumbnail;
size_t strlcpy(char* out,const char* in,size_t n){if(n){std::strncpy(out,in,n-1);out[n-1]=0;}return std::strlen(in);}
'''
 for name in ['static void exitApp()', 'static void coverGallery()', 'static core::health::RuntimePolicy runtimePolicy()']:code+=function(name)+'\n'
 code+=r'''
int main(){
 auto policy=runtimePolicy();assert(policy.allowIdleReturn&&!policy.reportActivity);
 s_browsePending=true;policy=runtimePolicy();assert(policy.allowIdleReturn&&policy.reportActivity);
 s_browsePending=false;s_state=PH_SYNCING;policy=runtimePolicy();
 assert(!policy.allowIdleReturn&&policy.reportActivity&&policy.allowBackgroundUploads&&policy.allowPowerSleep);
 coverGallery();policy=runtimePolicy();assert(policy.allowIdleReturn&&!policy.reportActivity&&closed==1&&cleared==1&&s_count==0&&!s_actionsOpen);
 s_state=PH_VIEWING;s_count=1;coverGallery();assert(!std::strcmp(s_resumePath,"/videos/fixture.avi"));
 s_state=PH_SYNCING;exitApp();assert(runtimePolicy().allowIdleReturn&&!runtimePolicy().reportActivity);
}
'''
 (p/'test.cpp').write_text(code)
 subprocess.run(['c++','-std=c++17','-fsanitize=address,undefined','-I'+str(root/'include'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('PASS actual Photos policy: sync survives idle timeout; manual sleep/background uploads stay allowed; cover/exit release activity and preserve selection')
