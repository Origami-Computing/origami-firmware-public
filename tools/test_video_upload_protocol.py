"""Check bounded device-video admission/receipt and TUS destination validation.

Compiles production parsing with pinned ArduinoJson and sanitizers; no network,
credentials, SD media or live backend changes.
"""
from pathlib import Path
import argparse, os, subprocess, tempfile
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument("--backend-fixture",type=Path,help="Synthetic responses exported by backend device-video-uploads.test.ts")
args=parser.parse_args()
ROOT=Path(__file__).resolve().parents[1]
JSON=Path(os.environ.get('ARDUINOJSON_SRC',ROOT/'.pio/libdeps/esp32-s3-menu-n16r8/ArduinoJson/src'))
code=r'''
#include "core/video_upload_protocol.h"
#include <cassert>
#include <string>
#include <cstdio>
#include <memory>
#include <fstream>
#include <iterator>
using namespace core::video_upload;
const char* session="7245f6c7-4546-8dcf-becc-e276c07394a3";
const char* object="device-video/11111111-1111-4111-8111-111111111111/7245f6c7-4546-8dcf-becc-e276c07394a3.avi";
const char* endpoint="https://project.storage.supabase.co/storage/v1/upload/resumable/sign";
Identity identity;
void base(JsonDocument& d) {
 d.clear();d["name"]=identity.name;d["bytes"]=identity.bytes;d["sha256"]=identity.sha256;
 d["uploaded"]=false;d["session_id"]=session;d["state"]="pending";d["generation"]=0;
}
void admission(JsonDocument& d) {
 base(d);auto u=d["upload"].to<JsonObject>();u["endpoint"]=endpoint;u["bucket"]="media";
 u["object"]=object;u["token"]="synthetic.signed.token";u["chunk_bytes"]=kChunkBytes;u["content_type"]="video/x-msvideo";
}
bool tus(const std::string& wire,TusReply& reply) {
 size_t at=0;
 return readTusReply([&]() {return at<wire.size()?int(static_cast<unsigned char>(wire[at++])):-1;},reply);
}
void checkTus() {
 TusReply r;char location[512];uint32_t offset=123;
 const std::string version="Tus-Resumable: 1.0.0\r\n";
 const std::string created="HTTP/1.1 201 Created\r\n"+version+"Location: /storage/v1/upload/resumable/sign/id\r\n\r\n";
 assert(tus(created,r)&&createdResource(r,endpoint,100,location,sizeof(location)));
 for(size_t n=0;n<created.size();++n)assert(!tus(created.substr(0,n),r));
 assert(tus("HTTP/1.0 201\r\n"+version+"Location: https://evil.invalid/storage/v1/upload/resumable/sign/id\r\n\r\n",r));
 assert(!createdResource(r,endpoint,100,location,sizeof(location)));
 assert(tus("HTTP/1.1 200 OK\r\n"+version+"Upload-Offset: 50\r\nUpload-Length: 100\r\n\r\n",r));
 assert(resumeOffset(r,100,offset)&&offset==50);
 assert(!resumeOffset(r,99,offset));
 r.offset=101;assert(!resumeOffset(r,100,offset));
 r.offset=50;r.hasLength=false;assert(!resumeOffset(r,100,offset));
 assert(tus("HTTP/1.1 204 No Content\r\n"+version+"Upload-Offset: 6291456\r\nContent-Length: 0\r\n\r\n",r));
 assert(acceptedChunk(r,kChunkBytes+100,0,kChunkBytes));
 r.offset--;assert(!acceptedChunk(r,kChunkBytes+100,0,kChunkBytes));
 r.offset=kChunkBytes+100;assert(acceptedChunk(r,kChunkBytes+100,kChunkBytes,100));
 assert(!acceptedChunk(r,kChunkBytes+101,kChunkBytes,100));
 assert(!acceptedChunk(r,kChunkBytes+100,kChunkBytes,0));
 r.contentLength=1;assert(!acceptedChunk(r,kChunkBytes+100,kChunkBytes,100));
 r.contentLength=0;r.chunked=true;assert(!acceptedChunk(r,kChunkBytes+100,kChunkBytes,100));
 assert(tus("HTTP/1.1 204 No Content\r\nUpload-Offset: 100\r\nUpload-Length: 100\r\n\r\n",r));
 assert(!resumeOffset(r,100,offset)&&!acceptedChunk(r,100,0,100));
 // Header parsing alone never authenticates an operation's expected status.
 assert(tus("HTTP/1.1 409 Conflict\r\n"+version+"Upload-Offset: 100\r\nUpload-Length: 100\r\n\r\n",r));
 assert(!resumeOffset(r,100,offset)&&!acceptedChunk(r,100,0,100));
 for(const char* fields:{"Upload-Offset: 268435457\r\n","Upload-Length: -1\r\n",
   "Upload-Offset: 999999999999999999999999\r\n","Content-Length: 4294967296\r\n",
   "Upload-Offset: 1\r\nupload-offset: 1\r\n","Tus-Resumable: 1.0.0\r\n",
   "Location: /a\r\nLocation: /b\r\n","Content-Length: 0\r\nTransfer-Encoding: chunked\r\n",
   "Transfer-Encoding: chunked\r\nContent-Length: 0\r\n","Transfer-Encoding: gzip\r\n",
   "Bad Header: value\r\n"," folded: value\r\n","X: value\n"})
  assert(!tus("HTTP/1.1 204 No Content\r\n"+version+fields+"\r\n",r));
 for(const char* status:{"HTTP/2 204","HTTP/1.1 2040","HTTP/1.1 20","HTTP/1.1 600","HTTP/1.1 099"})
  assert(!tus(std::string(status)+"\r\n\r\n",r));
 assert(!tus("HTTP/1.1 200 OK\r\nX: "+std::string(768,'x')+"\r\n\r\n",r));
 std::string large="HTTP/1.1 200 OK\r\n";
 for(int i=0;i<100;++i)large+="X: "+std::string(100,'x')+"\r\n";
 assert(!tus(large+"\r\n",r));
 const char nulHeader[]="HTTP/1.1 200 OK\r\nX: a\0b\r\n\r\n";
 assert(!tus(std::string(nulHeader,sizeof(nulHeader)-1),r));
 puts("PASS bounded TUS headers, interrupted replies, offsets, chunk acknowledgements and destination fencing");
}
int main(int argc,char** argv) {
 checkTus();
 strcpy(identity.name,"VID_test.avi");identity.bytes=10000000;
 memset(identity.sha256,'a',64);identity.sha256[64]=0;assert(validIdentity(identity));
 JsonDocument d;auto output=std::unique_ptr<Response>(new Response);
 admission(d);assert(parseResponse(d,identity,*output)&&output->hasUpload&&output->state==State::Pending);
 assert(parseResponse(d,identity,*output,session,0));
 assert(!parseResponse(d,identity,*output,"11111111-1111-4111-8111-111111111111",0));
 assert(!parseResponse(d,identity,*output,session,1));
 assert(!parseResponse(d,identity,*output,session,-2));
 assert(!strcmp(output->endpoint,endpoint)&&!strcmp(output->object,object)&&!strcmp(output->session,session));
 for(const char* state:{"pending","verifying","failed"}) {
  base(d);d["state"]=state;assert(parseResponse(d,identity,*output)&&!output->hasUpload&&!output->receipt[0]);
  d["uploaded"]=true;assert(!parseResponse(d,identity,*output));
 }
 base(d);d["state"]="complete";d["uploaded"]=true;d["kind"]="video";d["id"]=session;
 assert(parseResponse(d,identity,*output)&&output->state==State::Complete&&!strcmp(output->receipt,session));
 d["upload"]["token"]="forbidden";assert(!parseResponse(d,identity,*output));
 for(const char* field:{"name","bytes","sha256","session_id","state","generation","uploaded"}) {
  admission(d);d.remove(field);assert(!parseResponse(d,identity,*output));
 }
 for(const char* field:{"endpoint","bucket","object","token","chunk_bytes","content_type"}) {
  admission(d);d["upload"].remove(field);assert(!parseResponse(d,identity,*output));
 }
 for(const char* value:{"https://host/storage/v1/upload/resumable","","bad","../VID_test.avi","VID_test.avi\r\nX:bad"}) {
  admission(d);d["name"]=value;assert(!parseResponse(d,identity,*output));
 }
 for(int n:{-1,0,9999999,10000001}) {admission(d);d["bytes"]=n;assert(!parseResponse(d,identity,*output));}
 for(int n:{-1,4,256}) {admission(d);d["generation"]=n;assert(!parseResponse(d,identity,*output));}
 admission(d);d["generation"]=1.5;assert(!parseResponse(d,identity,*output));
 admission(d);d["uploaded"]="false";assert(!parseResponse(d,identity,*output));
 admission(d);d["sha256"]=std::string(64,'b');assert(!parseResponse(d,identity,*output));
 for(int generation=1;generation<=3;++generation) {
  admission(d);d["generation"]=generation;assert(!parseResponse(d,identity,*output));
  d["upload"]["object"]=std::string(object).substr(0,strlen(object)-4)+"-r"+std::to_string(generation)+".avi";
  assert(parseResponse(d,identity,*output)&&output->generation==generation);
 }
 for(const char* value:{"https://user@host/storage/v1/upload/resumable/sign","http://project.storage.supabase.co/storage/v1/upload/resumable/sign",
  "https://host:0/storage/v1/upload/resumable/sign","https://host:65536/storage/v1/upload/resumable/sign",
  "https://host/storage/v1/upload/resumable/sign?secret=x","https://host/storage/v1/upload/resumable/sign#x",
  "https://host/storage/v1/upload/resumable/sign/","https://host/other","https://host\r\nX:bad/storage/v1/upload/resumable/sign"}) {
  admission(d);d["upload"]["endpoint"]=value;assert(!parseResponse(d,identity,*output));
 }
 for(const char* value:{"", "../other", "device-video/11111111-1111-4111-8111-111111111111/other.avi"}) {
  admission(d);d["upload"]["object"]=value;assert(!parseResponse(d,identity,*output));
 }
 for(const char* value:{"","token\r\nAuthorization:bad","token with spaces"}) {
  admission(d);d["upload"]["token"]=value;assert(!parseResponse(d,identity,*output));
 }
 admission(d);d["upload"]["token"]=std::string(2048,'x');assert(!parseResponse(d,identity,*output));
 // Embedded NUL is not an exact metadata match or a safe header value.
 admission(d);std::string name(identity.name);name+='\0';name+="other";d["name"]=name;assert(!parseResponse(d,identity,*output));
 admission(d);d["upload"]["token"]=std::string("good\0evil",9);assert(!parseResponse(d,identity,*output));
 char location[512];
 const std::string path="/storage/v1/upload/resumable/sign/resource-id";
 assert(resumeLocation(endpoint,path.c_str(),location,sizeof(location)));
 assert(std::string(location)=="https://project.storage.supabase.co"+path);
 assert(resumeLocation(endpoint,(std::string(endpoint)+"/id").c_str(),location,sizeof(location)));
 assert(resumeLocation(endpoint,("https://PROJECT.storage.supabase.co:443"+path).c_str(),location,sizeof(location)));
 for(const char* bad:{"/storage/v1/upload/resumable/id","https://evil.invalid/storage/v1/upload/resumable/sign/id","https://project.storage.supabase.co:444/storage/v1/upload/resumable/sign/id",
  "//evil.invalid/storage/v1/upload/resumable/sign/id","http://project.storage.supabase.co/storage/v1/upload/resumable/sign/id",
  "/storage/v1/upload/resumable/sign","/storage/v1/upload/resumable/sign/","/storage/v1/upload/resumable/sign-fake/id",
  "/storage/v1/upload/resumable/sign/../object","/storage/v1/upload/resumable/sign/%2e%2e/object",
  "/storage/v1/upload/resumable/sign/id?token=x","/storage/v1/upload/resumable/sign/id#fragment","/storage/v1/upload/resumable/sign/id\r\nX:bad"})
  assert(!resumeLocation(endpoint,bad,location,sizeof(location)));
 assert(!resumeLocation(endpoint,path.c_str(),location,10));
 Identity invalid=identity;memset(invalid.name,'x',sizeof(invalid.name));assert(!validIdentity(invalid));
 invalid=identity;invalid.bytes=kMaximumBytes+1;assert(!validIdentity(invalid));
 invalid=identity;memset(invalid.sha256,'a',sizeof(invalid.sha256));assert(!validIdentity(invalid));
 if(argc==2) {
  std::ifstream input(argv[1]);assert(input.good());
  const std::string json((std::istreambuf_iterator<char>(input)),{});
  JsonDocument fixture;assert(!deserializeJson(fixture,json));
  const std::string name=fixture["identity"]["name"].as<std::string>();
  const std::string hash=fixture["identity"]["sha256"].as<std::string>();
  assert(name.size()<sizeof(identity.name)&&hash.size()==64);
  strcpy(identity.name,name.c_str());strcpy(identity.sha256,hash.c_str());
  identity.bytes=fixture["identity"]["bytes"].as<uint32_t>();assert(validIdentity(identity));
  unsigned count=0;
  for(JsonVariantConst sample:fixture["responses"].as<JsonArrayConst>()) {
   assert(parseResponse(sample["body"],identity,*output));
   const char* states[]={"pending","verifying","complete","failed"};
   assert(core::video_upload::detail::equal(sample["state"],states[unsigned(output->state)]));
   assert(output->hasUpload==sample["hasUpload"].as<bool>());++count;
  }
  assert(count==7);puts("PASS seven actual backend route responses across admission/status/recovery/completion");
 }
 puts("PASS video metadata/receipt binding, generation/object matching, bounded credentials and same-origin resume URLs");
}
'''
with tempfile.TemporaryDirectory() as directory:
    p=Path(directory);(p/'test.cpp').write_text(code)
    subprocess.run(['c++','-std=c++11','-fsanitize=address,undefined','-I'+str(ROOT/'include'),'-I'+str(JSON),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')]+([str(args.backend_fixture)] if args.backend_fixture else []),check=True)
