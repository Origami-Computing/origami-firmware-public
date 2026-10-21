#ifdef ORIGAMI_DEV_SERIAL_INPUT
#include "tooling/morning_diagnostics.h"
#include "core/morning_quote_cache.h"
#include "apps/morning_routine_app.h"
#include "hal/hal_bus.h"
#include "hal/hal_sd.h"
#include "services/background_network.h"
#include "services/agent_coordination.h"
#include <WiFiClientSecure.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>

namespace tooling {
namespace {
bool morningRegression() {
  using namespace core::morning_quote_record;
  Record records[2]; bool valid[2]={false,false};
  bool cacheOk=replacementSlot(records,valid,20260930)==0;
  records[0].dayKey=20260930; valid[0]=true;
  cacheOk &= replacementSlot(records,valid,20261001)==1;
  records[1].dayKey=20261001; valid[1]=true;
  cacheOk &= replacementSlot(records,valid,20260930)==0;
  cacheOk &= replacementSlot(records,valid,20261001)==1;
  cacheOk &= replacementSlot(records,valid,20261002)==0;
  // No changes to the user's quote cache or alarm settings.
  Serial.printf("[morning-regression] cache-retention=%u\n",cacheOk);
  if(!services::background_network::suspendForPeripheralIo(5000)) return false;
  services::agent_coordination::TransportPause agentPause;
  if(!agentPause.acquire(5000)) {
    services::background_network::resumeAfterPeripheralIo();
    return false;
  }
  bool overlapOk=false;
  {
    services::agent_coordination::TransportPause nested;
    overlapOk=nested.acquire(0);
    nested.release();
    nested.release();
    overlapOk &= agentPause.held() && services::agent_coordination::suspendRequested();
  }
  Serial.printf("[morning-regression] agent-overlap=%u\n",overlapOk);
  hal::bus::lockSpi();
  char path[96];
  snprintf(path,sizeof(path),"/sd/morning_tls_probe_%lu.tmp",(unsigned long)millis());
  const int fd=::open(path,O_CREAT|O_EXCL|O_RDWR,0600);
  bool fileOk=fd>=0;
  if(fileOk) {
    // Exact failure trigger: stop, stop, then destructor (also stop). The old
    // Arduino cleanup closed fd 0 even though this client had no socket.
    bool sentinelOk=true;
    {
      WiFiClientSecure client;
      for(int i=0;i<3;++i) {
        client.stop();
        sentinelOk &= client.fd()==-1;
      }
    }
    errno=0;
    const char payload[]="TLS cleanup must not close a file";
    struct stat st{};
    fileOk=sentinelOk && ::write(fd,payload,sizeof(payload))==sizeof(payload) &&
        ::fsync(fd)==0 && ::fstat(fd,&st)==0 && st.st_size==sizeof(payload);
    const int error=errno;
    ::close(fd);
    const bool removed=::unlink(path)==0;
    Serial.printf("[morning-regression] tls-file fd=%d sentinel=%u alive=%u cleanup=%u errno=%d\n",
        fd,sentinelOk,fileOk,removed,error);
    fileOk &= removed;
  }
  hal::bus::unlockSpi();
  services::background_network::resumeAfterPeripheralIo();
  agentPause.release();
  return cacheOk && fileOk && overlapOk;
}
} // namespace

bool morningUiCommand(const char* command) {
  static services::agent_coordination::TransportPause diagnosticPause;
  if(strcmp(command,"self-test")==0 && !apps::morning_routine::isRecording()) return morningRegression();
  if(strcmp(command,"diag-pause-agent")==0) {
    const bool ok=diagnosticPause.acquire(8000);
    Serial.printf("[morning-diag] agent-paused=%u\n",ok);
    return ok;
  }
  if(strcmp(command,"diag-resume-agent")==0) {
    diagnosticPause.release();
    return true;
  }
  return apps::morning_routine::developmentUi(command);
}
} // namespace tooling
#endif
