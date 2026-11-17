"""Exercise gallery wrap boundaries, including empty and large deltas."""
from pathlib import Path
import subprocess, tempfile
root=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory() as td:
 p=Path(td);source=p/'main.cpp'
 source.write_text(r'''#include "apps/photos_policy.h"
#include <cassert>
#include <climits>
int main(){using apps::photos::policy::moveIndex;
 assert(moveIndex(0,-1,203)==202);assert(moveIndex(202,1,203)==0);
 assert(moveIndex(0,-204,203)==202);assert(moveIndex(202,204,203)==0);
 for(int delta:{INT_MIN,-203,-1,0,1,203,INT_MAX}){
  assert(moveIndex(0,delta,0)==0);assert(moveIndex(0,delta,1)==0);
  int actual=moveIndex(202,delta,203);assert(actual>=0&&actual<203);
 }
}'''.replace('#include <climits>','#include <climits>\n#include <initializer_list>'))
 subprocess.run(['c++','-std=c++11','-fsanitize=address,undefined','-I'+str(root/'include'),str(source),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('PASS Photos wrap: first/last, multi-item deltas, empty/single library and integer limits under ASan/UBSan')
