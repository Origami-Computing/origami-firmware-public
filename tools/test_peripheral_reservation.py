"""Fault-inject reservation acquisition and verify rollback/release ownership."""
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    with tempfile.TemporaryDirectory() as directory:
        temp = Path(directory)
        def write(name, text):
            p = temp/name
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_text(text)
        write('services/background_network.h', '#include <stdint.h>\nnamespace services { namespace background_network { void requestPeripheralIo();bool peripheralIoReady();bool suspendForPeripheralIo(uint32_t); void resumeAfterPeripheralIo(); } }')
        write('main.cpp', r'''
#include "services/peripheral_reservation.h"
#include <cassert>
#include <string>
bool peripheralOk=true, agentOk=true;
std::string trace;
namespace services { namespace background_network {
bool suspendForPeripheralIo(uint32_t timeout) { assert(timeout==42); trace+='P'; return peripheralOk; }
void requestPeripheralIo(){trace+='R';}bool peripheralIoReady(){return peripheralOk;}
void resumeAfterPeripheralIo() { trace+='p'; }
} }
namespace services { namespace agent_coordination {
bool TransportPause::pollAcquire(){if(!held_){trace+='A';held_=true;}return agentOk;}
bool TransportPause::acquire(uint32_t timeout) {if(held_)return true; assert(timeout==42); trace+='A'; return held_=agentOk; }
void TransportPause::release() {if(!held_)return;held_=false; trace+='a'; }
} }
int main() {
 using Lease=services::background_network::PeripheralReservation;
 using Result=Lease::Result;
 { Lease lease; peripheralOk=false;
   assert(lease.acquire(true,42)==Result::PeripheralBusy);
   lease.release(); assert(trace=="P"); }
 trace.clear(); peripheralOk=true; agentOk=false;
 { Lease lease; assert(lease.acquire(true,42)==Result::AgentBusy);
   lease.release(); assert(trace=="PAp"); }
 trace.clear(); agentOk=true;
 { Lease lease; assert(lease.acquire(true,42)==Result::Acquired);
   assert(lease.acquire(true,42)==Result::Acquired); assert(trace=="PA");
   assert(lease.acquire(false,42)==Result::PeripheralBusy); assert(trace=="PA");
   lease.release(); lease.release(); assert(trace=="PApa"); }
 trace.clear();
 { Lease lease; assert(lease.acquire(false,42)==Result::Acquired); }
 assert(trace=="Pp"); // Destruction releases only the resource actually held.
 trace.clear();
 { Lease lease; assert(lease.acquire(true,42)==Result::Acquired); }
 assert(trace=="PApa"); // Existing resume ordering is preserved.
 trace.clear();peripheralOk=false;
 {Lease lease;assert(lease.pollAcquire(true)==Result::Pending && !lease.held());
  assert(lease.pollAcquire(true)==Result::Pending && trace=="R");
  assert(lease.acquire(true,42)==Result::PeripheralBusy);
  assert(lease.pollAcquire(false)==Result::PeripheralBusy);
 }assert(trace=="Rp");
 trace.clear();peripheralOk=true;agentOk=false;
 {Lease lease;assert(lease.pollAcquire(true)==Result::Pending && !lease.held());
  assert(lease.pollAcquire(true)==Result::Pending && trace=="RA");
  agentOk=true;assert(lease.pollAcquire(true)==Result::Acquired && lease.held());
  assert(lease.pollAcquire(true)==Result::Acquired && trace=="RA");
 }assert(trace=="RApa");
 trace.clear();agentOk=false;
 {Lease lease;assert(lease.pollAcquire(true)==Result::Pending);}
 assert(trace=="RApa");
}
''')
        subprocess.run([shutil.which('c++') or 'c++', '-std=c++11', '-I'+str(temp), '-I'+str(ROOT/'include'),
                        str(ROOT/'src/services/peripheral_reservation.cpp'), str(temp/'main.cpp'), '-o', str(temp/'check')], check=True)
        subprocess.run([str(temp/'check')], check=True)
    print('PASS reservation failures, rollback, idempotent acquisition/release and destructor cleanup')


if __name__ == '__main__':
    main()
