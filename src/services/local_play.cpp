#include "services/local_play.h"
#include <cstring>
#ifdef KAMI_NATIVE_SIMULATOR
#include <cstdlib>
#include <sys/socket.h>
#include <netinet/in.h>
#else
#include "hal/hal_wifi.h"
#include <esp_system.h>
#include <lwip/sockets.h>
#endif
#include <fcntl.h>
#include <unistd.h>

namespace services { namespace local_play {
namespace {
constexpr uint16_t port=43964;
int fd=-1;
uint32_t active;
#ifdef KAMI_NATIVE_SIMULATOR
uint32_t currentStation(){return htonl(INADDR_LOOPBACK);}
uint32_t nonce(){return arc4random();}
#else
uint32_t currentStation(){return hal::wifi::getSTAIP().addr;}
uint32_t nonce(){return esp_random();}
#endif
uint32_t station;
}
void close(uint32_t owner) {
    if(owner && owner==active) { ::close(fd); fd=-1; active=0; }
}
uint32_t open() {
    if(active)return 0;
    station=currentStation();
    if(!station)return 0;
    int candidate=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
    if(candidate<0)return 0;
    sockaddr_in local{}; local.sin_family=AF_INET; local.sin_port=htons(port);
    int yes=1;
#ifdef KAMI_NATIVE_SIMULATOR
    // Multiple desktop players share one LAN interface and listen port.
    if(setsockopt(candidate,SOL_SOCKET,SO_REUSEPORT,&yes,sizeof(yes))) { ::close(candidate);return 0; }
#endif
    if(setsockopt(candidate,SOL_SOCKET,SO_BROADCAST,&yes,sizeof(yes)) ||
       fcntl(candidate,F_SETFL,O_NONBLOCK)<0 ||
       bind(candidate,reinterpret_cast<sockaddr*>(&local),sizeof(local))<0) {
        ::close(candidate); return 0;
    }
    fd=candidate;
    do {active=nonce();} while(!active);
    return active;
}
bool send(uint32_t owner,const Packet& packet) {
    if(!owner || owner!=active || currentStation()!=station)return false;
    sockaddr_in dest{};dest.sin_family=AF_INET;dest.sin_port=htons(port);dest.sin_addr.s_addr=INADDR_BROADCAST;
    return sendto(fd,packet,sizeof(packet),0,reinterpret_cast<sockaddr*>(&dest),sizeof(dest))==sizeof(packet);
}
bool receive(uint32_t owner,Packet& packet) {
    if(!owner || owner!=active)return false;
    // An extra byte distinguishes an oversized packet from an exact frame.
    uint8_t input[packetSize+1];sockaddr_in from{};socklen_t len=sizeof(from);
    if(recvfrom(fd,input,sizeof(input),0,reinterpret_cast<sockaddr*>(&from),&len)!=packetSize ||
       from.sin_port!=htons(port))return false;
    memcpy(packet,input,packetSize);return true;
}
}}
