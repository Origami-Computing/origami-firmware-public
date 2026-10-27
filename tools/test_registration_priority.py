"""Exercise the actual cross-worker onboarding priority mailbox, including wrap and newer intent."""
from pathlib import Path
import subprocess,tempfile
ROOT=Path(__file__).resolve().parents[1]
s=(ROOT/'src/services/net_poll.cpp').read_text()
def fn(signature):
 a=s.index(signature);i=s.index('{',a)+1;depth=1
 while depth:depth+=(s[i]=='{')-(s[i]=='}');i+=1
 return s[a:i]
code='''#include <cstdint>
#include <cassert>
#include <cstdio>
uint32_t now=0,s_onboardingPriorityAt=0;
bool s_onboardingPriority=false,s_onboardingRegistration=false;
int s_healthMux;
#define portENTER_CRITICAL(x) ((void)0)
#define portEXIT_CRITICAL(x) ((void)0)
uint32_t millis(){return now;}
'''+ '\n'.join(fn(x) for x in ['bool onboardingRequestPending()', 'void publishRegistrationPriority(', 'void requestOnboardingRegistration()', 'bool onboardingRegistrationPriority()'])+'''
int main(){
 assert(!onboardingRegistrationPriority());assert(!onboardingRequestPending());
 now=UINT32_MAX-100;requestOnboardingRegistration();assert(onboardingRegistrationPriority());assert(onboardingRequestPending());
 s_onboardingRegistration=false;publishRegistrationPriority(true);now+=44999;assert(onboardingRegistrationPriority());
 ++now;assert(!onboardingRegistrationPriority());
 requestOnboardingRegistration();assert(onboardingRegistrationPriority());assert(onboardingRequestPending());
 // Completion of an older attempt must not erase a newer pending scan.
 publishRegistrationPriority(false);assert(onboardingRegistrationPriority());
 s_onboardingRegistration=false;publishRegistrationPriority(false);assert(!onboardingRegistrationPriority());
 puts("PASS priority expiry, rollover, completion and newer pending scan preservation");
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d);(p/'test.cpp').write_text(code)
 subprocess.run(['c++','-std=c++11',str(p/'test.cpp'),'-o',str(p/'check')],check=True)
 subprocess.run([str(p/'check')],check=True)
