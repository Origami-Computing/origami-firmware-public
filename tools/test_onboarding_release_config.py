"""Compile channel defaults and encode the real setup QR for each release role."""
import json,os,shlex,subprocess,tempfile
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
CXX=shlex.split(os.environ.get('CXX','c++'))
SOURCE=r'''
#include "core/menu_config.h"
#include "assets/onboarding_website_qr.h"
#include "qrcodegen.h"
#include <cassert>
#include <cstdio>
#include <cstring>
int main(){
 const char* api=BACKEND_URL_PROD;const char* web=assets::onboarding_website::kUrl;
 assert(!WIFI_SSID[0]&&!WIFI_PASS[0]&&!AUTH_TOKEN[0]);
 if(core::release_policy::stagingBuild){
  assert(!strcmp(api,"https://kami2-staging.up.railway.app"));
  assert(!strcmp(web,"https://vivacious-courtesy-staging.up.railway.app/device/manage"));
 }else{
  assert(!strcmp(api,"https://api.kami.so"));
  assert(!strcmp(web,"https://kami.so/device/manage"));
 }
 uint8_t tmp[qrcodegen_BUFFER_LEN_FOR_VERSION(6)],qr[qrcodegen_BUFFER_LEN_FOR_VERSION(6)];
 assert(qrcodegen_encodeText(web,tmp,qr,qrcodegen_Ecc_LOW,2,6,qrcodegen_Mask_AUTO,true));
 const int size=qrcodegen_getSize(qr),module=core::release_policy::stagingBuild?165/(size+8):5;
 assert(module>=3&&size*module<=165);
 const int y0=core::release_policy::stagingBuild?(230-size*module)/2:52;
 assert(y0-4*module>=28);
 assert(y0+size*module+4*module<=(core::release_policy::stagingBuild?198:204));
 std::printf("%s -> %s; QR %d modules at %d px/module\n",api,web,size,module);
}
'''

def main():
 with tempfile.TemporaryDirectory() as folder:
  temp=Path(folder);src=temp/'check.cpp';src.write_text(SOURCE)
  for role,flags in [('custom',[]),('staging',['-DORIGAMI_STAGING_BUILD=1']),('production',['-DORIGAMI_PRODUCTION_BUILD=1'])]:
   binary=temp/(role+'.exe')
   subprocess.run(CXX+['-std=c++11','-I'+str(ROOT/'include'),'-I'+str(ROOT/'lib/qrcodegen/src'),*flags,'-x','c++',str(src),str(ROOT/'lib/qrcodegen/src/qrcodegen.c'),'-o',str(binary)],check=True)
   subprocess.run([str(binary)],check=True)
  bad=subprocess.run(CXX+['-std=c++11','-I'+str(ROOT/'include'),'-I'+str(ROOT/'lib/qrcodegen/src'),'-DORIGAMI_STAGING_BUILD=1','-DORIGAMI_PRODUCTION_BUILD=1','-fsyntax-only',str(src)],capture_output=True,text=True)
  assert bad.returncode and 'cannot target both' in bad.stderr
 print('PASS channel-specific API/setup URL, real QR capacity/quiet zone, empty credentials and conflicting-channel rejection')

if __name__=='__main__':main()
