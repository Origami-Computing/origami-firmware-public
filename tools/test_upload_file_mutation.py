"""Test actual queue/file mutation lifetime, subtree matching and persistence failures."""
from pathlib import Path
import subprocess,tempfile
ROOT=Path(__file__).resolve().parents[1]
s=(ROOT/'src/upload_queue.cpp').read_text();h=(ROOT/'include/upload_queue.h').read_text()
def block(text,marker):
 a=text.index(marker);b=text.index('{',a)+1;depth=1
 while depth:depth+=(text[b]=='{')-(text[b]=='}');b+=1
 return text[a:b]
code=r'''
#include <cassert>
#include <cstring>
#include <cstdio>
#include <cstdint>
#include <initializer_list>
bool locked=false,eraseOk=true;
namespace services {namespace video_upload {bool erase(const char*){return eraseOk;}}}
struct MutexGuard {bool taken=true;MutexGuard(int){assert(!locked);locked=true;}~MutexGuard(){locked=false;}};
enum QueueItemStatus:uint8_t{QSTATUS_PENDING,QSTATUS_UPLOADING,QSTATUS_COMPLETED,QSTATUS_FAILED};
'''+block(h,'struct QueueItem {')+';\nclass UploadQueue {public:\n'+block(h,'    enum class MutationResult')+';\n'+block(h,'    class FileMutation {')+r''';
 QueueItem items[20]{};QueueItem* m_items=items;int m_count=0,m_mutex=0,saves=0;bool m_initialized=true,saveOk=true;
 char m_inFlightPath[96]{},m_mutationPath[96]{};
 bool mutationBlocksUnlocked(const char*)const;
 MutationResult beginFileMutation(const char*,bool,int&);void endFileMutation();
 bool saveManifestUnlocked(){assert(locked);++saves;return saveOk;}
 void removeItemAtUnlocked(int index){assert(locked);for(int i=index;i<m_count-1;++i)m_items[i]=m_items[i+1];--m_count;}
 void add(const char* path,bool active=false){strcpy(items[m_count].filePath,path);items[m_count++].status=active?QSTATUS_UPLOADING:QSTATUS_PENDING;}
};
'''+ '\n'.join(block(s,m) for m in ['bool withinUploadPath(', 'bool forgetVideoSource(', 'bool UploadQueue::mutationBlocksUnlocked(', 'UploadQueue::MutationResult UploadQueue::beginFileMutation(', 'void UploadQueue::endFileMutation(', 'UploadQueue::FileMutation::FileMutation(', 'UploadQueue::FileMutation::~FileMutation('])+r'''
int main(){
 using M=UploadQueue::FileMutation;using R=UploadQueue::MutationResult;
 {UploadQueue q;q.add("/videos/VID_a.avi");M denied(q,"/videos/VID_a.avi",false);assert(denied.result()==R::Busy&&q.m_count==1&&!q.saves);}
 {UploadQueue q;q.add("/videos/VID_a.avi");q.add("/videos/VID_b.avi");q.add("/videos-other/VID_c.avi");
  {M deletion(q,"/VIDEOS/",true);assert(deletion.ready()&&deletion.removed()==2&&q.m_count==1&&q.saves==1&&!locked);
   assert(q.mutationBlocksUnlocked("/videos/VID_a.avi")&&!q.mutationBlocksUnlocked("/videos-other/VID_a.avi"));
   M second(q,"/elsewhere",false);assert(second.result()==R::Busy);}
  assert(!q.m_mutationPath[0]);
 }
 {UploadQueue q;q.add("/videos/VID_a.avi",true);M blocked(q,"/videos",true);assert(blocked.result()==R::Busy&&q.m_count==1&&!q.saves);}
 {UploadQueue q;strcpy(q.m_inFlightPath,"/videos/VID_a.avi");M blocked(q,"/videos",true);assert(blocked.result()==R::Busy&&!q.saves);}
 {UploadQueue q;q.add("/videos/VID_a.avi");q.saveOk=false;{M failed(q,"/videos/VID_a.avi",true);assert(failed.result()==R::StorageUnavailable&&failed.removed()==1&&!q.m_count&&!q.m_mutationPath[0]);}
  q.saveOk=true;M retry(q,"/videos/VID_a.avi",true);assert(retry.ready()&&q.saves==2);
 }
 {UploadQueue q;M overwrite(q,"/videos/test.avi",false);assert(overwrite.ready()&&q.saves==1&&q.mutationBlocksUnlocked("/VIDEOS/TEST.AVI")&&!q.mutationBlocksUnlocked("/videos/test.avi2"));}
 {UploadQueue q;M root(q,"/",true);assert(root.ready()&&q.mutationBlocksUnlocked("/videos/a.avi"));}
 for(const char* path:{static_cast<const char*>(nullptr),"","relative","/videos/../x","/videos/..","/videos/.","/videos/./x","/videos//x","/videos/\nname"}){
  UploadQueue q;M invalid(q,path,true);assert(invalid.result()==R::Invalid&&!q.saves&&!q.m_mutationPath[0]);
 }
 {UploadQueue q;q.m_initialized=false;M failed(q,"/videos/a.avi",true);assert(failed.result()==R::StorageUnavailable);}
 {UploadQueue q;eraseOk=false;M failed(q,"/videos/test.avi",false);assert(failed.result()==R::StorageUnavailable&&!q.m_mutationPath[0]);eraseOk=true;}
 assert(!locked);puts("PASS actual file mutation lease: pending/active sources, cancelled in-flight owner, subtree/case boundaries, failed persistence and release");
}
'''
with tempfile.TemporaryDirectory() as directory:
 p=Path(directory);(p/'main.cpp').write_text(code)
 subprocess.run(['c++','-std=c++11','-fsanitize=address,undefined',str(p/'main.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
