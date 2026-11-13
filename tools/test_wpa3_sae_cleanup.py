"""Exercise the actual pinned SSWU cleanup, including every allocation/operation failure.
Crypto arithmetic is mocked: device WPA3 joins validate the real arithmetic/SDK link.
"""
from pathlib import Path
import hashlib,json,subprocess,tempfile
ROOT=Path(__file__).resolve().parents[1]
source=(ROOT/'vendor/wpa3-sae/src/common/sae.c').read_text()
original=source.replace('const struct crypto_bignum *b, *prime;\n\tstruct crypto_bignum *a;', 'const struct crypto_bignum *a, *b, *prime;').replace('if (!a || !u2 ||', 'if (!u2 ||').replace('fail:\n\tcrypto_bignum_deinit(a, 0);\n\tcrypto_bignum_deinit(tmp, 0);','fail:\n\tcrypto_bignum_deinit(tmp, 0);')
manifest=json.loads((ROOT/'vendor/wpa3-sae/upstream-sha256.json').read_text())
assert hashlib.sha256(original.encode()).hexdigest()==manifest['files']['src/common/sae.c'], 'Unexpected upstream SAE changes'
for file,digest in manifest['files'].items():
 if file!='src/common/sae.c':assert hashlib.sha256((ROOT/'vendor/wpa3-sae'/file).read_bytes().replace(b'\r\n',b'\n')).hexdigest()==digest,file
start=source.index('static struct crypto_ec_point * sswu(')
end=source.index('static int sae_pwd_seed',start)
function=source[start:end]
preamble=r"""
#include <cassert>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <set>
using u8=uint8_t;
#define SAE_MAX_ECC_PRIME_LEN 66
#define MSG_DEBUG 0
struct crypto_bignum{int id;};struct crypto_ec{};struct crypto_ec_point{};
static crypto_bignum prime{0},b{0},input{0};static crypto_ec_point point;
static std::set<crypto_bignum*> live;
static int allocations,operations,failAllocation,failOperation;
static crypto_bignum* alloc(){if(++allocations==failAllocation)return nullptr;auto* p=new crypto_bignum{allocations};live.insert(p);return p;}
static crypto_bignum* crypto_bignum_init(){return alloc();}
static crypto_bignum* crypto_bignum_init_uint(unsigned){return alloc();}
static crypto_bignum* crypto_bignum_init_set(const u8*,size_t){return alloc();}
static void crypto_bignum_deinit(crypto_bignum* p,int){if(!p)return;assert(live.erase(p)==1);delete p;}
static int sswu_curve_param(int,int* z){*z=-10;return 0;}
static const crypto_bignum* crypto_ec_get_prime(crypto_ec*){return &prime;}
static const crypto_bignum* crypto_ec_get_b(crypto_ec*){return &b;}
static size_t crypto_ec_prime_len(crypto_ec*){return 32;}
static void valid(const crypto_bignum* p){assert(p&&(p==&prime||p==&b||p==&input||live.count(const_cast<crypto_bignum*>(p))));}
static int op(){return ++operations==failOperation?-1:0;}
#define OP3(name) static int name(const crypto_bignum* a,const crypto_bignum* b,crypto_bignum* c){valid(a);valid(b);valid(c);return op();}
#define OP4(name) static int name(const crypto_bignum* a,const crypto_bignum* b,const crypto_bignum* c,crypto_bignum* d){valid(a);valid(b);valid(c);valid(d);return op();}
OP3(crypto_bignum_sub) OP3(crypto_bignum_add) OP3(crypto_bignum_mod)
OP3(crypto_bignum_sqrmod) OP3(crypto_bignum_inverse)
OP4(crypto_bignum_mulmod) OP4(crypto_bignum_exptmod)
static int crypto_bignum_rshift(const crypto_bignum* a,int,crypto_bignum* b){valid(a);valid(b);return op();}
static int crypto_bignum_to_bin(const crypto_bignum* a,u8* out,size_t n,size_t){valid(a);memset(out,0,n);out[31]=3;return op();}
static int crypto_bignum_is_zero(const crypto_bignum* a){valid(a);return 0;}
static int crypto_bignum_is_one(const crypto_bignum* a){valid(a);return 1;}
static unsigned const_time_eq(unsigned a,unsigned b){return a==b?~0u:0;}
static void const_time_select_bin(unsigned c,const u8* a,const u8* b,size_t n,u8* out){memcpy(out,c?a:b,n);}
static void debug_print_bignum(const char*,const crypto_bignum*,size_t){}
static void wpa_printf(int,const char*,...){}
static void wpa_hexdump_key(int,const char*,const void*,size_t){}
static void forced_memzero(void* p,size_t n){memset(p,0,n);}
static crypto_ec_point* crypto_ec_point_from_bin(crypto_ec*,const u8*){return op()?nullptr:&point;}
static void reset(int a=0,int o=0){for(auto* p:live)delete p;live.clear();allocations=operations=0;failAllocation=a;failOperation=o;}
"""
main=r"""
int main(){
 crypto_ec ec;reset();assert(sswu(&ec,19,&input)==&point);
 const int totalAllocations=allocations,totalOperations=operations;
#ifdef BASELINE
 assert(live.size()==1&&(*live.begin())->id==1);reset();
 puts("PASS reproduced upstream leak: coefficient allocation survives SSWU cleanup");
#else
 assert(live.empty());
 for(int i=1;i<=totalAllocations;++i){reset(i);assert(sswu(&ec,19,&input)==nullptr);assert(live.empty());}
 for(int i=1;i<=totalOperations;++i){reset(0,i);assert(sswu(&ec,19,&input)==nullptr);assert(live.empty());}
 for(int i=0;i<100;++i){reset();assert(sswu(&ec,19,&input)==&point);assert(live.empty());}
 printf("PASS corrected SSWU: 100 successes, %d allocation failures, %d operation failures; zero live allocations\n",totalAllocations,totalOperations);
#endif
}
"""
with tempfile.TemporaryDirectory() as temp:
 folder=Path(temp)
 for baseline in (True,False):
  code=preamble+(original[start:original.index('static int sae_pwd_seed',start)] if baseline else function)+main
  cpp=folder/'check.cpp';exe=folder/'check';cpp.write_text(code)
  cmd=['c++','-std=c++17','-fsanitize=address,undefined','-fno-omit-frame-pointer',str(cpp),'-o',str(exe)]
  if baseline:cmd.append('-DBASELINE')
  subprocess.run(cmd,check=True);subprocess.run([str(exe)],check=True)
print('PASS upstream hashes: only coefficient ownership, allocation guard and paired cleanup differ')
