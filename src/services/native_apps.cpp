#include "services/native_apps.h"
#ifdef ORIGAMI_NATIVE_APPS
#include "config/native_apps.h"
#include <esp_elf.h>
#include <esp_heap_caps.h>
#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>
#include <stdio.h>
#include <atomic>
#include <string.h>
namespace services { namespace native_apps {
namespace {
#include "native_app_symbols.inc"
esp_elf_t module{};
enum class State { Idle, Preparing, Ready, Active, Retired };
std::atomic<State> state{State::Idle};
bool registered=false;
void destroy(){esp_elf_deinit(&module);memset(&module,0,sizeof(module));state.store(State::Idle,std::memory_order_release);}
int nibble(char c){return c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:-1;}
}
bool verify(const char* id,const char* name,const char* version,unsigned abi,
            const char* digest,const char* signature,const char* icon){
    if(abi!=KAMI_NATIVE_ABI_VERSION || !id||!name||!version||!digest||!signature || strlen(digest)!=64 || strlen(signature)!=512)return false;
    if(icon){if(strlen(icon)!=100)return false;for(unsigned i=0;i<100;++i)if(nibble(icon[i])<0)return false;}
    char message[384];const int n=icon
        ?snprintf(message,sizeof(message),"KAMI-NATIVE-2\n%s\n%s\n%s\n%u\n%s\n%s\n",id,name,version,abi,digest,icon)
        :snprintf(message,sizeof(message),"KAMI-NATIVE-1\n%s\n%s\n%s\n%u\n%s\n",id,name,version,abi,digest);
    if(n<0||size_t(n)>=sizeof(message))return false;
    uint8_t sig[256],hash[32];for(size_t i=0;i<sizeof(sig);++i){int a=nibble(signature[2*i]),b=nibble(signature[2*i+1]);if(a<0||b<0)return false;sig[i]=(a<<4)|b;}
    if(mbedtls_sha256_ret(reinterpret_cast<const uint8_t*>(message),n,hash,0))return false;
    mbedtls_pk_context key;mbedtls_pk_init(&key);
    const bool ok=mbedtls_pk_parse_public_key(&key,reinterpret_cast<const uint8_t*>(config::native_apps::publicKey),sizeof(config::native_apps::publicKey))==0 &&
        mbedtls_pk_verify(&key,MBEDTLS_MD_SHA256,hash,sizeof(hash),sig,sizeof(sig))==0;
    mbedtls_pk_free(&key);return ok;
}
void discardPrepared(){State expected=State::Ready;if(state.compare_exchange_strong(expected,State::Preparing))destroy();}
void retire(){State expected=State::Active;state.compare_exchange_strong(expected,State::Retired);}
void collect(){State expected=State::Retired;if(state.compare_exchange_strong(expected,State::Preparing))destroy();}
bool prepare(const uint8_t* bytes,size_t size){
    if(!bytes||!size||size>kMaxBytes)return false;
    State idle=State::Idle;if(!state.compare_exchange_strong(idle,State::Preparing))return false;
    if(!registered){if(esp_elf_register_symbol(kSymbols)){state=State::Idle;return false;}registered=true;}
    // KAPP need not align the ELF. Relocation only borrows this immutable copy.
    auto* aligned=static_cast<uint8_t*>(heap_caps_malloc(size,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
    if(!aligned){state=State::Idle;return false;}memcpy(aligned,bytes,size);
    esp_elf_init(&module);
    int status=esp_elf_relocate(&module,aligned,size);heap_caps_free(aligned);
    if(status){destroy();return false;}
    state.store(State::Ready,std::memory_order_release);return true;
}
bool activate(void*& descriptor){
    descriptor=nullptr;State ready=State::Ready;
    if(!state.compare_exchange_strong(ready,State::Active))return false;
    const int status=esp_elf_request(&module,0,0,reinterpret_cast<char**>(&descriptor));
    if(status||!descriptor){descriptor=nullptr;retire();return false;}return true;
}
} }
#endif
