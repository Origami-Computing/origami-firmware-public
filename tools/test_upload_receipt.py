"""Exercise production receipt framing/JSON with bounded, hostile HTTP responses.

No network or device writes. Uses the pinned ArduinoJson dependency and sanitizers.
"""
from pathlib import Path
import os, subprocess, tempfile
ROOT=Path(__file__).resolve().parents[1]
json_src=Path(os.environ.get('ARDUINOJSON_SRC',ROOT/'.pio/libdeps/esp32-s3-menu-n16r8/ArduinoJson/src'))
if not (json_src/'ArduinoJson.h').exists():
    raise SystemExit('Install the project ArduinoJson dependency, or set ARDUINOJSON_SRC (7.4.3)')
code=r'''
#include "core/upload_receipt.h"
#include <cassert>
#include <cstdio>
#include <string>
#include <vector>
using namespace core::uploads;
const std::string receipt=R"({"uploaded":true,"id":"7245f6c7-4546-8dcf-becc-e276c07394a3","kind":"video","duplicate":true})";
bool accept(const std::string& wire) {
 size_t pos=0,length=0;char body[1024];
 auto read=[&]()->int{return pos<wire.size()?static_cast<unsigned char>(wire[pos++]):-1;};
 return readReceiptBody(read,body,sizeof(body),length) && validReceipt(body,length,"video");
}
std::string framed(const std::string& body) {
 return "Content-Type: application/json\r\nContent-Length: "+std::to_string(body.size())+"\r\n\r\n"+body;
}
int main() {
 std::string wire=framed(receipt);assert(accept(wire));
 for(size_t i=0;i<wire.size();++i)assert(!accept(wire.substr(0,i)));
 char size[24];snprintf(size,sizeof(size),"%zx",receipt.size());
 std::string chunked="transfer-encoding: chunked\r\n\r\n"+std::string(size)+"\r\n"+receipt+"\r\n0\r\n\r\n";
 assert(accept(chunked));
 for(size_t i=0;i<chunked.size();++i)assert(!accept(chunked.substr(0,i)));
 std::string small="Transfer-Encoding: chunked\r\n\r\n";
 for(char c:receipt)small+="1\r\n"+std::string(1,c)+"\r\n";
 assert(accept(small+"0\r\nX-Trace: ignored\r\n\r\n"));
 for(auto body:{"{}","{\"uploaded\":true}","{\"uploaded\":\"true\"}","null","[]"})assert(!accept(framed(body)));
 for(const std::string& bad:{receipt+"garbage",receipt+receipt,receipt+std::string(1,'\0')})assert(!accept(framed(bad)));
 assert(accept(framed(receipt+" \t\r\n")));
 for(auto pair:std::vector<std::pair<std::string,std::string>>{{"true","false"},{"true","1"},{"video","audio"},{"7245f6c7","zzzzzzzz"},{"7245f6c7-","7245f6c7_"}}) {
  auto body=receipt;body.replace(body.find(pair.first),pair.first.size(),pair.second);assert(!accept(framed(body)));
 }
 for(auto headers:{"Content-Length: -1\r\n", "Content-Length: 99999999999999999999999\r\n",
     "Content-Length: 1025\r\n", "Content-Length: 0\r\nContent-Length: 0\r\n",
     "Content-Length: 0\r\nTransfer-Encoding: chunked\r\n", "Transfer-Encoding: gzip\r\n",
     "Content-Encoding: gzip\r\n", "Content-Length : 100\r\n", "broken\r\n", "Content-Length: 100\n"})assert(!accept(std::string(headers)+"\r\n"+receipt));
 assert(!accept("\r\n"+receipt));
 assert(!accept("X: "+std::string(256,'a')+"\r\n"+wire));
 assert(!accept("Transfer-Encoding: chunked\r\n\r\nFFFFFFFFFFFFFFFFFFFFFFFF\r\n"));
 assert(!accept("Transfer-Encoding: chunked\r\n\r\n0\r\n\r\n"));
 std::string headers;for(int i=0;i<1500;++i)headers+="X:a\r\n";assert(!accept(headers+wire));
 // Arbitrary bytes must remain within the caller's scratch and header budget.
 unsigned random=1234567;
 for(int n=0;n<5000;++n){std::string bytes;for(int j=0;j<n%2048;++j){random=random*1664525+1013904223;bytes+=char(random>>24);}accept(bytes);}
 puts("PASS durable receipt: truncation at every byte, chunk framing, bounds, typed receipt identity, wrong kind, trailing data and hostile input");
}
'''
with tempfile.TemporaryDirectory() as directory:
    path=Path(directory);(path/'test.cpp').write_text(code)
    subprocess.run(['c++','-std=c++17','-fsanitize=address,undefined','-I'+str(ROOT/'include'),'-I'+str(json_src),str(path/'test.cpp'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
