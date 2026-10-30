"""Compile the production HTTP acknowledgement gate; reject unrelated 2xx bodies."""
from pathlib import Path
import tempfile,subprocess
s=(Path(__file__).resolve().parents[1]/'src/services/net_poll.cpp').read_text()
a=s.index('int postJson(');b=s.index('\n}\n',a)+2
source=s[a:b]
code=r'''
#include <string>
#include <cassert>
struct String:std::string{using std::string::string;String(const std::string&s):std::string(s){}bool startsWith(const char*s){return rfind(s,0)==0;}};
int status=200,length=0,reads=0,cleared=0;String reply;
namespace core{namespace secrets{struct Request{struct{char url[32]="https://example.test";char apiKey[8]="key";}backend;char token[8]="token";};Request backendRequest(){return {};}void setDeviceTokenFor(Request,const char*){++cleared;}}namespace device{const char*id(){return "test";}}}
struct WiFiClient{};struct WiFiClientSecure:WiFiClient{void setHandshakeTimeout(int){}};
namespace services{namespace tls{void useRootBundle(WiFiClientSecure&){}}}
struct HTTPClient{void setConnectTimeout(int){}void setTimeout(int){}bool begin(WiFiClient&,String){return true;}template<class T>void addHeader(const char*,T){}int POST(String&){return status;}int getSize(){return length;}String getString(){++reads;return reply;}void end(){}};
struct{int getFreeHeap(){return 100000;}}ESP;
struct{template<class...A>void printf(A...){}}Serial;
''' + source + r'''
int main(){String body="{}",expected="{\"ok\":true,\"batch\":\"0123456789abcdef\",\"count\":3}";
 reply=expected;length=reply.length();assert(postJson("/api/device/events",body,&expected)==200);
 reply[reply.length()-2]='2';assert(postJson("/api/device/events",body,&expected)==-1);
 reply=expected.substr(0,expected.length()-1);assert(postJson("/api/device/events",body,&expected)==-1);
 for(int size:{-1,0,999999}){length=size;reads=0;assert(postJson("/api/device/events",body,&expected)==-1);assert(reads==0);}
 status=503;assert(postJson("/api/device/events",body,&expected)==503);
 status=401;assert(postJson("/api/device/events",body,&expected)==401);assert(cleared==1);
 status=200;assert(postJson("/api/device/logs",body)==200);
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d);(p/'test.cpp').write_text(code)
 subprocess.run(['c++','-std=c++17','-fsanitize=address,undefined',str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('PASS production receipt gate: exact nonce/count, wrong receipt, truncation, size bound, HTTP failure and legacy compatibility')
