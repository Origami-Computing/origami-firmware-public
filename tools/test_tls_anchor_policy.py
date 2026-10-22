#!/usr/bin/env python3
"""Exercise the actual bundle callback with controlled certificate identities."""
from pathlib import Path
import subprocess, tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/services/tls_trust.cpp').read_text()
s=s[s.index('int verifyAgainstBundle('):s.index('\n}  // namespace')]
code='''#include <cstdint>
#include <cassert>
constexpr uint32_t MBEDTLS_X509_BADCERT_BAD_MD=1, MBEDTLS_X509_BADCERT_NOT_TRUSTED=2;
struct mbedtls_x509_crt { int issuer_raw, subject_raw; };
uint8_t root=1; bool exactRoot=false, signature=true; int signatureChecks=0;
const uint8_t* findRoot(int name){return name ? &root : nullptr;}
bool signedByRoot(mbedtls_x509_crt*,const uint8_t*){++signatureChecks;return signature;}
bool isRoot(mbedtls_x509_crt*,const uint8_t*){return exactRoot;}
'''+s+'''
int main(){
 mbedtls_x509_crt child{1,0}; uint32_t flags=3;
 verifyAgainstBundle(nullptr,&child,0,&flags); assert(flags==3 && signatureChecks==0);
 flags=2; verifyAgainstBundle(nullptr,&child,0,&flags); assert(flags==0);
 signature=false; flags=2; verifyAgainstBundle(nullptr,&child,0,&flags); assert(flags==2);
 mbedtls_x509_crt self{0,1}; exactRoot=true; flags=3;
 verifyAgainstBundle(nullptr,&self,0,&flags); assert(flags==0);
 for(uint32_t other: {4u,8u,16u}) {flags=3|other; verifyAgainstBundle(nullptr,&self,0,&flags);assert(flags==(3|other));}
 exactRoot=false; flags=3; verifyAgainstBundle(nullptr,&self,0,&flags);assert(flags==3);
}
'''
code='#include <initializer_list>\n'+code
with tempfile.TemporaryDirectory() as d:
 p=Path(d);(p/'test.cpp').write_text(code)
 subprocess.run(['c++','-std=c++17','-fsanitize=address,undefined',str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('PASS weak child signatures rejected; exact roots accepted; other verification flags preserved')
