"""Compare actual old/new Agent URL/header parsing over owned configuration."""
from pathlib import Path
import hashlib,json,re,subprocess,tempfile
ROOT=Path(__file__).resolve().parents[1]
fixture=ROOT/'test/fixtures/backend_parser'
old=(fixture/'parser.cpp').read_text()
assert hashlib.sha256(old.encode()).hexdigest()==json.loads((fixture/'manifest.json').read_text())['sha256']
source=(ROOT/'src/services/agent_transport.cpp').read_text()
a=source.index('bool parseBackendUrl(');b=source.index('{',a)+1;depth=1
while depth:depth+=(source[b]=='{')-(source[b]=='}');b+=1
new=source[a:b]
declarations='\n'.join(re.findall(r'^static (?:char|uint16_t|bool)\s+s_ws(?:Host|Path|Headers|Port|Secure)[^\n]*',source,re.M))
assert declarations.count(';')==5
code='''#include "core/secrets.h"
#include "services/agent_transport_config.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
core::secrets::BackendConfig config;
namespace core {namespace secrets {
const char* backendUrl(){return config.url;}
const char* apiKey(){return config.apiKey;}
} namespace device {const char* id(){return "synthetic-device";}}}
struct Result {bool valid,secure;uint16_t port;std::string host,path,headers;
 bool operator==(const Result& r)const{return valid==r.valid&&secure==r.secure&&port==r.port&&host==r.host&&path==r.path&&headers==r.headers;}};
'''
for name,body,call in [('baseline',old,'parseBackendUrl()'),('candidate',new,'parseBackendUrl(config)')]:
    code+='namespace '+name+'{\n'+declarations+'\n'+body+'\nResult run(){\n'
    code+='s_wsHost[0]=s_wsPath[0]=s_wsHeaders[0]=0;s_wsSecure=false;s_wsPort=0;\n'
    code+='const bool valid='+call+';return {valid,s_wsSecure,s_wsPort,s_wsHost,s_wsPath,s_wsHeaders};}\n}\n'
code+='''int main(){
 std::vector<std::string> urls={"","http://","https://","ftp://one.invalid","http://one.invalid","https://one.invalid/","http://one.invalid:8794/base/","http://one.invalid:0/","http://one.invalid:65536/","http://one.invalid:abc/","http://[::1]:80/"};
 for(int n=1;n<=190;++n){urls.push_back("http://"+std::string(n,'h')+"/base");urls.push_back("https://host/"+std::string(n,'p'));}
 unsigned count=0;
 for(const auto& url:urls)for(int length:{0,1,32,128,190,191}){
  snprintf(config.url,sizeof(config.url),"%s",url.c_str());
  const std::string key(length,'k');snprintf(config.apiKey,sizeof(config.apiKey),"%s",key.c_str());
  assert(baseline::run()==candidate::run());++count;
 }
 printf("PASS %u identical actual Agent parser/header cases\\n",count);
}
'''
with tempfile.TemporaryDirectory() as directory:
    p=Path(directory);(p/'test.cpp').write_text(code)
    subprocess.run(['c++','-std=c++11','-fsanitize=address,undefined','-I'+str(ROOT/'include'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
